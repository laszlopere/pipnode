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

/* ------------------------------------------------------------------ */
/*  PnFilter — gui tier.                                               */
/*                                                                     */
/*  The settings-dialog customisation for the Filter node.  The node's */
/*  GType, the "rules" string property, the compiled-rule cache and    */
/*  the rule-matching receive() logic live in the GTK-free core file   */
/*  pn-filter.c; this file installs the build_class_tab vfunc onto      */
/*  that class at editor startup (pn_filter_gui_install).  The whole    */
/*  per-rule row editor reads and writes the node's "rules" property    */
/*  via g_object_get/g_object_set, so it needs no core seam.  The       */
/*  headless runtime never loads this half, so the rule matching runs   */
/*  without GTK.                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-filter-gui.h"
#include "pn-filter.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_class_tab override              */
/*                                                                     */
/*  PnFilter exposes a single string property "rules" carrying a JSON  */
/*  array of {path, op, literal} records.  The auto-generated grid     */
/*  would render that as one giant text entry, which is unusable; the  */
/*  builder below renders it as one row of widgets per rule plus an    */
/*  Add button, and serialises the row state back to the property     */
/*  string whenever any widget changes.  A binding struct attached    */
/*  to the editor widget owns the lifetime, an `updating` flag breaks  */
/*  the loop between widget edits and notify::rules.                   */
/* ------------------------------------------------------------------ */

#define PN_FILTER_PATH_CUSTOM "__custom__"

typedef enum
{
    LIT_NUMBER,
    LIT_STRING,
    LIT_BOOLEAN,
} LiteralType;

typedef struct _PnFilterBinding PnFilterBinding;

typedef struct
{
    PnFilterBinding *binding;       /* borrowed */
    GtkWidget       *row_box;       /* outer GtkBox for this row */
    GtkComboBox     *path_combo;
    GtkEntry        *path_entry;    /* shown only when "Custom…" picked */
    GtkComboBox     *type_combo;    /* number / string / boolean */
    GtkComboBox     *op_combo;
    GtkEntry        *value_entry;   /* number / string */
    GtkComboBox     *value_bool;    /* true / false */
    gboolean         building;      /* skip per-widget signals */
} PnFilterRow;

/** Show one of (value_entry, value_bool) according to @type, hide the
 *  other.  Two siblings in a box with explicit visibility toggling
 *  side-step a GtkStack quirk where `gtk_widget_show_all` on the
 *  stack's ancestor flips the visible-child as it cascades show
 *  through the stack's children. */
static void
filter_row_apply_type (PnFilterRow *row, LiteralType type)
{
    gtk_widget_set_no_show_all (GTK_WIDGET (row->value_entry),
                                type == LIT_BOOLEAN);
    gtk_widget_set_no_show_all (GTK_WIDGET (row->value_bool),
                                type != LIT_BOOLEAN);
    gtk_widget_set_visible (GTK_WIDGET (row->value_entry),
                            type != LIT_BOOLEAN);
    gtk_widget_set_visible (GTK_WIDGET (row->value_bool),
                            type == LIT_BOOLEAN);
}

struct _PnFilterBinding
{
    GObject     *target;            /* borrowed PnFilter */
    GtkBox      *list;              /* vbox of PnFilterRow.row_box */
    gulong       notify_handler;
    gboolean     updating;
};

static void filter_serialise           (PnFilterBinding *bind);
static void filter_rebuild_from_property (PnFilterBinding *bind);
static GtkWidget *filter_build_row     (PnFilterBinding *bind,
                                        const gchar     *path,
                                        const gchar     *op,
                                        JsonNode        *literal);

static void
filter_binding_free (gpointer data)
{
    PnFilterBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static LiteralType
filter_type_from_node (JsonNode *node)
{
    if (node != NULL && JSON_NODE_HOLDS_VALUE (node))
    {
        GType t = json_node_get_value_type (node);
        if (t == G_TYPE_BOOLEAN)               return LIT_BOOLEAN;
        if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE) return LIT_NUMBER;
    }
    return LIT_STRING;
}

