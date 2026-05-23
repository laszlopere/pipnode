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
    /* build_class_tab installed by the gui tier (pn_filter_gui_install). */

    node_class->class_name     = "Filter";
    node_class->icon           = "\xef\x82\xb0";  /* fa-filter U+F0B0 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Gate";
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
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

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
