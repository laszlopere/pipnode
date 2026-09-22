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

#ifndef PN_EXPR_FUNCS_H
#define PN_EXPR_FUNCS_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  The calculator language's built-in names — TODO #81.4              */
/*                                                                     */
/*  One table, read by both halves of the language: pn-expr-parser.c   */
/*  needs the ARITY so a call with the wrong number of arguments is a  */
/*  parse error the moment it is typed, and pn-var-store.c needs the   */
/*  IMPLEMENTATION to evaluate it.  Keeping them together is the whole */
/*  point — a function added in one place and forgotten in the other   */
/*  is exactly the drift this file exists to prevent, and the parser   */
/*  must not start depending on the var store to learn an arity.       */
/*                                                                     */
/*  This header is deliberately NOT installed.  pn-expr-parser.h is    */
/*  public API that reaches plugins through pipnode.h, so anything     */
/*  declared there is an ABI commitment; the table is an internal      */
/*  detail of core and stays one.                                      */
/* ------------------------------------------------------------------ */

typedef gdouble (*PnExprUnaryFn)  (gdouble x);
typedef gdouble (*PnExprBinaryFn) (gdouble x, gdouble y);

typedef struct
{
    const gchar    *name;
    gint            arity;  /* 1 or 2 — how many arguments the call takes */
    PnExprUnaryFn   fn1;    /* set when @arity == 1 */
    PnExprBinaryFn  fn2;    /* set when @arity == 2 */
} PnExprFunc;

/* The largest @arity in the table.  The AST represents a call's first
 * argument in PnExprNode.left and its second in .right (TODO #81.2), so
 * the representation itself caps the language here; raising this means
 * finding somewhere for a third argument to live. */
#define PN_EXPR_MAX_ARITY 2

/**
 * pn_expr_func_lookup:
 * @name: a function name as it appears in a program
 *
 * Returns: (nullable): the table entry for @name, or %NULL when the
 *   language has no function by that name.  The entry is static and
 *   lives for the life of the process.
 */
const PnExprFunc *pn_expr_func_lookup (const gchar *name);

/**
 * pn_expr_func_count:
 *
 * The number of functions in the table.  Together with
 * pn_expr_func_nth() this exists so a TEST can walk the whole table and
 * insist that every row was exercised (TODO #81.11): adding a row is
 * meant to be the only work a new function needs, and that is only true
 * if forgetting its assertion FAILS rather than passing quietly.
 *
 * Returns: the row count.
 */
gsize pn_expr_func_count (void);

/**
 * pn_expr_func_nth:
 * @index: a row index below pn_expr_func_count()
 *
 * Returns: (nullable): the @index'th table entry, or %NULL when @index
 *   is out of range.  Static, like pn_expr_func_lookup()'s result.
 */
const PnExprFunc *pn_expr_func_nth (gsize index);

/**
 * pn_expr_constant_lookup:
 * @name:      an identifier as it appears in a program
 * @out_value: (out) (optional): receives the constant's value on success
 *
 * The language's named constants (`pi`, `e`).  They are NOT bindings:
 * the evaluator consults this table only AFTER the variables, as a
 * fallback (TODO #81.6), so a data-bag member actually called `pi`
 * still shadows the constant and pn_var_store_clear() can never lose
 * one.
 *
 * Returns: %TRUE when @name is a constant (and writes @out_value).
 */
gboolean pn_expr_constant_lookup (const gchar *name, gdouble *out_value);

G_END_DECLS

#endif /* PN_EXPR_FUNCS_H */
