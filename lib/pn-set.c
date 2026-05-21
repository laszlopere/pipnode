/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * This file is part of Pipnode.  Pipnode is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License version 3, with the additional permission described in
 * LICENSE.PLUGIN-EXCEPTION, as published by the Free Software Foundation.
 *
 * Pipnode is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; see the GNU General Public License for more details.  You
 * should have received a copy of the license in the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-set.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Compiled assignment                                                */
/*                                                                     */
/*  An assignment's literal is held as the JsonNode parsed straight    */
/*  from the user's "props" string.  The literal's value type drives   */
/*  how the corresponding data-bag member is written at receive time,  */
/*  so the on-disk representation does not need a separate type tag.   */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar    *path;
    JsonNode *literal;   /* owned */
} CompiledSet;

static void
compiled_set_clear (CompiledSet *entry)
{
    g_clear_pointer (&entry->path,    g_free);
    g_clear_pointer (&entry->literal, json_node_unref);
}

/* ------------------------------------------------------------------ */
/*  PnSet                                                              */
/* ------------------------------------------------------------------ */

struct _PnSet
{
    PnNode parent_instance;

    gchar  *props_json;     /* canonical string property */
    GArray *compiled;       /* CompiledSet, parsed cache */
};

G_DEFINE_TYPE (PnSet, pn_set, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PROPS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Compilation                                                        */
/* ------------------------------------------------------------------ */

static void
free_compiled (GArray *arr)
{
    if (arr == NULL)
        return;

    for (guint i = 0; i < arr->len; i++)
        compiled_set_clear (&g_array_index (arr, CompiledSet, i));

    g_array_set_size (arr, 0);
}

static GArray *
compile_props (const gchar *json_string)
{
    GArray       *out    = g_array_new (FALSE, FALSE, sizeof (CompiledSet));
    JsonParser   *parser = json_parser_new ();
    JsonNode     *root;
    JsonArray    *array;
    GError       *error  = NULL;
    guint         i, n;

    if (json_string == NULL || *json_string == '\0')
        json_string = "[]";

    if (!json_parser_load_from_data (parser, json_string, -1, &error))
    {
        g_warning ("pn-set: invalid props JSON: %s", error->message);
        g_error_free (error);
        g_object_unref (parser);
        return out;
    }

    root  = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
        g_object_unref (parser);
        return out;
    }

    array = json_node_get_array (root);
    n     = json_array_get_length (array);
    for (i = 0; i < n; i++)
    {
        JsonNode    *item = json_array_get_element (array, i);
        JsonObject  *obj;
        const gchar *path;
        JsonNode    *lit;
        CompiledSet  entry = { 0 };

        if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
            continue;

        obj = json_node_get_object (item);
        if (!json_object_has_member (obj, "path") ||
            !json_object_has_member (obj, "literal"))
            continue;

        path = json_object_get_string_member (obj, "path");
        lit  = json_object_get_member        (obj, "literal");

        if (path == NULL || *path == '\0' || lit == NULL)
            continue;

        entry.path    = g_strdup (path);
        entry.literal = json_node_copy (lit);
        g_array_append_val (out, entry);
    }

    g_object_unref (parser);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Path assignment                                                    */
/*                                                                     */
/*  A path is normally a single member name like "value" or "tcp4",    */
/*  but accepting a dotted form ("foo.bar") for free lets the user     */
/*  reach nested data-bag objects without needing a Query node         */
/*  upstream.  Intermediate segments that are missing — or hold a      */
/*  non-object value — are replaced with a fresh JsonObject so the     */
/*  full path becomes addressable; this matches what a user typing     */
/*  "foo.bar" expects ("create the nested shape on demand") and        */
/*  mirrors how an object-literal assignment in JavaScript or a        */
/*  jq `.foo.bar = …` behaves.                                         */
/* ------------------------------------------------------------------ */

static void
assign_path (JsonObject *data, const gchar *path, JsonNode *literal)
{
    gchar    **parts;
    guint      i;

    if (data == NULL || path == NULL || *path == '\0' || literal == NULL)
        return;

    /* Bare member name fast path. */
    if (strchr (path, '.') == NULL)
    {
        json_object_set_member (data, path, json_node_copy (literal));
        return;
    }

    parts = g_strsplit (path, ".", -1);
    {
        JsonObject *cur = data;
        for (i = 0; parts[i] != NULL; i++)
        {
            if (parts[i + 1] == NULL)
            {
                json_object_set_member (cur, parts[i],
                                        json_node_copy (literal));
                break;
            }

            /* Intermediate segment: descend into an existing nested
             * object, or replace whatever is there with a fresh one. */
            if (json_object_has_member (cur, parts[i]))
            {
                JsonNode *child = json_object_get_member (cur, parts[i]);
                if (child != NULL && JSON_NODE_HOLDS_OBJECT (child))
                {
                    cur = json_node_get_object (child);
                    continue;
                }
            }

            {
                JsonObject *fresh = json_object_new ();
                JsonNode   *node  = json_node_new (JSON_NODE_OBJECT);
                json_node_take_object (node, fresh);
                json_object_set_member (cur, parts[i], node);
                cur = fresh;
            }
        }
    }
    g_strfreev (parts);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_set_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnSet      *self = PN_SET (node);
    JsonObject *data;
    guint       i;

    if (self->compiled == NULL || self->compiled->len == 0)
    {
        pn_node_emit_message (node, message);
        return;
    }

    data = pn_message_get_data (message);

    for (i = 0; i < self->compiled->len; i++)
    {
        const CompiledSet *entry = &g_array_index (self->compiled,
                                                   CompiledSet, i);
        assign_path (data, entry->path, entry->literal);
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_set_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnSet *self = PN_SET (object);

    switch (prop_id)
    {
    case PROP_PROPS:
        g_value_set_string (value,
                            self->props_json != NULL ? self->props_json
                                                     : "[]");
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_set_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnSet *self = PN_SET (object);

    switch (prop_id)
    {
    case PROP_PROPS:
        {
            const gchar *new_str = g_value_get_string (value);
            if (new_str == NULL || *new_str == '\0')
                new_str = "[]";

            if (g_strcmp0 (self->props_json, new_str) == 0)
                break;

            g_free (self->props_json);
            self->props_json = g_strdup (new_str);

            free_compiled (self->compiled);
            g_array_unref (self->compiled);
            self->compiled = compile_props (self->props_json);

            g_object_notify_by_pspec (object, props[PROP_PROPS]);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_class_tab override              */
/*                                                                     */
/*  PnSet exposes a single string property "props" carrying a JSON     */
/*  array of {path, literal} records.  Same shape as PnFilter's rule   */
/*  editor (one row of widgets per assignment, an Add button below,    */
/*  serialise-on-edit, rebuild-on-notify, updating-flag re-entrancy    */
/*  guard) minus the operator combo: Set assigns rather than compares, */
/*  so the only choices per row are the data-bag path, the literal     */
/*  type, and the value itself.                                        */
/* ------------------------------------------------------------------ */

#define PN_SET_PATH_CUSTOM "__custom__"

typedef enum
{
    SET_LIT_NUMBER,
    SET_LIT_STRING,
    SET_LIT_BOOLEAN,
} SetLiteralType;

typedef struct _PnSetBinding PnSetBinding;

typedef struct
{
    PnSetBinding    *binding;       /* borrowed */
    GtkWidget       *row_box;       /* outer GtkBox for this row */
    GtkComboBox     *path_combo;
    GtkEntry        *path_entry;    /* shown only when "Custom…" picked */
    GtkComboBox     *type_combo;    /* number / string / boolean */
    GtkEntry        *value_entry;   /* number / string */
    GtkComboBox     *value_bool;    /* true / false */
    gboolean         building;      /* skip per-widget signals */
} PnSetRow;

static SetLiteralType
set_lit_type_from_node (JsonNode *node)
{
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node))
    {
        GType t = json_node_get_value_type (node);
        if (t == G_TYPE_BOOLEAN)               return SET_LIT_BOOLEAN;
        if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE) return SET_LIT_NUMBER;
    }
    return SET_LIT_STRING;
}

static const gchar *
set_lit_type_id (SetLiteralType t)
{
    switch (t)
    {
    case SET_LIT_NUMBER:  return "number";
    case SET_LIT_BOOLEAN: return "boolean";
    case SET_LIT_STRING:
    default:              return "string";
    }
}

static SetLiteralType
set_lit_type_from_id (const gchar *id)
{
    if (g_strcmp0 (id, "boolean") == 0) return SET_LIT_BOOLEAN;
    if (g_strcmp0 (id, "string")  == 0) return SET_LIT_STRING;
    return SET_LIT_NUMBER;
}

static gint
set_suggested_type_for_path (const gchar *path)
{
    if (g_strcmp0 (path, "success")  == 0) return SET_LIT_BOOLEAN;
    if (g_strcmp0 (path, "value")    == 0) return SET_LIT_NUMBER;
    if (g_strcmp0 (path, "output")   == 0) return SET_LIT_STRING;
    if (g_strcmp0 (path, "endpoint") == 0) return SET_LIT_STRING;
    return -1;
}

static void
set_row_apply_type (PnSetRow *row, SetLiteralType type)
{
    gtk_widget_set_no_show_all (GTK_WIDGET (row->value_entry),
                                type == SET_LIT_BOOLEAN);
    gtk_widget_set_no_show_all (GTK_WIDGET (row->value_bool),
                                type != SET_LIT_BOOLEAN);
    gtk_widget_set_visible (GTK_WIDGET (row->value_entry),
                            type != SET_LIT_BOOLEAN);
    gtk_widget_set_visible (GTK_WIDGET (row->value_bool),
                            type == SET_LIT_BOOLEAN);
}

struct _PnSetBinding
{
    GObject     *target;            /* borrowed PnSet */
    GtkBox      *list;              /* vbox of PnSetRow.row_box */
    gulong       notify_handler;
    gboolean     updating;
};

static void set_serialise             (PnSetBinding *bind);
static void set_rebuild_from_property (PnSetBinding *bind);
static GtkWidget *set_build_row       (PnSetBinding *bind,
                                       const gchar  *path,
                                       JsonNode     *literal);

static void
set_binding_free (gpointer data)
{
    PnSetBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static gchar *
set_row_get_path (PnSetRow *row)
{
    const gchar *id = gtk_combo_box_get_active_id (row->path_combo);

    if (g_strcmp0 (id, PN_SET_PATH_CUSTOM) == 0)
        return g_strdup (gtk_entry_get_text (row->path_entry));
    if (id != NULL)
        return g_strdup (id);
    return g_strdup ("");
}

static void
set_row_emit_change (PnSetRow *row)
{
    if (row->building)
        return;
    if (row->binding->updating)
        return;

    set_serialise (row->binding);
}

static void
on_set_path_changed (GtkComboBox *combo, gpointer user_data)
{
    PnSetRow    *row = user_data;
    const gchar *id  = gtk_combo_box_get_active_id (combo);
    gboolean     custom = g_strcmp0 (id, PN_SET_PATH_CUSTOM) == 0;

    gtk_widget_set_visible (GTK_WIDGET (row->path_entry), custom);

    if (!custom)
    {
        gint suggested = set_suggested_type_for_path (id);
        if (suggested >= 0)
        {
            gboolean was_building = row->building;
            row->building = TRUE;
            gtk_combo_box_set_active_id (
                    row->type_combo,
                    set_lit_type_id ((SetLiteralType) suggested));
            set_row_apply_type (row, (SetLiteralType) suggested);
            row->building = was_building;
        }
    }

    set_row_emit_change (row);
}

static void
on_set_path_entry_changed (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    set_row_emit_change (user_data);
}

static void
on_set_type_changed (GtkComboBox *combo, gpointer user_data)
{
    PnSetRow       *row  = user_data;
    SetLiteralType  type = set_lit_type_from_id (
            gtk_combo_box_get_active_id (combo));
    gboolean        was_building = row->building;

    row->building = TRUE;
    set_row_apply_type (row, type);
    row->building = was_building;

    set_row_emit_change (row);
}

static void
on_set_value_entry_changed (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    set_row_emit_change (user_data);
}

static void
on_set_value_bool_changed (GtkComboBox *combo, gpointer user_data)
{
    (void) combo;
    set_row_emit_change (user_data);
}

static void
on_set_remove_clicked (GtkButton *btn, gpointer user_data)
{
    PnSetRow     *row  = user_data;
    PnSetBinding *bind = row->binding;

    (void) btn;

    gtk_widget_destroy (row->row_box);

    if (!bind->updating)
        set_serialise (bind);
}

static void
on_set_add_clicked (GtkButton *btn, gpointer user_data)
{
    PnSetBinding *bind = user_data;
    GtkWidget    *row;
    JsonNode     *zero;

    (void) btn;

    /* New rows default to "value = 0" — same shape PnFilter starts
     * with, so a user adding a Set after a Filter sees the same
     * skeleton and just edits the value. */
    zero = json_node_new (JSON_NODE_VALUE);
    json_node_set_double (zero, 0.0);
    row = set_build_row (bind, "value", zero);
    json_node_unref (zero);

    gtk_box_pack_start (bind->list, row, FALSE, FALSE, 0);
    gtk_widget_show_all (row);

    if (!bind->updating)
        set_serialise (bind);
}

static GtkWidget *
set_build_row (
        PnSetBinding *bind,
        const gchar  *path,
        JsonNode     *literal)
{
    PnSetRow       *row     = g_new0 (PnSetRow, 1);
    GtkWidget      *box     = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget      *path_combo;
    GtkWidget      *path_entry;
    GtkWidget      *type_combo;
    GtkWidget      *equals;
    GtkWidget      *value_entry;
    GtkWidget      *value_bool;
    GtkWidget      *remove_btn;
    SetLiteralType  type;
    gboolean        custom;

    row->binding  = bind;
    row->row_box  = box;
    row->building = TRUE;

    /* --- Path combo + custom entry ------------------------------- */
    path_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               "value",    "value");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               "success",  "success");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               "output",   "output");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               "endpoint", "endpoint");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               PN_SET_PATH_CUSTOM, "Custom…");
    row->path_combo = GTK_COMBO_BOX (path_combo);

    path_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (path_entry),
                                    "data-bag path");
    row->path_entry = GTK_ENTRY (path_entry);

    /* --- Literal type combo -------------------------------------- */
    type_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (type_combo),
                               "number",  "number");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (type_combo),
                               "string",  "string");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (type_combo),
                               "boolean", "boolean");
    row->type_combo = GTK_COMBO_BOX (type_combo);

    /* "=" label between type and value to read the row left-to-right
     * as "<path> := <literal>", clearer than "<path> <type> <value>"
     * which the eye reads as a Filter rule. */
    equals = gtk_label_new ("=");

    /* --- Literal value: see the Filter editor for why entry / bool
     *     combo coexist as siblings rather than living in a stack. */
    value_entry = gtk_entry_new ();
    gtk_widget_set_hexpand (value_entry, TRUE);
    value_bool  = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (value_bool),
                               "true",  "true");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (value_bool),
                               "false", "false");
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (value_bool), "true");

    row->value_entry = GTK_ENTRY (value_entry);
    row->value_bool  = GTK_COMBO_BOX (value_bool);

    remove_btn = gtk_button_new_from_icon_name ("list-remove-symbolic",
                                                GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (remove_btn, "Remove this assignment");

    gtk_box_pack_start (GTK_BOX (box), path_combo,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), path_entry,  TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (box), type_combo,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), equals,      FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), value_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (box), value_bool,  TRUE,  TRUE,  0);
    gtk_box_pack_end   (GTK_BOX (box), remove_btn,  FALSE, FALSE, 0);

    /* --- Initial state ------------------------------------------- */
    type   = set_lit_type_from_node (literal);

    custom = !(g_strcmp0 (path, "value")    == 0 ||
               g_strcmp0 (path, "success")  == 0 ||
               g_strcmp0 (path, "output")   == 0 ||
               g_strcmp0 (path, "endpoint") == 0);

    gtk_combo_box_set_active_id (row->path_combo,
                                 custom ? PN_SET_PATH_CUSTOM : path);
    gtk_entry_set_text (row->path_entry, custom && path != NULL ? path : "");

    gtk_combo_box_set_active_id (row->type_combo, set_lit_type_id (type));

    set_row_apply_type (row, type);

    if (literal != NULL && JSON_NODE_HOLDS_VALUE (literal))
    {
        switch (type)
        {
        case SET_LIT_BOOLEAN:
            gtk_combo_box_set_active_id (
                    row->value_bool,
                    json_node_get_boolean (literal) ? "true" : "false");
            break;
        case SET_LIT_NUMBER:
            {
                gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
                g_ascii_dtostr (buf, sizeof buf,
                                json_node_get_double (literal));
                gtk_entry_set_text (row->value_entry, buf);
            }
            break;
        case SET_LIT_STRING:
            gtk_entry_set_text (row->value_entry,
                                json_node_get_string (literal) != NULL
                                    ? json_node_get_string (literal) : "");
            break;
        }
    }
    else
    {
        gtk_combo_box_set_active_id (row->value_bool, "true");
    }

    g_object_set_data_full (G_OBJECT (box),
                            "pn-set-row", row, g_free);

    g_signal_connect (path_combo,  "changed",
                      G_CALLBACK (on_set_path_changed), row);
    g_signal_connect (path_entry,  "changed",
                      G_CALLBACK (on_set_path_entry_changed), row);
    g_signal_connect (type_combo,  "changed",
                      G_CALLBACK (on_set_type_changed), row);
    g_signal_connect (value_entry, "changed",
                      G_CALLBACK (on_set_value_entry_changed), row);
    g_signal_connect (value_bool,  "changed",
                      G_CALLBACK (on_set_value_bool_changed), row);
    g_signal_connect (remove_btn,  "clicked",
                      G_CALLBACK (on_set_remove_clicked), row);

    gtk_widget_set_no_show_all (path_entry, TRUE);
    gtk_widget_set_visible (path_entry,
                            g_strcmp0 (
                                gtk_combo_box_get_active_id (row->path_combo),
                                PN_SET_PATH_CUSTOM) == 0);

    row->building = FALSE;
    return box;
}

