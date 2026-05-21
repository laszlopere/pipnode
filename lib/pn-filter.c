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

#include "pn-filter.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Compiled rule                                                      */
/*                                                                     */
/*  A rule's literal is held as the JsonNode parsed straight from the  */
/*  user's "rules" string.  The literal's value type (number /         */
/*  boolean / string) drives both which operators are valid and how    */
/*  the corresponding data-bag member is compared at receive time, so  */
/*  the on-disk representation does not need a separate type tag.      */
/* ------------------------------------------------------------------ */

typedef enum
{
    OP_LT,    /* < */
    OP_LE,    /* <= */
    OP_EQ,    /* == */
    OP_NE,    /* != */
    OP_GE,    /* >= */
    OP_GT,    /* > */
    OP_CONTAINS,
    OP_INVALID,
} PnFilterOp;

typedef struct
{
    gchar        *path;
    PnFilterOp    op;
    JsonNode     *literal;   /* owned */
    /* Pre-compiled glob pattern, %NULL unless @literal is a string
     * containing `*` or `?`.  When set, the OP_EQ / OP_NE branches in
     * compare_string() switch from byte-exact comparison to
     * g_pattern_spec_match — turns a literal like
     * "mqtt/stat/sonoff19/(asterisk)" into "matches every topic that starts
     * with that prefix" without forcing the user to learn the
     * `contains` operator (which would also match
     * "x/mqtt/stat/sonoff19/y" — usually not what they meant). */
    GPatternSpec *pattern;
} CompiledRule;

static void
compiled_rule_clear (CompiledRule *rule)
{
    g_clear_pointer (&rule->path,    g_free);
    g_clear_pointer (&rule->literal, json_node_unref);
    g_clear_pointer (&rule->pattern, g_pattern_spec_free);
}

/* ------------------------------------------------------------------ */
/*  PnFilter                                                           */
/* ------------------------------------------------------------------ */

struct _PnFilter
{
    PnNode parent_instance;

    gchar  *rules_json;     /* canonical string property */
    GArray *compiled;       /* CompiledRule, parsed cache */
};

