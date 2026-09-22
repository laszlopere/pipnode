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
#include "pn-expr-bind.h"
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

/** Write one program-assigned name onto the outgoing message, so a
 *  multi-statement expression can emit several computed fields, not just
 *  data.value.  A vector assignment is written as a `$pnvector` marker;
 *  a scalar as a plain numeric member. */
static void
surface_assignment (const gchar       *name,
                    const PnExprValue *value,
                    gpointer           user_data)
{
    PnMessage *message = PN_MESSAGE (user_data);

    if (value->vec != NULL)
        pn_message_set_vector (message, name, value->vec);
    else
        pn_message_set_double (message, name, value->scalar);
}

static void
pn_expression2_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnExpression2 *self   = PN_EXPRESSION2 (node);
    PnExprValue    result = { NULL, 0.0 };
    GError        *error  = NULL;

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
     * "value1 + value2" resolve automatically.  The rule itself — the
     * latched headline values, then this message's other numeric members
     * suffixed with the arriving input number — lives in pn-expr-bind.c,
     * shared with the figure (80.8c). */
    pn_var_store_clear (self->vars);
    pn_expr_bind_collated (node, message, pn_expr_bind_to_store, self->vars);

    if (pn_var_store_evaluate_value (self->vars, self->ast, &result, &error))
    {
        gchar *out = pn_var_store_value_to_string (&result);

        /* Surface any names the program assigned first, then let the
         * final expression's result own the reserved data.value. */
        pn_var_store_foreach_assignment_value (self->vars, surface_assignment,
                                               message);

        if (result.vec != NULL)
            pn_message_set_vector (message, "value", result.vec);
        else
            pn_message_set_double (message, "value", result.scalar);
        pn_message_set_boolean (message, "success", TRUE);
        pn_message_set_string  (message, "output",  out);

        g_free (out);
        pn_expr_value_clear (&result);
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
            "(true) or 0.0 (false). Integer operators: % (modulo), "
            "& | ^ << >> and ~ (bitwise, on the whole-number part). "
            "Functions: trigonometry (radians) sin, cos, tan, asin, "
            "acos, atan, cot, sec, csc, atan2(y, x); angles "
            "degrees(x), radians(x); hyperbolic sinh, cosh, tanh, "
            "asinh, acosh, atanh; powers, roots and logs sqrt, cbrt, "
            "exp, exp2, expm1, pow(x, y) (^ is XOR, not a power), "
            "hypot(x, y), log(x) (natural), ln, log(x, base), log10, "
            "log2, log1p; rounding and remainder abs, floor, ceil, "
            "round (halves away from zero), trunc, sign, fmod(x, y), "
            "copysign(x, y); whole numbers factorial(n) (0 to 170), "
            "gcd(a, b), lcm(a, b) — the only three names that refuse "
            "a fraction, a nan or an infinity instead of answering; "
            "percentages and money pct(x, p), pct_change(old, new), "
            "bps(x, b), compound(principal, rate, periods), "
            "fv(pmt, rate, nper), pv(pmt, rate, nper), "
            "pmt(pv, rate, nper) — the rate is per period and a zero "
            "rate is a limit, not a division; "
            "testing isnan, isinf, isfinite; curves "
            "sinc, erf, erfc, j0, j1; choosing and easing min, max, "
            "clamp(x, lo, hi), if(cond, a, b) (a true select: the arm "
            "not chosen cannot poison the result), lerp(a, b, t), "
            "step(edge, x), smoothstep(lo, hi, x). An undefined "
            "result is a value, not an "
            "error: sqrt(-1) is nan and log(0) is -inf, and both "
            "travel on — the three whole-number names are the only "
            "exception, and they stop the sheet rather than guess. "
            "Constants: pi and e, which a bound member of "
            "the same name shadows. "
            "Write several newline-separated "
            "statements to compute step by step; `name = expr` binds a "
            "variable for later lines and is also written to the outgoing "
            "message, and data.value is the last statement's value. A "
            "$pnvector member binds as a vector: scalars broadcast over it, "
            "two vectors combine elementwise, functions map over each "
            "element, and a comparison reduces to a single 1.0/0.0 (true "
            "iff every element passes).",
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
