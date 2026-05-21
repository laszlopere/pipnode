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

#include "pn-var-store.h"

#include <math.h>

struct _PnVarStore
{
    GObject     parent_instance;

    /* name (gchar*, owned) -> value (gdouble*, owned) */
    GHashTable *vars;
};

G_DEFINE_TYPE (PnVarStore, pn_var_store, G_TYPE_OBJECT)

G_DEFINE_QUARK (pn-var-store-error, pn_var_store_error)

/* ------------------------------------------------------------------ */
/*  Built-in functions                                                 */
/*                                                                     */
/*  Each accepts one argument and maps onto a C math-library call.     */
/*  `log` is the natural logarithm (matching C's log()); `log10` is    */
/*  the base-10 form; `abs` maps to fabs() since values are doubles.   */
/* ------------------------------------------------------------------ */

typedef gdouble (*UnaryFn) (gdouble);

typedef struct
{
    const gchar *name;
    UnaryFn      fn;
} FnEntry;

static const FnEntry builtin_fns[] = {
    { "sin",   sin   },
    { "cos",   cos   },
    { "tan",   tan   },
    { "log",   log   },
    { "log10", log10 },
    { "exp",   exp   },
    { "sqrt",  sqrt  },
    { "abs",   fabs  },
    { "floor", floor },
    { "ceil",  ceil  },
};

static UnaryFn
lookup_fn (const gchar *name)
{
    gsize i;
    for (i = 0; i < G_N_ELEMENTS (builtin_fns); i++)
        if (g_strcmp0 (builtin_fns[i].name, name) == 0)
            return builtin_fns[i].fn;
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_var_store_finalize (GObject *object)
{
    PnVarStore *self = PN_VAR_STORE (object);

    g_clear_pointer (&self->vars, g_hash_table_unref);

    G_OBJECT_CLASS (pn_var_store_parent_class)->finalize (object);
}

static void
pn_var_store_class_init (PnVarStoreClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = pn_var_store_finalize;
}

static void
pn_var_store_init (PnVarStore *self)
{
    self->vars = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_free);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnVarStore *
pn_var_store_new (void)
{
    return g_object_new (PN_TYPE_VAR_STORE, NULL);
}

void
pn_var_store_set (PnVarStore  *self,
                  const gchar *name,
                  gdouble      value)
{
    gdouble *boxed;

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (name != NULL);

    boxed  = g_new (gdouble, 1);
    *boxed = value;
    g_hash_table_insert (self->vars, g_strdup (name), boxed);
}

gboolean
pn_var_store_get (PnVarStore  *self,
                  const gchar *name,
                  gdouble     *out_value)
{
    gdouble *boxed;

    g_return_val_if_fail (PN_IS_VAR_STORE (self), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);

    boxed = g_hash_table_lookup (self->vars, name);
    if (boxed == NULL)
        return FALSE;

    if (out_value != NULL)
        *out_value = *boxed;
    return TRUE;
}

void
pn_var_store_clear (PnVarStore *self)
{
    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_hash_table_remove_all (self->vars);
}

gboolean
pn_var_store_evaluate (PnVarStore       *self,
                       const PnExprNode *node,
                       gdouble          *out_value,
                       GError          **error)
{
    g_return_val_if_fail (PN_IS_VAR_STORE (self), FALSE);
    g_return_val_if_fail (out_value != NULL, FALSE);

    if (node == NULL)
    {
        g_set_error_literal (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_BAD_AST,
                             "empty expression tree");
        return FALSE;
    }

    switch (node->type)
    {
    case PN_EXPR_NODE_NUMBER:
        *out_value = node->number;
        return TRUE;

    case PN_EXPR_NODE_VARIABLE:
        {
            gdouble v;
            if (!pn_var_store_get (self, node->name, &v))
            {
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE,
                             "unknown variable '%s'", node->name);
                return FALSE;
            }
            *out_value = v;
            return TRUE;
        }

    case PN_EXPR_NODE_UNARY:
        {
            gdouble operand;
            if (!pn_var_store_evaluate (self, node->left, &operand, error))
                return FALSE;
            *out_value = (node->op == '-') ? -operand : operand;
            return TRUE;
        }

    case PN_EXPR_NODE_BINARY:
        {
            gdouble a, b;
            if (!pn_var_store_evaluate (self, node->left, &a, error))
                return FALSE;
            if (!pn_var_store_evaluate (self, node->right, &b, error))
                return FALSE;

            switch (node->op)
            {
            case '+': *out_value = a + b; return TRUE;
            case '-': *out_value = a - b; return TRUE;
            case '*': *out_value = a * b; return TRUE;
            case '/': *out_value = a / b; return TRUE;  /* IEEE inf/nan on /0 */
            default:
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_BAD_AST,
                             "unknown operator '%c'", node->op);
                return FALSE;
            }
        }

    case PN_EXPR_NODE_CALL:
        {
            gdouble  arg;
            UnaryFn  fn = lookup_fn (node->name);

            if (fn == NULL)
            {
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION,
                             "unknown function '%s'", node->name);
                return FALSE;
            }
            if (!pn_var_store_evaluate (self, node->left, &arg, error))
                return FALSE;

            *out_value = fn (arg);
            return TRUE;
        }

    default:
        g_set_error (error, PN_VAR_STORE_ERROR,
                     PN_VAR_STORE_ERROR_BAD_AST,
                     "unknown AST node type %d", (gint) node->type);
        return FALSE;
    }
}
