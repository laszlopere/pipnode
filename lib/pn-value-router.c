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

#include "pn-value-router.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define PN_VALUE_ROUTER_DEF_PATH "value"

typedef struct
{
    PnValueRouterOp op;
    gdouble         a;   /* comparison value, or the range's low end */
    gdouble         b;   /* the range's high end                     */
} Rule;

struct _PnValueRouter
{
    PnNode parent_instance;

    gint   n_outputs;   /* configurable output count, 2..16            */
    gchar *path;        /* data-bag member to test, never NULL or ""   */

    /* Always PN_VALUE_ROUTER_MAX_OUTPUTS slots, so the rules of outputs
     * removed by lowering the count survive until it is raised again. */
    Rule   rules[PN_VALUE_ROUTER_MAX_OUTPUTS];
};

G_DEFINE_TYPE (PnValueRouter, pn_value_router, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OUTPUTS,
    PROP_PATH,
    PROP_RULES,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static const gchar *const op_names[PN_VALUE_ROUTER_N_OPS] = {
    [PN_VALUE_ROUTER_OP_UNUSED] = "",
    [PN_VALUE_ROUTER_OP_LT]     = "<",
    [PN_VALUE_ROUTER_OP_LE]     = "<=",
    [PN_VALUE_ROUTER_OP_EQ]     = "==",
    [PN_VALUE_ROUTER_OP_NE]     = "!=",
    [PN_VALUE_ROUTER_OP_GE]     = ">=",
    [PN_VALUE_ROUTER_OP_GT]     = ">",
    [PN_VALUE_ROUTER_OP_RANGE]  = "range",
    [PN_VALUE_ROUTER_OP_ELSE]   = "else",
};

const gchar *
pn_value_router_op_to_string (PnValueRouterOp op)
{
    if (op < 0 || op >= PN_VALUE_ROUTER_N_OPS)
        return "";
    return op_names[op];
}

PnValueRouterOp
pn_value_router_op_from_string (const gchar *text)
{
    gint i;

    if (text == NULL || *text == '\0')
        return PN_VALUE_ROUTER_OP_UNUSED;

    for (i = 1; i < PN_VALUE_ROUTER_N_OPS; i++)
        if (strcmp (text, op_names[i]) == 0)
            return (PnValueRouterOp) i;

    return PN_VALUE_ROUTER_OP_UNUSED;
}

/* ------------------------------------------------------------------ */
/*  Matching                                                           */
/* ------------------------------------------------------------------ */

static gboolean
rule_matches (const Rule *r, gboolean has_number, gdouble v)
{
    if (r->op == PN_VALUE_ROUTER_OP_ELSE)
        return TRUE;
    if (!has_number)
        return FALSE;

    switch (r->op)
    {
    case PN_VALUE_ROUTER_OP_LT:    return v <  r->a;
    case PN_VALUE_ROUTER_OP_LE:    return v <= r->a;
    case PN_VALUE_ROUTER_OP_EQ:    return v == r->a;
    case PN_VALUE_ROUTER_OP_NE:    return v != r->a;
    case PN_VALUE_ROUTER_OP_GE:    return v >= r->a;
    case PN_VALUE_ROUTER_OP_GT:    return v >  r->a;
    case PN_VALUE_ROUTER_OP_RANGE: return v >= r->a && v <= r->b;
    default:                       return FALSE;
    }
}

gint
pn_value_router_route_number (
        PnValueRouter *self,
        gboolean       has_number,
        gdouble        number)
{
    gint i;

    g_return_val_if_fail (PN_IS_VALUE_ROUTER (self), -1);

    for (i = 0; i < self->n_outputs; i++)
        if (rule_matches (&self->rules[i], has_number, number))
            return i;

    return -1;
}

/* The number at @path in @data: a bare member name, or a dotted path
 * descending through nested objects.  Booleans read as 1.0 / 0.0. */
static gboolean
read_number (JsonObject *data, const gchar *path, gdouble *out)
{
    JsonNode  *node = NULL;
    gchar    **parts;
    guint      i;
    GType      t;

    if (data == NULL)
        return FALSE;

    parts = g_strsplit (path, ".", -1);
    for (i = 0; parts[i] != NULL; i++)
    {
        if (data == NULL || !json_object_has_member (data, parts[i]))
        {
            node = NULL;
            break;
        }
        node = json_object_get_member (data, parts[i]);
        data = JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node)
                                             : NULL;
    }
    g_strfreev (parts);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    t = json_node_get_value_type (node);
    if (t == G_TYPE_DOUBLE)
        *out = json_node_get_double (node);
    else if (t == G_TYPE_INT64)
        *out = (gdouble) json_node_get_int (node);
    else if (t == G_TYPE_BOOLEAN)
        *out = json_node_get_boolean (node) ? 1.0 : 0.0;
    else
        return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_value_router_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnValueRouter *self = PN_VALUE_ROUTER (node);
    gdouble        v    = 0.0;
    gboolean       has  = read_number (pn_message_get_data (message),
                                       self->path, &v);
    gint           out  = pn_value_router_route_number (self, has, v);

    /* Forwarded untouched, like Topic Demux; unmatched is dropped. */
    if (out >= 0)
        pn_node_emit_message_on_output (node, message, out);
}