G_DEFINE_TYPE (PnFilter, pn_filter, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_RULES,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Operator parsing                                                   */
/* ------------------------------------------------------------------ */

static PnFilterOp
parse_op (const gchar *s)
{
    if (s == NULL)               return OP_INVALID;
    if (g_strcmp0 (s, "<")  == 0) return OP_LT;
    if (g_strcmp0 (s, "<=") == 0) return OP_LE;
    if (g_strcmp0 (s, "==") == 0) return OP_EQ;
    if (g_strcmp0 (s, "!=") == 0) return OP_NE;
    if (g_strcmp0 (s, ">=") == 0) return OP_GE;
    if (g_strcmp0 (s, ">")  == 0) return OP_GT;
    if (g_strcmp0 (s, "contains") == 0) return OP_CONTAINS;
    return OP_INVALID;
}

/* ------------------------------------------------------------------ */
/*  Rule list (re)compilation                                          */
/* ------------------------------------------------------------------ */

static void
free_compiled (GArray *arr)
{
    if (arr == NULL)
        return;

    for (guint i = 0; i < arr->len; i++)
        compiled_rule_clear (&g_array_index (arr, CompiledRule, i));

    g_array_set_size (arr, 0);
}

static GArray *
compile_rules (const gchar *json_string)
{
    GArray       *out    = g_array_new (FALSE, FALSE, sizeof (CompiledRule));
    JsonParser   *parser = json_parser_new ();
    JsonNode     *root;
    JsonArray    *array;
    GError       *error  = NULL;
    guint         i, n;

    if (json_string == NULL || *json_string == '\0')
        json_string = "[]";

    if (!json_parser_load_from_data (parser, json_string, -1, &error))
    {
        g_warning ("pn-filter: invalid rules JSON: %s", error->message);
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
        JsonNode     *item = json_array_get_element (array, i);
        JsonObject   *obj;
        const gchar  *path;
        const gchar  *op_s;
        JsonNode     *lit;
        CompiledRule  rule = { 0 };

        if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
            continue;

        obj = json_node_get_object (item);
        if (!json_object_has_member (obj, "path") ||
            !json_object_has_member (obj, "op")   ||
            !json_object_has_member (obj, "literal"))
            continue;

        path = json_object_get_string_member (obj, "path");
        op_s = json_object_get_string_member (obj, "op");
        lit  = json_object_get_member        (obj, "literal");

        if (path == NULL || *path == '\0' || lit == NULL)
            continue;

        rule.op = parse_op (op_s);
        if (rule.op == OP_INVALID)
            continue;

        rule.path    = g_strdup (path);
        rule.literal = json_node_copy (lit);
        rule.pattern = NULL;

        /* If the literal is a string containing a glob metacharacter
         * (`*` or `?`), compile it once so the per-message hot path
         * does not allocate.  Anything else (numbers, booleans,
         * literal strings without wildcards) stays on the byte-exact
         * comparison path. */
        if (JSON_NODE_HOLDS_VALUE (rule.literal) &&
            json_node_get_value_type (rule.literal) == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (rule.literal);
            if (s != NULL && (strchr (s, '*') != NULL ||
                              strchr (s, '?') != NULL))
                rule.pattern = g_pattern_spec_new (s);
        }

        g_array_append_val (out, rule);
    }

    g_object_unref (parser);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Path lookup                                                        */
/*                                                                     */
/*  A rule path is normally a single member name like "value" or       */
/*  "tcp4", but accepting a dotted form ("foo.bar") for free lets the  */
/*  user reach into nested data-bag objects without needing a Query    */
/*  node upstream.  The first segment is matched against @data; each   */
/*  remaining segment descends into the next object on the way.        */
/* ------------------------------------------------------------------ */

static JsonNode *
lookup_path (JsonObject *data, const gchar *path)
{
    JsonNode  *node = NULL;
    gchar    **parts;
    guint      i;

    if (data == NULL || path == NULL || *path == '\0')
        return NULL;

    /* Bare member name fast path. */
    if (strchr (path, '.') == NULL)
    {
        if (json_object_has_member (data, path))
            return json_object_get_member (data, path);
        return NULL;
    }

    parts = g_strsplit (path, ".", -1);
    {
        JsonObject *cur = data;
        for (i = 0; parts[i] != NULL; i++)
        {
            if (cur == NULL || !json_object_has_member (cur, parts[i]))
            {
                node = NULL;
                break;
            }

            node = json_object_get_member (cur, parts[i]);

            if (parts[i + 1] != NULL)
            {
                if (!JSON_NODE_HOLDS_OBJECT (node))
                {
                    node = NULL;
                    break;
                }
                cur = json_node_get_object (node);
            }
        }
    }
    g_strfreev (parts);
    return node;
}

/* ------------------------------------------------------------------ */
/*  Comparison                                                         */
/* ------------------------------------------------------------------ */

static gboolean
compare_number (gdouble a, PnFilterOp op, gdouble b)
{
    switch (op)
    {
    case OP_LT: return a <  b;
    case OP_LE: return a <= b;
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case OP_GE: return a >= b;
    case OP_GT: return a >  b;
    default:    return FALSE;
    }
}

static gboolean
compare_boolean (gboolean a, PnFilterOp op, gboolean b)
{
    switch (op)
    {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    default:    return FALSE;
    }
}

static gboolean
compare_string (
        const gchar  *a,
        PnFilterOp    op,
        const gchar  *b,
        GPatternSpec *pattern)
{
    if (a == NULL) a = "";
    if (b == NULL) b = "";

    switch (op)
    {
    case OP_EQ:
        if (pattern != NULL)
            return g_pattern_spec_match_string (pattern, a);
        return g_strcmp0 (a, b) == 0;

    case OP_NE:
        if (pattern != NULL)
            return !g_pattern_spec_match_string (pattern, a);
        return g_strcmp0 (a, b) != 0;

    case OP_CONTAINS:
        return strstr (a, b) != NULL;

    default:
        return FALSE;
    }
}

/** Resolve a rule's path against either the envelope (when the path
 *  starts with '/' — currently "/topic") or the data bag.  Envelope
 *  lookups synthesise a transient JsonNode the caller is responsible
 *  for unrefing via @out_owned; data-bag lookups return a borrowed
 *  pointer with @out_owned left %NULL. */
static JsonNode *
resolve_path (
        PnMessage    *message,
        const gchar  *path,
        JsonNode    **out_owned)
{
    *out_owned = NULL;

    if (path != NULL && path[0] == '/')
    {
        const gchar *value = NULL;

        if (g_strcmp0 (path, "/topic") == 0)
            value = pn_message_get_topic (message);
        else
            return NULL;

        if (value == NULL)
            value = "";

        *out_owned = json_node_new (JSON_NODE_VALUE);
        json_node_set_string (*out_owned, value);
        return *out_owned;
    }

    return lookup_path (pn_message_get_data (message), path);
}

static gboolean
evaluate_rule (const CompiledRule *rule, PnMessage *message)
{
    JsonNode *owned  = NULL;
    JsonNode *member = resolve_path (message, rule->path, &owned);
    GType     lit_type;
    gboolean  result = FALSE;

    if (member == NULL || !JSON_NODE_HOLDS_VALUE (member))
        goto done;

    if (!JSON_NODE_HOLDS_VALUE (rule->literal))
        goto done;

    lit_type = json_node_get_value_type (rule->literal);

    if (lit_type == G_TYPE_INT64 || lit_type == G_TYPE_DOUBLE)
    {
        GType mtype = json_node_get_value_type (member);
        if (mtype != G_TYPE_INT64 && mtype != G_TYPE_DOUBLE)
            goto done;

        result = compare_number (json_node_get_double (member),
                                 rule->op,
                                 json_node_get_double (rule->literal));
    }
    else if (lit_type == G_TYPE_BOOLEAN)
    {
        if (json_node_get_value_type (member) != G_TYPE_BOOLEAN)
            goto done;
        result = compare_boolean (json_node_get_boolean (member),
                                  rule->op,
                                  json_node_get_boolean (rule->literal));
    }
    else if (lit_type == G_TYPE_STRING)
    {
        if (json_node_get_value_type (member) != G_TYPE_STRING)
            goto done;
        result = compare_string (json_node_get_string (member),
                                 rule->op,
                                 json_node_get_string (rule->literal),
                                 rule->pattern);
    }

done:
    if (owned != NULL)
        json_node_unref (owned);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_filter_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnFilter *self = PN_FILTER (node);
    guint     i;

    if (self->compiled == NULL || self->compiled->len == 0)
    {
        pn_node_emit_message (node, message);
        return;
    }

    for (i = 0; i < self->compiled->len; i++)
    {
        const CompiledRule *r = &g_array_index (self->compiled,
                                                CompiledRule, i);
        if (!evaluate_rule (r, message))
            return;
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_filter_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFilter *self = PN_FILTER (object);

    switch (prop_id)
    {
    case PROP_RULES:
        g_value_set_string (value,
                            self->rules_json != NULL ? self->rules_json
                                                     : "[]");
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_filter_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFilter *self = PN_FILTER (object);

    switch (prop_id)
    {
    case PROP_RULES:
        {
            const gchar *new_str = g_value_get_string (value);
            if (new_str == NULL || *new_str == '\0')
                new_str = "[]";

            if (g_strcmp0 (self->rules_json, new_str) == 0)
                break;

            g_free (self->rules_json);
            self->rules_json = g_strdup (new_str);

            free_compiled (self->compiled);
            g_array_unref (self->compiled);
            self->compiled = compile_rules (self->rules_json);

            g_object_notify_by_pspec (object, props[PROP_RULES]);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

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
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_filter_finalize (GObject *object)
{
    PnFilter *self = PN_FILTER (object);

    if (self->compiled != NULL)
    {
        free_compiled (self->compiled);
        g_array_unref (self->compiled);
        self->compiled = NULL;
    }
    g_clear_pointer (&self->rules_json, g_free);

    G_OBJECT_CLASS (pn_filter_parent_class)->finalize (object);
}

static void
pn_filter_class_init (PnFilterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_filter_get_property;
    object_class->set_property = pn_filter_set_property;
    object_class->finalize     = pn_filter_finalize;
    node_class->receive        = pn_filter_receive;

    node_class->build_class_tab = pn_filter_build_class_tab;

    node_class->class_name     = "Filter";
    node_class->icon           = "\xef\x82\xb0";  /* fa-filter U+F0B0 */
    node_class->color          = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_RULES] = g_param_spec_string (
            "rules", "Rules",
            "JSON array of {path, op, literal} predicates ANDed together",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_filter_init (PnFilter *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->rules_json = g_strdup ("[]");
    self->compiled   = g_array_new (FALSE, FALSE, sizeof (CompiledRule));

    pn_node_set_class_name (node, "Filter");
    pn_node_set_icon       (node, "\xef\x82\xb0");  /* fa-filter U+F0B0 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFilter *
pn_filter_new (void)
{
    return g_object_new (PN_TYPE_FILTER, NULL);
}