static const gchar *
filter_type_id (LiteralType t)
{
    switch (t)
    {
    case LIT_NUMBER:  return "number";
    case LIT_BOOLEAN: return "boolean";
    case LIT_STRING:
    default:          return "string";
    }
}

static LiteralType
filter_type_from_id (const gchar *id)
{
    if (g_strcmp0 (id, "boolean") == 0) return LIT_BOOLEAN;
    if (g_strcmp0 (id, "string")  == 0) return LIT_STRING;
    return LIT_NUMBER;
}

/** Repopulate @combo with the operators valid for @type, preserving
 *  the previously-active op if it is still valid (otherwise picks the
 *  first available op). */
static void
filter_populate_op_combo (GtkComboBox *combo, LiteralType type)
{
    static const gchar * const ops_number[]  = {
        "<", "<=", "==", "!=", ">=", ">"
    };
    static const gchar * const ops_boolean[] = { "==", "!=" };
    static const gchar * const ops_string[]  = { "==", "!=", "contains" };

    const gchar * const *list;
    gsize                n;
    gchar               *previous;
    gboolean             previous_listed = FALSE;
    gsize                i;

    switch (type)
    {
    case LIT_BOOLEAN: list = ops_boolean; n = G_N_ELEMENTS (ops_boolean); break;
    case LIT_STRING:  list = ops_string;  n = G_N_ELEMENTS (ops_string);  break;
    case LIT_NUMBER:
    default:          list = ops_number;  n = G_N_ELEMENTS (ops_number);  break;
    }

    previous = g_strdup (gtk_combo_box_get_active_id (combo));

    gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
    for (i = 0; i < n; i++)
    {
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   list[i], list[i]);
        if (g_strcmp0 (previous, list[i]) == 0)
            previous_listed = TRUE;
    }

    if (previous_listed)
        gtk_combo_box_set_active_id (combo, previous);
    else
        gtk_combo_box_set_active (combo, 0);

    g_free (previous);
}

/** Suggest a literal type for the freshly-picked path.  The choice is
 *  driven by the documented standard data-bag members (`success` is a
 *  boolean, `value` is a number, `output` / `endpoint` are strings)
 *  plus the envelope addresses (paths that start with `/`).  Anything
 *  else (including the "Custom…" placeholder) returns -1 to mean
 *  "leave the user's current pick alone". */
static gint
filter_suggested_type_for_path (const gchar *path)
{
    if (g_strcmp0 (path, "success")  == 0) return LIT_BOOLEAN;
    if (g_strcmp0 (path, "value")    == 0) return LIT_NUMBER;
    if (g_strcmp0 (path, "output")   == 0) return LIT_STRING;
    if (g_strcmp0 (path, "endpoint") == 0) return LIT_STRING;
    if (g_strcmp0 (path, "/topic")   == 0) return LIT_STRING;
    return -1;
}

static gchar *
filter_row_get_path (PnFilterRow *row)
{
    const gchar *id = gtk_combo_box_get_active_id (row->path_combo);

    if (g_strcmp0 (id, PN_FILTER_PATH_CUSTOM) == 0)
        return g_strdup (gtk_entry_get_text (row->path_entry));
    if (id != NULL)
        return g_strdup (id);
    return g_strdup ("");
}

static void
filter_row_emit_change (PnFilterRow *row)
{
    if (row->building)
        return;
    if (row->binding->updating)
        return;

    filter_serialise (row->binding);
}

static void
on_filter_path_changed (GtkComboBox *combo, gpointer user_data)
{
    PnFilterRow *row = user_data;
    const gchar *id  = gtk_combo_box_get_active_id (combo);
    gboolean     custom = g_strcmp0 (id, PN_FILTER_PATH_CUSTOM) == 0;

    gtk_widget_set_visible (GTK_WIDGET (row->path_entry), custom);

    if (!custom)
    {
        gint suggested = filter_suggested_type_for_path (id);
        if (suggested >= 0)
        {
            gboolean was_building = row->building;
            row->building = TRUE;
            gtk_combo_box_set_active_id (
                    row->type_combo,
                    filter_type_id ((LiteralType) suggested));
            filter_populate_op_combo (row->op_combo,
                                      (LiteralType) suggested);
            filter_row_apply_type (row, (LiteralType) suggested);
            row->building = was_building;
        }
    }

    filter_row_emit_change (row);
}

