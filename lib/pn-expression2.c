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

#include "pn-expression2.h"
#include "pn-expr-parser.h"
#include "pn-var-store.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>

#define PN_EXPRESSION2_MIN_INPUTS 2
#define PN_EXPRESSION2_MAX_INPUTS 8
#define PN_EXPRESSION2_DEF_INPUTS 2

struct _PnExpression2
{
    PnNode parent_instance;

    gint          n_inputs;    /* configurable input count, 2..8           */
    gchar        *expression;  /* source text of the configured expression */
    PnExprNode   *ast;         /* compiled AST, or NULL when invalid/empty  */
    gchar        *parse_error; /* why @ast is NULL, for the error path      */

    PnExprParser *parser;      /* reused for every recompile                */
    PnVarStore   *vars;        /* reused per message; rebuilt each receive  */
};

G_DEFINE_TYPE (PnExpression2, pn_expression2, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INPUTS,
    PROP_EXPRESSION,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Compilation                                                        */
/* ------------------------------------------------------------------ */

/** (Re)parse self->expression into self->ast.  On any failure the AST
 *  is left NULL and a human-readable reason is stashed in
 *  self->parse_error for the receive path to report. */
static void
expr_recompile (PnExpression2 *self)
{
    GError *error = NULL;

    g_clear_pointer (&self->ast, pn_expr_node_free);
    g_clear_pointer (&self->parse_error, g_free);

    if (self->expression == NULL || *self->expression == '\0')
    {
        self->parse_error = g_strdup ("no expression set");
        return;
    }

    self->ast = pn_expr_parser_parse (self->parser, self->expression, &error);
    if (self->ast == NULL)
    {
        self->parse_error = g_strdup (error != NULL ? error->message
                                                    : "parse error");
        g_clear_error (&error);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** TRUE if @name is the display name of one of @node's inputs — i.e. a
 *  member the core's input-value collation injected into the data bag.
 *  Used to tell core-injected per-input headline values (bound by their
 *  own name) apart from this message's own sibling fields (suffixed). */
static gboolean
is_input_name (PnNode *node, gint n, const gchar *name)
{
    gint i;
    for (i = 0; i < n; i++)
        if (g_strcmp0 (pn_node_get_input_name (node, i), name) == 0)
            return TRUE;
    return FALSE;
}

/** Write one program-assigned name onto the outgoing message as a
 *  numeric data-bag member, so a multi-statement expression can emit
 *  several computed fields, not just data.value. */
static void
surface_assignment (const gchar *name,
                    gdouble      value,
                    gpointer     user_data)
{
    pn_message_set_double (PN_MESSAGE (user_data), name, value);
}

static void
pn_expression2_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnExpression2  *self = PN_EXPRESSION2 (node);
    gint            idx  = pn_node_current_input ();
    gint            n    = pn_node_get_n_inputs (node);
    JsonObject     *data;
    JsonObjectIter  iter;
    const gchar    *name;
    JsonNode       *val;
    gdouble         result;
    GError         *error = NULL;
    gint            i;

    if (idx < 0)
        idx = 0;
    else if (idx >= n)
        idx = n - 1;

    /* No usable expression: forward the message flagged as failed so a
     * downstream chain keeps flowing rather than stalling. */
    if (self->ast == NULL)
    {
        const gchar *why = self->parse_error != NULL ? self->parse_error
                                                     : "no expression set";
        pn_message_set_boolean (message, "success", FALSE);
        pn_message_set_string  (message, "error",  why);
        pn_message_set_string  (message, "output", why);
        pn_node_emit_message (node, message);
        return;
    }

    /* Build the variable set from the (already collated) data bag.  The
     * core's input-value collation has latched each input's last
     * /data/value and injected them under the inputs' display names, so
     * value1/value2 (or whatever the inputs are named) are present even
     * for inputs that did not just fire — that is what makes
     * "value1 + value2" resolve automatically. */
    pn_var_store_clear (self->vars);
    data = pn_message_get_data (message);

    /* (1) The latched per-input headline values, bound under their input
     *     names exactly as the core injected them. */
    for (i = 0; i < n; i++)
    {
        const gchar *nm = pn_node_get_input_name (node, i);
        JsonNode    *vn = (data != NULL) ? json_object_get_member (data, nm)
                                         : NULL;
        if (vn != NULL && JSON_NODE_HOLDS_VALUE (vn))
        {
            GType vt = json_node_get_value_type (vn);
            if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
                pn_var_store_set (self->vars, nm, json_node_get_double (vn));
        }
    }

    /* (2) Sibling numeric fields from *this* message only, suffixed with
     *     the 1-based arriving input number (data.temp -> temp1/temp2,
     *     ...).  Unlike the headline values these are not latched across
     *     inputs — only the message actually being processed contributes
     *     them.  Skip the input-name members (the latched values bound
     *     above) and the bare reserved "value" (already surfaced under
     *     its input name). */
    if (data != NULL)
    {
        json_object_iter_init (&iter, data);
        while (json_object_iter_next (&iter, &name, &val))
        {
            GType vt;
            gchar *vname;

            if (val == NULL || !JSON_NODE_HOLDS_VALUE (val))
                continue;
            vt = json_node_get_value_type (val);
            if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
                continue;
            if (g_strcmp0 (name, "value") == 0)
                continue;
            if (is_input_name (node, n, name))
                continue;

            vname = g_strdup_printf ("%s%d", name, idx + 1);
            pn_var_store_set (self->vars, vname, json_node_get_double (val));
            g_free (vname);
        }
    }

    if (pn_var_store_evaluate (self->vars, self->ast, &result, &error))
    {
        gchar *out = g_strdup_printf ("%g", result);

        /* Surface any names the program assigned first, then let the
         * final expression's result own the reserved data.value. */
        pn_var_store_foreach_assignment (self->vars, surface_assignment,
                                         message);

        pn_message_set_double  (message, "value",   result);
        pn_message_set_boolean (message, "success", TRUE);
        pn_message_set_string  (message, "output",  out);

        g_free (out);
    }
    else
    {
        const gchar *why = error != NULL ? error->message
                                         : "evaluation failed";
        /* Leave data.value untouched on failure. */
        pn_message_set_boolean (message, "success", FALSE);
        pn_message_set_string  (message, "error",  why);
        pn_message_set_string  (message, "output", why);
        g_clear_error (&error);
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_expression2_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnExpression2 *self = PN_EXPRESSION2 (object);

    switch (prop_id)
    {
    case PROP_INPUTS:
        g_value_set_int (value, self->n_inputs);
        break;
    case PROP_EXPRESSION:
        g_value_set_string (value, self->expression);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_expression2_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnExpression2 *self = PN_EXPRESSION2 (object);

    switch (prop_id)
    {
    case PROP_INPUTS:
        {
            gint v = g_value_get_int (value);
            if (v != self->n_inputs)
            {
                self->n_inputs = v;
                /* Resize the live input ports; the core grows/shrinks its
                 * per-input collation latches to match.  Repaint so the
                 * worksheet redraws the stacked input rows at the new
                 * count (re-open the dialog to refresh its Input names). */
                pn_node_set_n_inputs    (PN_NODE (self), v);
                g_object_notify_by_pspec (object, props[PROP_INPUTS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_EXPRESSION:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->expression, s) != 0)
            {
                g_free (self->expression);
                self->expression = g_strdup (s != NULL ? s : "");
                expr_recompile (self);
                g_object_notify_by_pspec (object, props[PROP_EXPRESSION]);
            }
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
pn_expression2_finalize (GObject *object)
{
    PnExpression2 *self = PN_EXPRESSION2 (object);

    g_clear_pointer (&self->expression,  g_free);
    g_clear_pointer (&self->parse_error, g_free);
    g_clear_pointer (&self->ast,         pn_expr_node_free);
    g_clear_object  (&self->parser);
    g_clear_object  (&self->vars);

    G_OBJECT_CLASS (pn_expression2_parent_class)->finalize (object);
}

static void
pn_expression2_class_init (PnExpression2Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property  = pn_expression2_get_property;
    object_class->set_property  = pn_expression2_set_property;
    object_class->finalize      = pn_expression2_finalize;
    node_class->receive         = pn_expression2_receive;

    node_class->class_name     = "Calculator 2";
    node_class->icon           = "\xef\x87\xac";  /* fa-calculator U+F1EC */
    node_class->color          = (PnColor){ 0.55, 0.45, 0.80, 1.0 };
    node_class->category       = "Filters/Expressions";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_INPUTS] = g_param_spec_int (
            "inputs", "Inputs",
            "How many inputs the node has. Each input's last data.value is "
            "remembered and bound in the expression under that input's name "
            "(value1 … valueN by default, or whatever the inputs are "
            "renamed to).",
            PN_EXPRESSION2_MIN_INPUTS, PN_EXPRESSION2_MAX_INPUTS,
            PN_EXPRESSION2_DEF_INPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_EXPRESSION] = g_param_spec_string (
            "expression", "Expression",
            "Algebraic expression evaluated on each message, e.g. "
            "\"value1 + value2\". Each input's last data.value is "
            "remembered and bound under that input's name — `value1` and "
            "`value2` by default, or whatever the inputs are renamed to — "
            "so both inputs are available even when only one just fired. "
            "Any other numeric member of the message being processed is "
            "bound with the arriving input's number as a suffix (a sibling "
            "data.temp as `temp1`/`temp2`); unlike the headline value these "
            "siblings are not remembered across inputs. The result is "
            "written to data.value. Comparisons (< > <= >= == !=) yield 1.0 "
            "(true) or 0.0 (false). Functions: sin, cos, tan, log, log10, "
            "exp, sqrt, abs, floor, ceil. Write several newline-separated "
            "statements to compute step by step; `name = expr` binds a "
            "variable for later lines and is also written to the outgoing "
            "message, and data.value is the last statement's value.",
            "value1 + value2",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_EXPRESSION]);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema (Phase 7.5): single full-width
     * multiline editor on a tab named "Expression2", replacing the
     * deleted pn-expression2-gui.c build_class_tab.  See pn-expression.c
     * for the rationale; this node likewise needs no -gui.c companion. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Expression2");
        pn_settings_schema_row (schema, "expression", PN_EDITOR_MULTILINE);
        pn_settings_schema_row_flags (schema, "expression",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_expression2_init (PnExpression2 *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  purple = { 0.55, 0.45, 0.80, 1.0 };

    self->parser   = pn_expr_parser_new ();
    self->vars     = pn_var_store_new ();
    self->n_inputs = PN_EXPRESSION2_DEF_INPUTS;

    /* Mirror the property default and compile it so a freshly dropped
     * node is immediately usable. */
    self->expression = g_strdup ("value1 + value2");
    expr_recompile (self);

    pn_node_set_class_name (node, "Calculator 2");
    pn_node_set_icon       (node, "\xef\x87\xac");  /* fa-calculator U+F1EC */
    pn_node_set_color      (node, &purple);
    pn_node_set_n_inputs   (node, self->n_inputs);  /* 2..8, default 2 */
    pn_node_set_has_output (node, TRUE);
    /* Let the node dialog offer a spin for the input count on its
     * dedicated "Inputs" tab (alongside the editable per-input names),
     * rebuilding the name fields live as the count changes. */
    pn_node_set_input_count_property (node, "inputs");
    /* Let the core latch each input's /data/value and surface it under
     * the input's name (value1/value2 by default) on every message, so
     * the expression sees both inputs at once. */
    pn_node_set_collate_inputs (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnExpression2 *
pn_expression2_new (void)
{
    return g_object_new (PN_TYPE_EXPRESSION2, NULL);
}
