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
#include "pn-vector.h"

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
    PN_VAR_STORE_ERROR_TYPE_MISMATCH,    /* a vector reached a scalar sink  */
} PnVarStoreError;

/* ------------------------------------------------------------------ */
/*  PnExprValue                                                        */
/*                                                                     */
/*  A value flowing through the evaluator: either a plain scalar       */
/*  double, or a reference to an immutable #PnVector treated as a bag  */
/*  of numbers (TODO #43.7).  When @vec is %NULL the value is the      */
/*  scalar in @scalar; when @vec is non-%NULL the value is that vector */
/*  (and the struct OWNS one reference to it) and @scalar is unused.   */
/*  Release a value with pn_expr_value_clear().                        */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnVector *vec;    /* owned reference => vector value; %NULL => scalar */
    gdouble   scalar; /* the value when @vec is %NULL                     */
} PnExprValue;

/**
 * pn_expr_value_clear:
 * @value: (nullable): a value to release
 *
 * Drops @value's vector reference (if any) and resets it to the scalar
 * 0.0.  Safe to call with %NULL or on an already-cleared value.
 */
void pn_expr_value_clear (PnExprValue *value);

/**
 * pn_var_store_value_to_string:
 * @value: (nullable): a value to render
 *
 * Renders @value for the human-readable `output` field: a scalar as
 * "%g", a vector as a BOUNDED leading sample "[a, b, c, …] (N values)"
 * (never the whole buffer, so a megabyte vector cannot blow up the
 * string).
 *
 * Returns: (transfer full): a newly-allocated string.
 */
gchar *pn_var_store_value_to_string (const PnExprValue *value);

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
 * pn_var_store_set_vector:
 * @self:  the store
 * @name:  variable name
 * @vec:   the vector to bind; the store takes its own reference
 *
 * Binds @name to a vector value (a bag of numbers), replacing any
 * previous binding.  The expression nodes call this for a `$pnvector`
 * data-bag member so an expression can reference it by name.
 */
void pn_var_store_set_vector (PnVarStore *self,
                              const gchar *name,
                              PnVector    *vec);

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
 * PnVarStoreForeachValueFunc:
 * @name:      the bound name
 * @value:     its current value (scalar or vector); borrowed, do not free
 * @user_data: caller data passed through
 *
 * Value-aware callback for pn_var_store_foreach_assignment_value(): unlike
 * #PnVarStoreForeachFunc it can report a vector assignment, not just a
 * scalar.
 */
typedef void (*PnVarStoreForeachValueFunc) (const gchar       *name,
                                            const PnExprValue *value,
                                            gpointer           user_data);

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
 * pn_var_store_foreach_assignment_value:
 * @self:      the store
 * @func:      (scope call): called once per assigned name
 * @user_data: passed through to @func
 *
 * Like pn_var_store_foreach_assignment(), but reports each assignment as
 * a #PnExprValue so vector-valued assignments survive (the scalar variant
 * reports a vector assignment as 0.0).
 */
void pn_var_store_foreach_assignment_value (PnVarStore                 *self,
                                            PnVarStoreForeachValueFunc  func,
                                            gpointer                    user_data);

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
 * If the program's result is a VECTOR (e.g. the expression scales a
 * `$pnvector` input), this scalar-only entry point fails with
 * #PN_VAR_STORE_ERROR_TYPE_MISMATCH rather than crashing; callers that
 * accept vectors use pn_var_store_evaluate_value() instead.
 *
 * Returns: %TRUE on success (and writes @out_value); %FALSE with
 *   @error set otherwise.
 */
gboolean pn_var_store_evaluate (PnVarStore       *self,
                                const PnExprNode *node,
                                gdouble          *out_value,
                                GError          **error);

/**
 * pn_var_store_evaluate_value:
 * @self:      the store providing variable bindings
 * @node:      AST to evaluate
 * @out_value: (out caller-allocates): receives the result on success;
 *             the caller releases it with pn_expr_value_clear()
 * @error:     (out) (optional): set on failure
 *
 * Vector-aware evaluation (TODO #43.7).  Identical to
 * pn_var_store_evaluate() but the result may be a scalar OR a vector.
 * Vector operand rules:
 *  - scalar OP vector / vector OP scalar broadcast the scalar over every
 *    element (`2 * [a,b]` = `[2a,2b]`);
 *  - vector OP vector is elementwise; on a length mismatch the result is
 *    as long as the longer operand and the surviving tail element passes
 *    through VERBATIM (`[2,3] * [3,4,5]` = `[6,12,5]`);
 *  - functions (sin, cos, …) map element-by-element to a same-length
 *    vector;
 *  - comparisons ALWAYS reduce to a single scalar 0.0/1.0, true iff every
 *    compared element passes (all()-semantics; an unequal-length tail is
 *    vacuously true).
 *
 * Returns: %TRUE on success (and writes @out_value); %FALSE with
 *   @error set otherwise (and @out_value left cleared).
 */
gboolean pn_var_store_evaluate_value (PnVarStore       *self,
                                      const PnExprNode *node,
                                      PnExprValue      *out_value,
                                      GError          **error);

G_END_DECLS

#endif /* PN_VAR_STORE_H */
