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

#include "pn-expression.h"
#include "pn-expr-parser.h"
#include "pn-var-store.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>

struct _PnExpression
{
    PnNode parent_instance;

    gchar        *expression;  /* source text of the configured expression */
    PnExprNode   *ast;         /* compiled AST, or NULL when invalid/empty  */
    gchar        *parse_error; /* why @ast is NULL, for the error path      */

    PnExprParser *parser;      /* reused for every recompile                */
    PnVarStore   *vars;        /* reused per message; cleared each receive  */
};

G_DEFINE_TYPE (PnExpression, pn_expression, PN_TYPE_NODE)

enum {
    PROP_0,
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
expr_recompile (PnExpression *self)
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

static void
pn_expression_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnExpression *self = PN_EXPRESSION (node);
    JsonObject   *data;
    gdouble       result;
    GError       *error = NULL;

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

    /* Bind every numeric member of the incoming bag as a variable, so
     * the expression can reference `value` and any sibling numeric
     * field by name. */
    pn_var_store_clear (self->vars);
    data = pn_message_get_data (message);
    if (data != NULL)
    {
        JsonObjectIter  iter;
        const gchar    *member;
        JsonNode       *val;

        json_object_iter_init (&iter, data);
        while (json_object_iter_next (&iter, &member, &val))
        {
            if (val != NULL && JSON_NODE_HOLDS_VALUE (val))
            {
                GType vt = json_node_get_value_type (val);
                if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
                    pn_var_store_set (self->vars, member,
                                      json_node_get_double (val));
            }
        }
    }

    if (pn_var_store_evaluate (self->vars, self->ast, &result, &error))
    {
        gchar *out = g_strdup_printf ("%g", result);

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
pn_expression_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnExpression *self = PN_EXPRESSION (object);

    switch (prop_id)
    {
    case PROP_EXPRESSION:
        g_value_set_string (value, self->expression);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_expression_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnExpression *self = PN_EXPRESSION (object);

    switch (prop_id)
    {
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
pn_expression_finalize (GObject *object)
{
    PnExpression *self = PN_EXPRESSION (object);

    g_clear_pointer (&self->expression,  g_free);
    g_clear_pointer (&self->parse_error, g_free);
    g_clear_pointer (&self->ast,         pn_expr_node_free);
    g_clear_object  (&self->parser);
    g_clear_object  (&self->vars);

    G_OBJECT_CLASS (pn_expression_parent_class)->finalize (object);
}

static void
pn_expression_class_init (PnExpressionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_expression_get_property;
    object_class->set_property = pn_expression_set_property;
    object_class->finalize     = pn_expression_finalize;
    node_class->receive        = pn_expression_receive;

    node_class->class_name     = "Calculator";
    node_class->icon           = "\xef\x87\xac";  /* fa-calculator U+F1EC */
    node_class->color          = (PnColor){ 0.55, 0.45, 0.80, 1.0 };
    node_class->category       = "Filters/Expressions";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_EXPRESSION] = g_param_spec_string (
            "expression", "Expression",
            "Algebraic expression evaluated on each message, e.g. "
            "\"(12.3 * value) + 1\". Numeric data-bag members are bound "
            "as variables (incl. data.value as `value`); the result is "
            "written to data.value. Functions: sin, cos, tan, log, "
            "log10, exp, sqrt, abs, floor, ceil.",
            "value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_EXPRESSION]);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema (Phase 7.5): the node has the single
     * `expression` body, so replace the auto property tab with one named
     * "Expression" holding a single full-width multiline editor — no
     * redundant "Expression :" row label, the editor fills the page.
     * This is GTK-free data on the class; the GUI tier's renderer builds
     * the same tab the deleted pn-expression-gui.c build_class_tab did,
     * so this node no longer needs a -gui.c companion at all. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Expression");
        pn_settings_schema_row (schema, "expression", PN_EDITOR_MULTILINE);
        pn_settings_schema_row_flags (schema, "expression",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_expression_init (PnExpression *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  purple = { 0.55, 0.45, 0.80, 1.0 };

    self->parser = pn_expr_parser_new ();
    self->vars   = pn_var_store_new ();

    /* Mirror the property default ("value") and compile it so a freshly
     * dropped node already passes data.value straight through. */
    self->expression = g_strdup ("value");
    expr_recompile (self);

    pn_node_set_class_name (node, "Calculator");
    pn_node_set_icon       (node, "\xef\x87\xac");  /* fa-calculator U+F1EC */
    pn_node_set_color      (node, &purple);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnExpression *
pn_expression_new (void)
{
    return g_object_new (PN_TYPE_EXPRESSION, NULL);
}
