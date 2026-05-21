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
#include "pn-node-dialog-helpers.h"

#include <json-glib/json-glib.h>

#define PN_EXPRESSION2_N_INPUTS 2

struct _PnExpression2
{
    PnNode parent_instance;

    gchar        *expression;  /* source text of the configured expression */
    PnExprNode   *ast;         /* compiled AST, or NULL when invalid/empty  */
    gchar        *parse_error; /* why @ast is NULL, for the error path      */

    PnExprParser *parser;      /* reused for every recompile                */
    PnVarStore   *vars;        /* reused per message; rebuilt each receive  */

    /* Latest numeric members harvested from each input's most recent
     * message: name (gchar*) -> value (gdouble*).  NULL until that
     * input has received a message.  Harvested up front (before the
     * incoming message is mutated) so a recompute never reads back a
     * value this node itself just wrote. */
    GHashTable   *inputs[PN_EXPRESSION2_N_INPUTS];
};

G_DEFINE_TYPE (PnExpression2, pn_expression2, PN_TYPE_NODE)

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

/** Snapshot every numeric member of @data into a fresh name->value
 *  table.  Returns an (always non-NULL) table the caller owns. */
static GHashTable *
harvest_numbers (JsonObject *data)
{
    GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);

    if (data != NULL)
    {
        JsonObjectIter  iter;
        const gchar    *name;
        JsonNode       *val;

        json_object_iter_init (&iter, data);
        while (json_object_iter_next (&iter, &name, &val))
        {
            if (val != NULL && JSON_NODE_HOLDS_VALUE (val))
            {
                GType vt = json_node_get_value_type (val);
                if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
                {
                    gdouble *boxed = g_new (gdouble, 1);
                    *boxed = json_node_get_double (val);
                    g_hash_table_insert (t, g_strdup (name), boxed);
                }
            }
        }
    }

    return t;
}

static void
pn_expression2_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnExpression2 *self = PN_EXPRESSION2 (node);
    gint           idx  = pn_node_current_input ();
    gdouble        result;
    GError        *error = NULL;
    guint          i;

    /* Stash this input's numeric members *before* we mutate the
     * message below, so value1/value2 stay the inputs' values. */
    if (idx < 0)
        idx = 0;
    else if (idx >= PN_EXPRESSION2_N_INPUTS)
        idx = PN_EXPRESSION2_N_INPUTS - 1;

    g_clear_pointer (&self->inputs[idx], g_hash_table_unref);
    self->inputs[idx] = harvest_numbers (pn_message_get_data (message));

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

    /* Bind every numeric member of each input, suffixed with the
     * 1-based input number: input 1 -> name1 (value1), input 2 ->
     * name2 (value2). */
    pn_var_store_clear (self->vars);
    for (i = 0; i < PN_EXPRESSION2_N_INPUTS; i++)
    {
        GHashTableIter  it;
        gpointer        k, v;

        if (self->inputs[i] == NULL)
            continue;

        g_hash_table_iter_init (&it, self->inputs[i]);
        while (g_hash_table_iter_next (&it, &k, &v))
        {
            gchar *vname = g_strdup_printf ("%s%u", (const gchar *) k, i + 1);
            pn_var_store_set (self->vars, vname, *(const gdouble *) v);
            g_free (vname);
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
/*  Settings dialog tab                                                */
/* ------------------------------------------------------------------ */

/** Replace the auto-generated property tab with a single full-width
 *  editor.  The node has one property and the tab is already titled
 *  "Expression2", so the per-row label the default tab would add is
 *  redundant — drop it and let the editor fill the tab. */
static GtkWidget *
pn_expression2_build_class_tab (
        PnNode    *node,
        GtkWindow *dialog_parent)
{
    GtkWidget *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget *editor;

    (void) dialog_parent;

    editor = pn_node_dialog_default_editor (G_OBJECT (node),
                                            props[PROP_EXPRESSION]);
    gtk_widget_set_hexpand (editor, TRUE);
    gtk_widget_set_vexpand (editor, TRUE);

    gtk_grid_attach (GTK_GRID (grid), editor, 0, 0, 2, 1);

    return grid;
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
    guint          i;

    g_clear_pointer (&self->expression,  g_free);
    g_clear_pointer (&self->parse_error, g_free);
    g_clear_pointer (&self->ast,         pn_expr_node_free);
    g_clear_object  (&self->parser);
    g_clear_object  (&self->vars);

    for (i = 0; i < PN_EXPRESSION2_N_INPUTS; i++)
        g_clear_pointer (&self->inputs[i], g_hash_table_unref);

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
    node_class->build_class_tab = pn_expression2_build_class_tab;

    node_class->class_name     = "Expression 2";
    node_class->icon           = "\xef\x87\xac";  /* fa-calculator U+F1EC */
    node_class->color          = (GdkRGBA){ 0.55, 0.45, 0.80, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_EXPRESSION] = g_param_spec_string (
            "expression", "Expression",
            "Algebraic expression evaluated on each message, e.g. "
            "\"value1 + value2\". Numeric members of input 1 are bound "
            "with a \"1\" suffix and input 2 with a \"2\" suffix "
            "(data.value as `value1`/`value2`, a sibling data.temp as "
            "`temp1`/`temp2`, ...); the result is written to data.value. "
            "Functions: sin, cos, tan, log, log10, exp, sqrt, abs, "
            "floor, ceil.",
            "value1 + value2",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_EXPRESSION]);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_expression2_init (PnExpression2 *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  purple = { 0.55, 0.45, 0.80, 1.0 };

    self->parser = pn_expr_parser_new ();
    self->vars   = pn_var_store_new ();

    /* Mirror the property default and compile it so a freshly dropped
     * node is immediately usable. */
    self->expression = g_strdup ("value1 + value2");
    expr_recompile (self);

    pn_node_set_class_name (node, "Expression 2");
    pn_node_set_icon       (node, "\xef\x87\xac");  /* fa-calculator U+F1EC */
    pn_node_set_color      (node, &purple);
    pn_node_set_n_inputs   (node, PN_EXPRESSION2_N_INPUTS);  /* two inputs */
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnExpression2 *
pn_expression2_new (void)
{
    return g_object_new (PN_TYPE_EXPRESSION2, NULL);
}