/* ------------------------------------------------------------------ */
/*  Rule slots                                                         */
/* ------------------------------------------------------------------ */

/* The worksheet label of output @index; "outN" when the slot is unused. */
static void
update_output_name (PnValueRouter *self, gint index)
{
    const Rule *r     = &self->rules[index];
    gchar      *label = NULL;

    switch (r->op)
    {
    case PN_VALUE_ROUTER_OP_UNUSED:
        break;
    case PN_VALUE_ROUTER_OP_ELSE:
        label = g_strdup ("else");
        break;
    case PN_VALUE_ROUTER_OP_RANGE:
        label = g_strdup_printf ("%g..%g", r->a, r->b);
        break;
    default:
        label = g_strdup_printf ("%s %g", op_names[r->op], r->a);
        break;
    }

    pn_node_set_output_name (PN_NODE (self), index, label);
    g_free (label);
}

/* Store one slot; returns TRUE when it changed.  Does not notify.  Values
 * an op does not use are stored as 0 so equal rules compare equal. */
static gboolean
store_rule (PnValueRouter *self, gint index, PnValueRouterOp op,
            gdouble a, gdouble b)
{
    Rule *r = &self->rules[index];

    if (op < 0 || op >= PN_VALUE_ROUTER_N_OPS)
        op = PN_VALUE_ROUTER_OP_UNUSED;
    if (op == PN_VALUE_ROUTER_OP_UNUSED || op == PN_VALUE_ROUTER_OP_ELSE)
        a = 0.0;
    if (op != PN_VALUE_ROUTER_OP_RANGE)
        b = 0.0;

    if (r->op == op && r->a == a && r->b == b)
        return FALSE;

    r->op = op;
    r->a  = a;
    r->b  = b;
    update_output_name (self, index);
    return TRUE;
}

/* JSON array of every slot, trailing unused slots trimmed. */
static gchar *
rules_to_json (PnValueRouter *self)
{
    JsonBuilder *builder = json_builder_new ();
    JsonNode    *root;
    gchar       *json;
    gint         last = PN_VALUE_ROUTER_MAX_OUTPUTS - 1;
    gint         i;

    while (last >= 0 && self->rules[last].op == PN_VALUE_ROUTER_OP_UNUSED)
        last--;

    json_builder_begin_array (builder);
    for (i = 0; i <= last; i++)
    {
        const Rule *r = &self->rules[i];

        json_builder_begin_object (builder);
        if (r->op != PN_VALUE_ROUTER_OP_UNUSED)
        {
            json_builder_set_member_name (builder, "op");
            json_builder_add_string_value (builder, op_names[r->op]);
        }
        if (r->op == PN_VALUE_ROUTER_OP_RANGE)
        {
            json_builder_set_member_name (builder, "low");
            json_builder_add_double_value (builder, r->a);
            json_builder_set_member_name (builder, "high");
            json_builder_add_double_value (builder, r->b);
        }
        else if (r->op != PN_VALUE_ROUTER_OP_UNUSED &&
                 r->op != PN_VALUE_ROUTER_OP_ELSE)
        {
            json_builder_set_member_name (builder, "value");
            json_builder_add_double_value (builder, r->a);
        }
        json_builder_end_object (builder);
    }
    json_builder_end_array (builder);

    root = json_builder_get_root (builder);
    json = json_to_string (root, FALSE);
    json_node_unref (root);
    g_object_unref (builder);
    return json;
}

static gdouble
member_number (JsonObject *obj, const gchar *name)
{
    JsonNode *n;
    GType     t;

    if (!json_object_has_member (obj, name))
        return 0.0;

    n = json_object_get_member (obj, name);
    if (!JSON_NODE_HOLDS_VALUE (n))
        return 0.0;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double (n);
    if (t == G_TYPE_INT64)
        return (gdouble) json_node_get_int (n);
    return 0.0;
}

/* Replace every slot from a JSON array of rule objects.  Anything that is
 * not such an array clears them all; an element that is not an object
 * with a known "op" clears its slot. */