static void
on_filter_path_entry_changed (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    filter_row_emit_change (user_data);
}

static void
on_filter_type_changed (GtkComboBox *combo, gpointer user_data)
{
    PnFilterRow *row  = user_data;
    LiteralType  type = filter_type_from_id (
            gtk_combo_box_get_active_id (combo));
    gboolean     was_building = row->building;

    /* Repopulating the op combo fires "changed"; suppress the
     * downstream serialise so we only push once at the end. */
    row->building = TRUE;
    filter_populate_op_combo (row->op_combo, type);
    filter_row_apply_type   (row, type);
    row->building = was_building;

    filter_row_emit_change (row);
}

static void
on_filter_op_changed (GtkComboBox *combo, gpointer user_data)
{
    (void) combo;
    filter_row_emit_change (user_data);
}

static void
on_filter_value_entry_changed (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    filter_row_emit_change (user_data);
}

static void
on_filter_value_bool_changed (GtkComboBox *combo, gpointer user_data)
{
    (void) combo;
    filter_row_emit_change (user_data);
}

static void
on_filter_remove_clicked (GtkButton *btn, gpointer user_data)
{
    PnFilterRow     *row  = user_data;
    PnFilterBinding *bind = row->binding;

    (void) btn;

    /* Destroying the row widget drops the PnFilterRow attached via
     * g_object_set_data_full, so the per-row state goes with it. */
    gtk_widget_destroy (row->row_box);

    if (!bind->updating)
        filter_serialise (bind);
}

static void
on_filter_add_clicked (GtkButton *btn, gpointer user_data)
{
    PnFilterBinding *bind = user_data;
    GtkWidget       *row;
    JsonNode        *zero;

    (void) btn;

    /* New rows default to "value >= 0" — the most common shape and
     * cheap for the user to retarget. */
    zero = json_node_new (JSON_NODE_VALUE);
    json_node_set_double (zero, 0.0);
    row = filter_build_row (bind, "value", ">=", zero);
    json_node_unref (zero);

    gtk_box_pack_start (bind->list, row, FALSE, FALSE, 0);
    gtk_widget_show_all (row);

    if (!bind->updating)
        filter_serialise (bind);
}

