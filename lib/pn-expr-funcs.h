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

/* Three or more arguments, and the only shape that can serve a function
 * whose arity is a RANGE: @args holds exactly @n_args values, and @n_args
 * is within the row's min..max (TODO #83.2). */
typedef gdouble (*PnExprNaryFn)   (const gdouble *args, gint n_args);

/* A row's ARGUMENT CHECK, and the only thing in the whole table that can
 * make a call FAIL rather than return a number (TODO #83.14).  The NaN
 * and domain policy (83.18) says a result mathematics does not define is
 * a VALUE — sqrt(-1) is NaN and travels on — but it draws one line: an
 * argument wrong in KIND rather than out of range has no answer this
 * language can carry.  "The factorial of a half" is not a large number
 * or a NaN, it is a different function, so `factorial`, `gcd` and `lcm`
 * carry a check and nothing else does.
 *
 * Returns %NULL when @args are acceptable, or a STATIC message saying
 * what is wrong with them — rendered as "<name>: <message>", so the
 * message is the predicate and not the sentence.  It runs PER ELEMENT
 * of a vector call, before the implementation sees the arguments.
 */
typedef const gchar *(*PnExprCheckFn) (const gdouble *args, gint n_args);

typedef struct
{
    const gchar    *name;
    gint            min_arity;  /* fewest arguments the call may take */
    gint            max_arity;  /* most; equal to @min_arity for a fixed
                                 * arity, larger for `log(x[, base])`  */
    PnExprUnaryFn   fn1;        /* fixed arity 1 */
    PnExprBinaryFn  fn2;        /* fixed arity 2 */
    PnExprNaryFn    fnN;        /* arity 3+, or any range */
    PnExprCheckFn   check;      /* argument check, or %NULL for the
                                 * overwhelming majority that cannot
                                 * refuse an argument at all */
} PnExprFunc;

/* Use these rather than writing a row out: a function is meant to cost
 * ONE LINE, and these shapes are what keep it one line as the struct
 * grows.  FNR is the ranged form — FNR ("log", 1, 2, expr_log). */
#define PN_EXPR_FN1(name_, fn_)            { name_, 1, 1, fn_, NULL, NULL, \
                                             NULL }
#define PN_EXPR_FN2(name_, fn_)            { name_, 2, 2, NULL, fn_, NULL, \
                                             NULL }
#define PN_EXPR_FNN(name_, arity_, fn_)    { name_, arity_, arity_, \
                                             NULL, NULL, fn_, NULL }
#define PN_EXPR_FNR(name_, lo_, hi_, fn_)  { name_, lo_, hi_, NULL, NULL, \
                                             fn_, NULL }

/* The CHECKED forms (TODO #83.14).  Three rows in the table use them and
 * the shape is deliberately awkward to reach for: a function that can
 * refuse an argument stops a sheet, so adding one is a decision. */
#define PN_EXPR_FN1C(name_, fn_, check_)   { name_, 1, 1, fn_, NULL, NULL, \
                                             check_ }
#define PN_EXPR_FN2C(name_, fn_, check_)   { name_, 2, 2, NULL, fn_, NULL, \
                                             check_ }

/* The most arguments any call may take.  This used to be a
 * REPRESENTATIONAL limit — TODO #81.2 put argument one in
 * PnExprNode.left and argument two in .right, and a third had nowhere to
 * live — but 83.1 gave arguments 2..N a chain of PN_EXPR_NODE_ARG nodes,
 * so the tree no longer cares.  What is left is a POLICY: every call in
 * this language has a fixed, declared arity so the parser can check it
 * as the expression is typed (81.3), and a bound keeps that promise
 * cheap.  The language could do more and elected not to (83.21) — raise
 * this when a row needs it, and nothing else has to change. */
#define PN_EXPR_MAX_ARITY 4

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
 * pn_expr_func_arity_phrase:
 * @fn: a table entry
 *
 * The arity of @fn in words — "1 argument", "2 arguments", "1 or 2
 * arguments", "2 to 4 arguments" — for an error message.  It exists so
 * the parser's complaint (at parse time, about a typed program) and the
 * evaluator's (about a hand-built tree) cannot drift apart.
 *
 * Returns: (transfer full): a newly-allocated string.
 */
gchar *pn_expr_func_arity_phrase (const PnExprFunc *fn);

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