static void
rules_from_json (PnValueRouter *self, const gchar *json)
{
    JsonParser *parser = json_parser_new ();
    JsonArray  *arr    = NULL;
    guint       n      = 0;
    gint        i;

    if (json != NULL && *json != '\0' &&
        json_parser_load_from_data (parser, json, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            arr = json_node_get_array (root);
            n   = json_array_get_length (arr);
        }
    }

    for (i = 0; i < PN_VALUE_ROUTER_MAX_OUTPUTS; i++)
    {
        PnValueRouterOp op = PN_VALUE_ROUTER_OP_UNUSED;
        gdouble         a  = 0.0;
        gdouble         b  = 0.0;

        if ((guint) i < n)
        {
            JsonNode *el = json_array_get_element (arr, i);

            if (JSON_NODE_HOLDS_OBJECT (el))
            {
                JsonObject *obj = json_node_get_object (el);

                if (json_object_has_member (obj, "op"))
                    op = pn_value_router_op_from_string (
                            json_object_get_string_member_with_default (
                                    obj, "op", ""));

                if (op == PN_VALUE_ROUTER_OP_RANGE)
                {
                    a = member_number (obj, "low");
                    b = member_number (obj, "high");
                }
                else
                {
                    a = member_number (obj, "value");
                }
            }
        }
        store_rule (self, i, op, a, b);
    }

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
value_router_set_outputs (PnValueRouter *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    self->n_outputs = n;
    pn_node_set_n_outputs (node, n);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
value_router_set_path (PnValueRouter *self, const gchar *path)
{
    gchar *stripped = g_strstrip (g_strdup (path != NULL ? path : ""));

    if (*stripped == '\0')
    {
        g_free (stripped);
        stripped = g_strdup (PN_VALUE_ROUTER_DEF_PATH);
    }

    if (strcmp (stripped, self->path) == 0)
    {
        g_free (stripped);
        return;
    }

    g_free (self->path);
    self->path = stripped;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PATH]);
}

static void
pn_value_router_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnValueRouter *self = PN_VALUE_ROUTER (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        g_value_set_int (value, self->n_outputs);
        break;
    case PROP_PATH:
        g_value_set_string (value, self->path);
        break;
    case PROP_RULES:
        g_value_take_string (value, rules_to_json (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_value_router_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnValueRouter *self = PN_VALUE_ROUTER (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        value_router_set_outputs (self, g_value_get_int (value));
        break;
    case PROP_PATH:
        value_router_set_path (self, g_value_get_string (value));
        break;
    case PROP_RULES:
        rules_from_json (self, g_value_get_string (value));
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_value_router_finalize (GObject *object)
{
    PnValueRouter *self = PN_VALUE_ROUTER (object);

    g_free (self->path);

    G_OBJECT_CLASS (pn_value_router_parent_class)->finalize (object);
}

static void
pn_value_router_class_init (PnValueRouterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_value_router_get_property;
    object_class->set_property = pn_value_router_set_property;
    object_class->finalize     = pn_value_router_finalize;
    node_class->receive        = pn_value_router_receive;
    /* build_class_tab installed by the gui tier (pn_value_router_gui_install). */

    node_class->class_name     = "Value Router";
    node_class->icon           = "\xef\x87\xa0";  /* fa-share-alt U+F1E0 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Gate";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of outputs, each with its own rule.",
            PN_VALUE_ROUTER_MIN_OUTPUTS, PN_VALUE_ROUTER_MAX_OUTPUTS,
            PN_VALUE_ROUTER_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    props[PROP_PATH] = g_param_spec_string (
            "path", "Path",
            "Data-bag member holding the number the rules test, \"value\" "
            "by default. A dotted path (foo.bar) reaches into nested "
            "objects.",
            PN_VALUE_ROUTER_DEF_PATH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    props[PROP_RULES] = g_param_spec_string (
            "rules", "Rules",
            "JSON array of rules, one per output: {\"op\": \"<\", \"<=\", "
            "\"==\", \"!=\", \">=\" or \">\", \"value\": N}, {\"op\": "
            "\"range\", \"low\": A, \"high\": B} or {\"op\": \"else\"}. A "
            "message leaves by the first output whose rule matches; "
            "unmatched messages are dropped.",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_value_router_init (PnValueRouter *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->n_outputs = PN_VALUE_ROUTER_DEF_OUTPUTS;
    self->path      = g_strdup (PN_VALUE_ROUTER_DEF_PATH);

    pn_node_set_class_name (node, "Value Router");
    pn_node_set_icon       (node, "\xef\x87\xa0");  /* fa-share-alt U+F1E0 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_n_outputs  (node, self->n_outputs);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnValueRouter *
pn_value_router_new (void)
{
    return g_object_new (PN_TYPE_VALUE_ROUTER, NULL);
}

PnValueRouterOp
pn_value_router_get_rule (
        PnValueRouter *self,
        gint           index,
        gdouble       *a,
        gdouble       *b)
{
    const Rule *r;

    g_return_val_if_fail (PN_IS_VALUE_ROUTER (self), PN_VALUE_ROUTER_OP_UNUSED);
    g_return_val_if_fail (index >= 0 && index < PN_VALUE_ROUTER_MAX_OUTPUTS,
                          PN_VALUE_ROUTER_OP_UNUSED);

    r = &self->rules[index];
    if (a != NULL)
        *a = r->a;
    if (b != NULL)
        *b = r->b;
    return r->op;
}

void
pn_value_router_set_rule (
        PnValueRouter  *self,
        gint            index,
        PnValueRouterOp op,
        gdouble         a,
        gdouble         b)
{
    g_return_if_fail (PN_IS_VALUE_ROUTER (self));
    g_return_if_fail (index >= 0 && index < PN_VALUE_ROUTER_MAX_OUTPUTS);

    if (store_rule (self, index, op, a, b))
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_RULES]);
}