static GtkWidget *
filter_build_row (
        PnFilterBinding *bind,
        const gchar     *path,
        const gchar     *op,
        JsonNode        *literal)
{
    PnFilterRow *row     = g_new0 (PnFilterRow, 1);
    GtkWidget   *box     = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget   *path_combo;
    GtkWidget   *path_entry;
    GtkWidget   *type_combo;
    GtkWidget   *op_combo;
    GtkWidget   *value_entry;
    GtkWidget   *value_bool;
    GtkWidget   *remove_btn;
    LiteralType  type;
    gboolean     custom;

    row->binding  = bind;
    row->row_box  = box;
    row->building = TRUE;

    /* --- Path combo + custom entry -------------------------------
     *
     * Bare names ("value", "success", …) address the data bag.  A
     * leading slash on the id ("/topic") routes the lookup to the
     * envelope instead — currently only "/topic" is supported, so
     * one entry covers the only envelope path the filter can reach. */
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
                               "/topic",   "/topic");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (path_combo),
                               PN_FILTER_PATH_CUSTOM, "Custom…");
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

    /* --- Operator combo ------------------------------------------ */
    op_combo = gtk_combo_box_text_new ();
    row->op_combo = GTK_COMBO_BOX (op_combo);

    /* --- Literal value: entry (number / string) and bool combo are
     *     siblings in the row; filter_row_apply_type toggles which is
     *     visible.  This deliberately avoids GtkStack — show_all on
     *     the dialog cascades show through every stack child and
     *     races the visible-child setting, leaving the wrong child
     *     drawn after a reload. */
    value_entry = gtk_entry_new ();
    gtk_widget_set_hexpand (value_entry, TRUE);
    value_bool  = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (value_bool),
                               "true",  "true");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (value_bool),
                               "false", "false");
    /* Always have a defined active id so a switch to boolean type
     * never silently serialises as `false` for an unset combo. */
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (value_bool), "true");

    row->value_entry = GTK_ENTRY (value_entry);
    row->value_bool  = GTK_COMBO_BOX (value_bool);

    /* --- Remove button ------------------------------------------- */
    remove_btn = gtk_button_new_from_icon_name ("list-remove-symbolic",
                                                GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (remove_btn, "Remove this rule");

    gtk_box_pack_start (GTK_BOX (box), path_combo,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), path_entry,  TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (box), type_combo,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), op_combo,    FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), value_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (box), value_bool,  TRUE,  TRUE,  0);
    gtk_box_pack_end   (GTK_BOX (box), remove_btn,  FALSE, FALSE, 0);

    /* --- Initial state ------------------------------------------- */
    type   = filter_type_from_node (literal);

    custom = !(g_strcmp0 (path, "value")    == 0 ||
               g_strcmp0 (path, "success")  == 0 ||
               g_strcmp0 (path, "output")   == 0 ||
               g_strcmp0 (path, "endpoint") == 0 ||
               g_strcmp0 (path, "/topic")   == 0);

    gtk_combo_box_set_active_id (row->path_combo,
                                 custom ? PN_FILTER_PATH_CUSTOM : path);
    gtk_entry_set_text (row->path_entry, custom && path != NULL ? path : "");

    gtk_combo_box_set_active_id (row->type_combo, filter_type_id (type));
    filter_populate_op_combo    (row->op_combo,   type);
    if (op != NULL)
        gtk_combo_box_set_active_id (row->op_combo, op);

    filter_row_apply_type (row, type);

    if (literal != NULL && JSON_NODE_HOLDS_VALUE (literal))
    {
        switch (type)
        {
        case LIT_BOOLEAN:
            gtk_combo_box_set_active_id (
                    row->value_bool,
                    json_node_get_boolean (literal) ? "true" : "false");
            break;
        case LIT_NUMBER:
            {
                gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
                g_ascii_dtostr (buf, sizeof buf,
                                json_node_get_double (literal));
                gtk_entry_set_text (row->value_entry, buf);
            }
            break;
        case LIT_STRING:
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

    /* Tie the per-row struct's lifetime to the row widget. */
    g_object_set_data_full (G_OBJECT (box),
                            "pn-filter-row", row, g_free);

    /* Wire signals after initial state so initialisation does not
     * fire serialise repeatedly. */
    g_signal_connect (path_combo,  "changed",
                      G_CALLBACK (on_filter_path_changed), row);
    g_signal_connect (path_entry,  "changed",
                      G_CALLBACK (on_filter_path_entry_changed), row);
    g_signal_connect (type_combo,  "changed",
                      G_CALLBACK (on_filter_type_changed), row);
    g_signal_connect (op_combo,    "changed",
                      G_CALLBACK (on_filter_op_changed), row);
    g_signal_connect (value_entry, "changed",
                      G_CALLBACK (on_filter_value_entry_changed), row);
    g_signal_connect (value_bool,  "changed",
                      G_CALLBACK (on_filter_value_bool_changed), row);
    g_signal_connect (remove_btn,  "clicked",
                      G_CALLBACK (on_filter_remove_clicked), row);

    /* path_entry is only meaningful when the path combo is on
     * "Custom…"; gate it via no-show-all so the dialog's outer
     * show_all does not reveal it for any other selection. */
    gtk_widget_set_no_show_all (path_entry, TRUE);
    gtk_widget_set_visible (path_entry,
                            g_strcmp0 (
                                gtk_combo_box_get_active_id (row->path_combo),
                                PN_FILTER_PATH_CUSTOM) == 0);

    row->building = FALSE;
    return box;
}

/** Build a JsonNode literal from row widgets according to the
 *  selected literal type.  Caller takes ownership. */
static JsonNode *
filter_row_build_literal (PnFilterRow *row)
{
    LiteralType  type = filter_type_from_id (
            gtk_combo_box_get_active_id (row->type_combo));
    JsonNode    *out  = json_node_new (JSON_NODE_VALUE);

    switch (type)
    {
    case LIT_BOOLEAN:
        {
            const gchar *id = gtk_combo_box_get_active_id (row->value_bool);
            json_node_set_boolean (out, g_strcmp0 (id, "true") == 0);
        }
        break;
    case LIT_NUMBER:
        {
            const gchar *txt = gtk_entry_get_text (row->value_entry);
            gdouble      v   = g_ascii_strtod (txt, NULL);
            json_node_set_double (out, v);
        }
        break;
    case LIT_STRING:
        json_node_set_string (out, gtk_entry_get_text (row->value_entry));
        break;
    }

    return out;
}

static void
filter_serialise (PnFilterBinding *bind)
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
        GtkWidget   *row_box = iter->data;
        PnFilterRow *row     = g_object_get_data (G_OBJECT (row_box),
                                                  "pn-filter-row");
        JsonObject  *obj;
        gchar       *path;
        const gchar *op;

        if (row == NULL)
            continue;

        path = filter_row_get_path (row);
        op   = gtk_combo_box_get_active_id (row->op_combo);

        if (path == NULL || *path == '\0' || op == NULL)
        {
            g_free (path);
            continue;
        }

        obj = json_object_new ();
        json_object_set_string_member (obj, "path", path);
        json_object_set_string_member (obj, "op",   op);
        json_object_set_member        (obj, "literal",
                                       filter_row_build_literal (row));
        json_array_add_object_element (arr, obj);
        g_free (path);
    }
    g_list_free (children);

    root = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (root, arr);
    json_string = json_to_string (root, FALSE);
    json_node_unref (root);

    bind->updating = TRUE;
    g_object_set (bind->target, "rules", json_string, NULL);
    bind->updating = FALSE;

    g_free (json_string);
}