static JsonNode *
set_row_build_literal (PnSetRow *row)
{
    SetLiteralType  type = set_lit_type_from_id (
            gtk_combo_box_get_active_id (row->type_combo));
    JsonNode       *out  = json_node_new (JSON_NODE_VALUE);

    switch (type)
    {
    case SET_LIT_BOOLEAN:
        {
            const gchar *id = gtk_combo_box_get_active_id (row->value_bool);
            json_node_set_boolean (out, g_strcmp0 (id, "true") == 0);
        }
        break;
    case SET_LIT_NUMBER:
        {
            const gchar *txt = gtk_entry_get_text (row->value_entry);
            gdouble      v   = g_ascii_strtod (txt, NULL);
            json_node_set_double (out, v);
        }
        break;
    case SET_LIT_STRING:
        json_node_set_string (out, gtk_entry_get_text (row->value_entry));
        break;
    }

    return out;
}

static void
set_serialise (PnSetBinding *bind)
{
    JsonArray *arr = json_array_new ();
    JsonNode  *root;
    GList     *children;
    GList     *iter;
    gchar     *json_string;

    if (bind->updating)
    {
        json_array_unref (arr);
        return;
    }

    children = gtk_container_get_children (GTK_CONTAINER (bind->list));
    for (iter = children; iter != NULL; iter = iter->next)
    {
        GtkWidget  *row_box = iter->data;
        PnSetRow   *row     = g_object_get_data (G_OBJECT (row_box),
                                                 "pn-set-row");
        JsonObject *obj;
        gchar      *path;

        if (row == NULL)
            continue;

        path = set_row_get_path (row);

        if (path == NULL || *path == '\0')
        {
            g_free (path);
            continue;
        }

        obj = json_object_new ();
        json_object_set_string_member (obj, "path", path);
        json_object_set_member        (obj, "literal",
                                       set_row_build_literal (row));
        json_array_add_object_element (arr, obj);
        g_free (path);
    }
    g_list_free (children);

    root = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (root, arr);
    json_string = json_to_string (root, FALSE);
    json_node_unref (root);

    bind->updating = TRUE;
    g_object_set (bind->target, "props", json_string, NULL);
    bind->updating = FALSE;

    g_free (json_string);
}

