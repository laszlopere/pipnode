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

#ifndef PN_VAR_STORE_H
#define PN_VAR_STORE_H

#include <glib-object.h>

#include "pn-expr-parser.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Errors                                                             */
/* ------------------------------------------------------------------ */

#define PN_VAR_STORE_ERROR (pn_var_store_error_quark ())

GQuark pn_var_store_error_quark (void);

typedef enum
{
    PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE, /* AST names a variable not set    */
    PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION, /* AST calls an unknown function   */
    PN_VAR_STORE_ERROR_BAD_AST,          /* malformed / corrupt AST node    */
} PnVarStoreError;

/* ------------------------------------------------------------------ */
/*  PnVarStore                                                         */
/*                                                                     */
/*  Holds a set of named numeric variables and evaluates a #PnExprNode */
/*  AST against them.  The store is the symbol table the recursive     */
/*  evaluator (pn_var_store_evaluate()) reads variables from; built-in */
/*  functions (sin, cos, log, …) are resolved by the evaluator itself. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_VAR_STORE (pn_var_store_get_type ())

G_DECLARE_FINAL_TYPE (PnVarStore, pn_var_store, PN, VAR_STORE, GObject)

PnVarStore *pn_var_store_new (void);

/**
 * pn_var_store_set:
 * @self:  the store
 * @name:  variable name
 * @value: value to bind
 *
 * Binds @name to @value, replacing any previous binding.
 */
void pn_var_store_set (PnVarStore *self, const gchar *name, gdouble value);

/**
 * pn_var_store_assign:
 * @self:  the store
 * @name:  variable name
 * @value: value to bind
 *
 * Like pn_var_store_set(), but additionally records @name as an
 * assignment made during the current evaluation.  The evaluator calls
 * this for `name = expr` statements so a caller can later tell program
 * outputs (visited by pn_var_store_foreach_assignment()) apart from the
 * input variables it pre-bound with pn_var_store_set().  The record is
 * dropped by pn_var_store_clear() along with the binding.
 */
void pn_var_store_assign (PnVarStore *self, const gchar *name, gdouble value);

/**
 * PnVarStoreForeachFunc:
 * @name:      the bound name
 * @value:     its current value
 * @user_data: caller data passed through
 *
 * Callback for pn_var_store_foreach_assignment().
 */
typedef void (*PnVarStoreForeachFunc) (const gchar *name,
                                       gdouble      value,
                                       gpointer     user_data);

/**
 * pn_var_store_foreach_assignment:
 * @self:      the store
 * @func:      (scope call): called once per assigned name
 * @user_data: passed through to @func
 *
 * Invokes @func for every name bound by an assignment since the last
 * pn_var_store_clear() (in unspecified order).
 */
void pn_var_store_foreach_assignment (PnVarStore            *self,
                                      PnVarStoreForeachFunc  func,
                                      gpointer               user_data);

/**
 * pn_var_store_get:
 * @self:      the store
 * @name:      variable name
 * @out_value: (out) (optional): receives the bound value on success
 *
 * Returns: %TRUE when @name is bound (and writes @out_value), %FALSE
 *   otherwise.
 */
gboolean pn_var_store_get (PnVarStore *self, const gchar *name, gdouble *out_value);

/**
 * pn_var_store_clear:
 * @self: the store
 *
 * Drops every binding.
 */
void pn_var_store_clear (PnVarStore *self);

/**
 * pn_var_store_evaluate:
 * @self:      the store providing variable bindings
 * @node:      AST to evaluate
 * @out_value: (out): receives the result on success
 * @error:     (out) (optional): set on failure
 *
 * Recursively walks @node, resolving variables against @self and
 * dispatching function calls (sin, cos, tan, log, log10, exp, sqrt,
 * abs, floor, ceil) to the C math library.  Comparison operators
 * (`< > <= >= == !=`) evaluate to 1.0 (true) or 0.0 (false).  An
 * assignment statement binds its name (via pn_var_store_assign()) and
 * evaluates to the bound value; a statement sequence evaluates each in
 * order and yields the value of the last.  An unbound variable or an
 * unknown function name fails the whole evaluation.
 *
 * Returns: %TRUE on success (and writes @out_value); %FALSE with
 *   @error set otherwise.
 */
gboolean pn_var_store_evaluate (PnVarStore       *self,
                                const PnExprNode *node,
                                gdouble          *out_value,
                                GError          **error);

G_END_DECLS

#endif /* PN_VAR_STORE_H */