static void
filter_clear_rows (PnFilterBinding *bind)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (bind->list));
    GList *iter;

    for (iter = children; iter != NULL; iter = iter->next)
        gtk_widget_destroy (GTK_WIDGET (iter->data));

    g_list_free (children);
}

static void
filter_rebuild_from_property (PnFilterBinding *bind)
{
    gchar      *json_string = NULL;
    JsonParser *parser;
    JsonNode   *root;
    JsonArray  *arr;
    guint       i, n;

    /* Re-entrancy guard: when our own serialiser writes the property
     * the resulting notify::rules must not clobber the rows the user
     * is currently editing. */
    if (bind->updating)
        return;

    g_object_get (bind->target, "rules", &json_string, NULL);

    bind->updating = TRUE;
    filter_clear_rows (bind);

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
                const gchar *op;
                JsonNode    *lit;
                GtkWidget   *row;

                if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
                    continue;

                obj  = json_node_get_object (item);
                path = json_object_has_member (obj, "path")
                           ? json_object_get_string_member (obj, "path")
                           : "";
                op   = json_object_has_member (obj, "op")
                           ? json_object_get_string_member (obj, "op")
                           : "==";
                lit  = json_object_has_member (obj, "literal")
                           ? json_object_get_member (obj, "literal")
                           : NULL;

                row = filter_build_row (bind, path, op, lit);
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
on_filter_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    filter_rebuild_from_property (user_data);
}

static GtkWidget *
pn_filter_build_class_tab (PnNode    *self,
                           GtkWindow *parent G_GNUC_UNUSED)
{
    GObject         *target   = G_OBJECT (self);
    GtkWidget       *outer    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget       *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget       *list     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget       *add_btn  = gtk_button_new_with_label ("Add rule");
    GtkWidget       *btn_row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    PnFilterBinding *bind;

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

    bind = g_new0 (PnFilterBinding, 1);
    bind->target = target;
    bind->list   = GTK_BOX (list);

    bind->notify_handler = g_signal_connect (
            target, "notify::rules",
            G_CALLBACK (on_filter_target_notify), bind);

    g_signal_connect (add_btn, "clicked",
                      G_CALLBACK (on_filter_add_clicked), bind);

    /* Tie the binding to the outer widget. */
    g_object_set_data_full (G_OBJECT (outer),
                            "pn-filter-binding",
                            bind, filter_binding_free);

    /* Initial fill from the property. */
    filter_rebuild_from_property (bind);

    return outer;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_filter_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_FILTER));

    node_class->build_class_tab = pn_filter_build_class_tab;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