static void
set_clear_rows (PnSetBinding *bind)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (bind->list));
    GList *iter;

    for (iter = children; iter != NULL; iter = iter->next)
        gtk_widget_destroy (GTK_WIDGET (iter->data));

    g_list_free (children);
}

static void
set_rebuild_from_property (PnSetBinding *bind)
{
    gchar      *json_string = NULL;
    JsonParser *parser;
    JsonNode   *root;
    JsonArray  *arr;
    guint       i, n;

    if (bind->updating)
        return;

    g_object_get (bind->target, "props", &json_string, NULL);

    bind->updating = TRUE;
    set_clear_rows (bind);

    parser = json_parser_new ();
    if (json_string != NULL && *json_string != '\0' &&
        json_parser_load_from_data (parser, json_string, -1, NULL))
    {
        root = json_parser_get_root (parser);
        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            arr = json_node_get_array (root);
            n   = json_array_get_length (arr);
            for (i = 0; i < n; i++)
            {
                JsonNode    *item = json_array_get_element (arr, i);
                JsonObject  *obj;
                const gchar *path;
                JsonNode    *lit;
                GtkWidget   *row;

                if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
                    continue;

                obj  = json_node_get_object (item);
                path = json_object_has_member (obj, "path")
                           ? json_object_get_string_member (obj, "path")
                           : "";
                lit  = json_object_has_member (obj, "literal")
                           ? json_object_get_member (obj, "literal")
                           : NULL;

                row = set_build_row (bind, path, lit);
                gtk_box_pack_start (bind->list, row, FALSE, FALSE, 0);
                gtk_widget_show_all (row);
            }
        }
    }
    g_object_unref (parser);
    g_free (json_string);

    bind->updating = FALSE;
}

static void
on_set_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    set_rebuild_from_property (user_data);
}

static GtkWidget *
pn_set_build_class_tab (PnNode    *self,
                        GtkWindow *parent G_GNUC_UNUSED)
{
    GObject      *target   = G_OBJECT (self);
    GtkWidget    *outer    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget    *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget    *list     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget    *add_btn  = gtk_button_new_with_label ("Add property");
    GtkWidget    *btn_row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    PnSetBinding *bind;

    g_object_set (outer,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, 200);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    gtk_container_add (GTK_CONTAINER (scrolled), list);

    gtk_box_pack_start (GTK_BOX (outer),   scrolled, TRUE,  TRUE,  0);

    gtk_widget_set_halign (add_btn, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (btn_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer),   btn_row, FALSE, FALSE, 0);

    bind = g_new0 (PnSetBinding, 1);
    bind->target = target;
    bind->list   = GTK_BOX (list);

    bind->notify_handler = g_signal_connect (
            target, "notify::props",
            G_CALLBACK (on_set_target_notify), bind);

    g_signal_connect (add_btn, "clicked",
                      G_CALLBACK (on_set_add_clicked), bind);

    g_object_set_data_full (G_OBJECT (outer),
                            "pn-set-binding",
                            bind, set_binding_free);

    set_rebuild_from_property (bind);

    return outer;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_set_finalize (GObject *object)
{
    PnSet *self = PN_SET (object);

    if (self->compiled != NULL)
    {
        free_compiled (self->compiled);
        g_array_unref (self->compiled);
        self->compiled = NULL;
    }
    g_clear_pointer (&self->props_json, g_free);

    G_OBJECT_CLASS (pn_set_parent_class)->finalize (object);
}

static void
pn_set_class_init (PnSetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_set_get_property;
    object_class->set_property = pn_set_set_property;
    object_class->finalize     = pn_set_finalize;
    node_class->receive        = pn_set_receive;

    node_class->build_class_tab = pn_set_build_class_tab;

    node_class->class_name     = "Set";
    node_class->icon           = "\xef\x80\xac";  /* fa-tags U+F02C */
    node_class->color          = (GdkRGBA){ 0.62, 0.45, 0.78, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_PROPS] = g_param_spec_string (
            "props", "Props",
            "JSON array of {path, literal} assignments applied to every "
            "passing message",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_set_init (PnSet *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  violet = { 0.62, 0.45, 0.78, 1.0 };

    self->props_json = g_strdup ("[]");
    self->compiled   = g_array_new (FALSE, FALSE, sizeof (CompiledSet));

    pn_node_set_class_name (node, "Set");
    pn_node_set_icon       (node, "\xef\x80\xac");  /* fa-tags U+F02C */
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSet *
pn_set_new (void)
{
    return g_object_new (PN_TYPE_SET, NULL);
}
