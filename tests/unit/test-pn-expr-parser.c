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

/* Unit tests for PnExprParser.  Rather than assert on the raw AST shape
 * we parse an expression and evaluate it through a PnVarStore, so each
 * case checks the end-to-end meaning (precedence, associativity, unary
 * sign, parentheses, function calls, variables).  Parse failures are
 * checked directly via the NULL-plus-GError contract.  Headless. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-expr-parser.h"
#include "pn-var-store.h"
#include "pn-expr-funcs.h"

#include <math.h>
#include <string.h>

/* Parse @expr, evaluate against @vars, and return the numeric result.
 * Writes TRUE/FALSE to @ok for "parsed and evaluated cleanly".  The
 * parser instance carries no per-parse state, so one is reused. */
static gdouble
parse_eval (PnExprParser *p, PnVarStore *vars, const gchar *expr,
            gboolean *ok)
{
    GError     *err = NULL;
    PnExprNode *ast = pn_expr_parser_parse (p, expr, &err);
    gdouble     out = 0.0;

    *ok = FALSE;
    if (ast != NULL)
    {
        *ok = pn_var_store_evaluate (vars, ast, &out, &err);
        pn_expr_node_free (ast);
    }
    g_clear_error (&err);
    return out;
}

static void
test_arithmetic_and_precedence (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "1 + 2 * 3", &ok), 7.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "(1 + 2) * 3", &ok), 9.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "10 / 4", &ok), 2.5, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 - 3 - 4", &ok), -5.0, 1e-9);  /* left-assoc */
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

static void
test_unary_sign (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "-3 + 5", &ok), 2.0, 1e-9);
    PN_CHECK (ok);
    /* Unary minus binds tighter than '*'. */
    PN_CHECK_NEAR (parse_eval (p, vars, "2 * -4", &ok), -8.0, 1e-9);
    PN_CHECK (ok);
    /* Unary plus is a no-op. */
    PN_CHECK_NEAR (parse_eval (p, vars, "+7", &ok), 7.0, 1e-9);
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

static void
test_variables_and_functions (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    pn_var_store_set (vars, "value1", 10.0);
    pn_var_store_set (vars, "value2", 2.0);
    pn_var_store_set (vars, "value",  9.0);

    PN_CHECK_NEAR (parse_eval (p, vars, "value1 / value2", &ok), 5.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "sqrt(value)", &ok), 3.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "sin(0) + 1", &ok), 1.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "(value1 + value2) * 2", &ok), 24.0, 1e-9);
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Every number form the lexer accepts: plain integer, decimal, a
 * leading-dot fraction, and scientific notation (both signs of exp).
 * g_ascii_strtod does the heavy lifting; this pins which prefixes the
 * lexer is willing to hand it. */
static void
test_number_literals (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "12",      &ok), 12.0,  1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "12.5",    &ok), 12.5,  1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, ".5",      &ok), 0.5,   1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1e3",     &ok), 1000.0,1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2.5e-1",  &ok), 0.25,  1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* The full built-in function table — one assertion per entry so a
 * dropped or mis-wired row is caught.  sin/sqrt are also covered in
 * test_variables_and_functions; repeated here so this one test pins the
 * whole table in a single place.
 *
 * And it pins it EXHAUSTIVELY: check_fn() records the name it exercised
 * and the walk at the end fails for any row nobody checked (TODO
 * #81.11).  A new function is meant to cost one row in builtin_funcs[],
 * its assertion here and its name in the help — the first two are now
 * enforced, so only the help can be forgotten. */
static void
check_fn (PnExprParser *p, PnVarStore *vars, GHashTable *seen,
          const gchar *expr, gdouble want)
{
    const gchar *paren = strchr (expr, '(');
    gboolean     ok;

    PN_CHECK_NEAR (parse_eval (p, vars, expr, &ok), want, 1e-9);
    PN_CHECK (ok);

    if (paren != NULL)
        g_hash_table_add (seen, g_strndup (expr, (gsize) (paren - expr)));
}

static void
test_all_builtin_functions (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    GHashTable   *seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);
    gsize         i;

    check_fn (p, vars, seen, "sin(0)",      0.0);
    check_fn (p, vars, seen, "cos(0)",      1.0);
    check_fn (p, vars, seen, "tan(0)",      0.0);
    check_fn (p, vars, seen, "log(1)",      0.0);
    check_fn (p, vars, seen, "log10(1000)", 3.0);
    check_fn (p, vars, seen, "exp(0)",      1.0);
    check_fn (p, vars, seen, "sqrt(16)",    4.0);
    check_fn (p, vars, seen, "abs(-7)",     7.0);
    check_fn (p, vars, seen, "floor(2.7)",  2.0);
    check_fn (p, vars, seen, "ceil(2.1)",   3.0);

    /* TODO #81.11's one-argument additions.  The inverse trig three are
     * checked where a wrong row would give a different answer; `round`
     * is checked on a HALF, which is the only interesting input it has
     * (away from zero, per C); `trunc` on a negative, which is where it
     * parts company with `floor`; `sign` on all three of its answers. */
    check_fn (p, vars, seen, "asin(1)",     G_PI / 2.0);
    check_fn (p, vars, seen, "acos(1)",     0.0);
    check_fn (p, vars, seen, "atan(1)",     G_PI / 4.0);
    check_fn (p, vars, seen, "round(0.5)",  1.0);
    check_fn (p, vars, seen, "round(-0.5)", -1.0);
    check_fn (p, vars, seen, "trunc(-1.7)", -1.0);
    check_fn (p, vars, seen, "sign(-2)",    -1.0);
    check_fn (p, vars, seen, "sign(0)",     0.0);
    check_fn (p, vars, seen, "sign(2)",     1.0);

    /* TODO #83.3/83.4/83.5/83.6.  The hyperbolics at 0, where each has a
     * different answer, so a mis-wired row cannot hide; the inverse
     * three against their own forward function; the reciprocal three a
     * quarter turn apart; and the two angle conversions, which are the
     * rows a worksheet reaches for most. */
    check_fn (p, vars, seen, "sinh(0)",      0.0);
    check_fn (p, vars, seen, "cosh(0)",      1.0);
    check_fn (p, vars, seen, "tanh(0)",      0.0);
    check_fn (p, vars, seen, "asinh(sinh(1))", 1.0);
    check_fn (p, vars, seen, "acosh(1)",     0.0);
    check_fn (p, vars, seen, "atanh(0)",     0.0);
    check_fn (p, vars, seen, "cot(pi / 4)",  1.0);
    check_fn (p, vars, seen, "sec(0)",       1.0);
    check_fn (p, vars, seen, "csc(pi / 2)",  1.0);
    check_fn (p, vars, seen, "degrees(pi)",  180.0);
    check_fn (p, vars, seen, "radians(180)", G_PI);

    /* The two-argument entries (TODO #81.1): atan2(1,1) is pi/4. */
    check_fn (p, vars, seen, "atan2(1, 1)", G_PI / 4.0);
    check_fn (p, vars, seen, "min(2, 3)",   2.0);
    check_fn (p, vars, seen, "max(2, 3)",   3.0);
    check_fn (p, vars, seen, "pow(2, 10)",  1024.0);
    check_fn (p, vars, seen, "hypot(3, 4)", 5.0);

    /* TODO #83: the three-argument specimen, and the ranged row in both
     * of its forms — one argument is the natural log, two is the log to
     * that base, and both come from one table row. */
    check_fn (p, vars, seen, "clamp(42, 0, 10)", 10.0);
    check_fn (p, vars, seen, "log(8, 2)",        3.0);

    /* Why `pow` is in the table: `^` is bitwise XOR in this language
     * (TODO #81.7 — no new operators), so the same two numbers written
     * with the operator give 8, not 1024.  Asserted next to the call so
     * the trap is visible in one place. */
    {
        gboolean ok;
        PN_CHECK_NEAR (parse_eval (p, vars, "2 ^ 10", &ok), 8.0, 1e-9);
        PN_CHECK (ok);
    }

    /* Every row of the table was exercised above. */
    PN_CHECK_CMPINT ((gint) g_hash_table_size (seen), ==,
                     (gint) pn_expr_func_count ());
    for (i = 0; i < pn_expr_func_count (); i++)
    {
        const PnExprFunc *fn = pn_expr_func_nth (i);
        if (!g_hash_table_contains (seen, fn->name))
            g_printerr ("      no assertion for built-in '%s'\n", fn->name);
        PN_CHECK (g_hash_table_contains (seen, fn->name));
    }

    g_hash_table_unref (seen);
    g_object_unref (vars);
    g_object_unref (p);
}

/* A function argument is a full sub-expression, and calls nest. */
static void
test_functions_nested_and_arg_expr (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "sqrt(9 + 7)",      &ok), 4.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "sqrt(sqrt(16))",   &ok), 2.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "abs(floor(-2.5))", &ok), 3.0, 1e-9); PN_CHECK (ok);
    /* A two-argument call nests inside a two-argument call, in both
     * positions (TODO #81.11's names, same grammar). */
    PN_CHECK_NEAR (parse_eval (p, vars, "max(min(5, 3), hypot(3, 4))", &ok),
                   5.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "round(atan(1) * 4 - pi + 2.5)", &ok),
                   3.0, 1e-9);
    PN_CHECK (ok);

    /* Both arguments of a two-argument call are full expressions, and a
     * call nests inside a call's argument list in either position. */
    PN_CHECK_NEAR (parse_eval (p, vars, "atan2(2 - 2, 3)",    &ok), 0.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "atan2(atan2(0,1),3)", &ok), 0.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "atan2(sin(0), 1)",   &ok), 0.0, 1e-9);
    PN_CHECK (ok);
    /* Whitespace around the comma is insignificant, and the call still
     * composes with the operators around it. */
    PN_CHECK_NEAR (parse_eval (p, vars, "2 * atan2(1,1)", &ok), G_PI / 2.0, 1e-9);
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Unary signs chain and combine with binary operators and parens. */
static void
test_unary_chains (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "--5",       &ok),  5.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "3 - -2",    &ok),  5.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-(2 + 3)",  &ok), -5.0, 1e-9); PN_CHECK (ok);
    /* Unary binds to the factor, so this is (-2) * 3, not -(2 * 3)
     * — same value, but it proves the bind level via the left operand. */
    PN_CHECK_NEAR (parse_eval (p, vars, "-2 * 3",    &ok), -6.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Identifiers may start with / contain underscores. */
static void
test_identifier_forms (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    pn_var_store_set (vars, "_x",     4.0);
    pn_var_store_set (vars, "my_var", 10.0);

    PN_CHECK_NEAR (parse_eval (p, vars, "_x + 1",     &ok),  5.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "my_var / 2", &ok),  5.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Division (like subtraction) is left-associative: 8/4/2 is (8/4)/2. */
static void
test_left_associative_division (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "8 / 4 / 2",    &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "100 / 10 / 2", &ok), 5.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Comparison operators yield a boolean encoded as 1.0 (true) / 0.0
 * (false).  One true and one false case per operator pins the whole set
 * (< > <= >= == !=), including the tie-breaking behaviour of <= / >= and
 * the exact-equality of == / != on the doubles the evaluator works in. */
static void
test_comparisons (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "1 < 2",  &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 < 1",  &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "3 > 2",  &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 > 3",  &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 <= 2", &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "3 <= 2", &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 >= 2", &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1 >= 2", &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 == 2", &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 == 3", &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 != 3", &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 != 2", &ok), 0.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Comparisons bind looser than arithmetic, are left-associative, work
 * over variables, and (being plain numbers) compose with arithmetic and
 * parentheses like any other value. */
static void
test_comparison_precedence (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    pn_var_store_set (vars, "value", 5.0);

    /* Arithmetic on both sides resolves before the comparison. */
    PN_CHECK_NEAR (parse_eval (p, vars, "1 + 1 == 2",  &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "2 * 3 > 5",   &ok), 1.0, 1e-9); PN_CHECK (ok);
    /* Left-associative: (1 < 2) == 1  ->  1 == 1  ->  1. */
    PN_CHECK_NEAR (parse_eval (p, vars, "1 < 2 == 1",  &ok), 1.0, 1e-9); PN_CHECK (ok);
    /* Variable comparison, and a boolean reused as an ordinary number. */
    PN_CHECK_NEAR (parse_eval (p, vars, "value >= 10", &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "(value > 3) * 10", &ok), 10.0, 1e-9); PN_CHECK (ok);
    /* Parentheses override the looser comparison precedence. */
    PN_CHECK_NEAR (parse_eval (p, vars, "(1 < 2) + (3 < 2)", &ok), 1.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* '%' is a floored modulo at the '*' '/' level: the result takes the
 * divisor's sign, fractions work, and a zero divisor gives NaN. */
static void
test_modulo (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "7 % 3",     &ok),  1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-1 % 8",    &ok),  7.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "7 % -3",    &ok), -2.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-8 % 4",    &ok),  0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "5.5 % 2",   &ok),  1.5, 1e-9); PN_CHECK (ok);
    /* Same level as '*': left to right, and tighter than '+'. */
    PN_CHECK_NEAR (parse_eval (p, vars, "2 * 7 % 4", &ok),  2.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1 + 7 % 4", &ok),  4.0, 1e-9); PN_CHECK (ok);
    PN_CHECK (isnan (parse_eval (p, vars, "5 % 0", &ok))); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* The bitwise operators work on the truncated int64 of each operand. */
static void
test_bitwise (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "12 & 10",  &ok),   8.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "12 | 10",  &ok),  14.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "12 ^ 10",  &ok),   6.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "~0",       &ok),  -1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "~5",       &ok),  -6.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-~5",      &ok),   6.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1 << 4",   &ok),  16.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "100 >> 3", &ok),  12.0, 1e-9); PN_CHECK (ok);
    /* '>>' is arithmetic: a negative value stays negative (rounds down). */
    PN_CHECK_NEAR (parse_eval (p, vars, "-8 >> 1",  &ok),  -4.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-1 >> 5",  &ok),  -1.0, 1e-9); PN_CHECK (ok);
    /* Fractions are truncated toward zero first. */
    PN_CHECK_NEAR (parse_eval (p, vars, "7.9 & 3",  &ok),   3.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "-2.5 | 0", &ok),  -2.0, 1e-9); PN_CHECK (ok);
    /* Shifting into the sign bit wraps rather than being undefined. */
    PN_CHECK_NEAR (parse_eval (p, vars, "1 << 63",  &ok), -9223372036854775808.0, 1.0);
    PN_CHECK (ok);
    /* No int64 reading, or a shift count outside 0..63: NaN, not an error. */
    PN_CHECK (isnan (parse_eval (p, vars, "1 << 64",       &ok))); PN_CHECK (ok);
    PN_CHECK (isnan (parse_eval (p, vars, "1 >> -1",       &ok))); PN_CHECK (ok);
    PN_CHECK (isnan (parse_eval (p, vars, "(1 / 0) & 1",   &ok))); PN_CHECK (ok);
    PN_CHECK (isnan (parse_eval (p, vars, "1e19 | 0",      &ok))); PN_CHECK (ok);
    PN_CHECK (isnan (parse_eval (p, vars, "~(0 / 0)",      &ok))); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Precedence, tightest first: unary, * / %, + -, << >>, &, ^, |, then
 * comparisons (Python's order, not C's). */
static void
test_bitwise_precedence (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    pn_var_store_set (vars, "value", 0x5A);   /* 90 = 0101 1010 */

    PN_CHECK_NEAR (parse_eval (p, vars, "1 << 2 + 1",    &ok),  8.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1 << 2 & 6",    &ok),  4.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "6 | 1 & 3",     &ok),  7.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "1 | 3 ^ 1",     &ok),  3.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "3 ^ 1 & 1",     &ok),  2.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "value & 1 == 0", &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "~1 & 7",        &ok),  6.0, 1e-9); PN_CHECK (ok);
    /* Decode a byte: high nibble (opcode) and low nibble (operand). */
    PN_CHECK_NEAR (parse_eval (p, vars, "value >> 4",    &ok),  5.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "value & 15",    &ok), 10.0, 1e-9); PN_CHECK (ok);
    /* Left-associative shifts: (256 >> 2) >> 1. */
    PN_CHECK_NEAR (parse_eval (p, vars, "256 >> 2 >> 1", &ok), 32.0, 1e-9); PN_CHECK (ok);
    /* '<<' next to '<' / '<=' still lexes as intended. */
    PN_CHECK_NEAR (parse_eval (p, vars, "1 << 1 < 3",    &ok),  1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "4 >> 1 >= 2",   &ok),  1.0, 1e-9); PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* A program may be several newline-separated statements; assignments
 * bind names for later lines and the program's value is the last
 * statement's.  Blank lines and a trailing newline are ignored. */
static void
test_statements_and_assignment (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    pn_var_store_set (vars, "value", 10.0);

    /* Assignments feed later lines; value is the last statement. */
    PN_CHECK_NEAR (parse_eval (p, vars, "a = 2\nb = 3\na + b", &ok), 5.0, 1e-9);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "x = value * 2\nx + 1", &ok), 21.0, 1e-9);
    PN_CHECK (ok);
    /* Reassignment: a later line may rebind a name using its own value. */
    PN_CHECK_NEAR (parse_eval (p, vars, "x = 1\nx = x + 4\nx", &ok), 5.0, 1e-9);
    PN_CHECK (ok);
    /* A bare assignment evaluates to the value it bound. */
    PN_CHECK_NEAR (parse_eval (p, vars, "y = 7", &ok), 7.0, 1e-9);
    PN_CHECK (ok);
    /* The assigned value can be a comparison (1.0/0.0). */
    PN_CHECK_NEAR (parse_eval (p, vars, "flag = value > 5\nflag", &ok), 1.0, 1e-9);
    PN_CHECK (ok);
    /* `name ==` is a comparison, not an assignment: the peek that finds
     * an assignment must see a lone '=', not the '==' of equality. */
    PN_CHECK_NEAR (parse_eval (p, vars, "value == 10", &ok), 1.0, 1e-9);
    PN_CHECK (ok);
    /* Blank lines (incl. leading/trailing) are ignored. */
    PN_CHECK_NEAR (parse_eval (p, vars, "\n\na = 1\n\nb = 2\n\na + b\n\n", &ok),
                   3.0, 1e-9);
    PN_CHECK (ok);
    /* A single trailing newline is fine and changes nothing. */
    PN_CHECK_NEAR (parse_eval (p, vars, "value\n", &ok), 10.0, 1e-9);
    PN_CHECK (ok);

    /* An error in any statement (here an unbound variable in the first
     * line) fails the whole program. */
    PN_CHECK_NEAR (parse_eval (p, vars, "missing + 1\n5", &ok), 0.0, 1e-9);
    PN_CHECK_FALSE (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* Runtime (evaluation-time) semantics that parse cleanly but are decided
 * by the evaluator: an unset variable and an unknown function are errors
 * with their own codes, while division by zero is NOT an error — it
 * yields an IEEE infinity, matching the comment in pn-var-store.c. */
static void
test_eval_semantics (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    GError       *err  = NULL;
    PnExprNode   *ast;
    gdouble       out  = 0.0;
    gboolean      ok;

    /* Unknown variable: parses, fails to evaluate with UNKNOWN_VARIABLE. */
    ast = pn_expr_parser_parse (p, "x + 1", &err);
    PN_CHECK (ast != NULL && err == NULL);
    ok = pn_var_store_evaluate (vars, ast, &out, &err);
    PN_CHECK_FALSE (ok);
    PN_CHECK (err != NULL && err->domain == PN_VAR_STORE_ERROR &&
              err->code == PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE);
    g_clear_error (&err);
    pn_expr_node_free (ast);

    /* Unknown function: any IDENT '(' … ')' parses, but evaluation
     * rejects a name not in the built-in table. */
    ast = pn_expr_parser_parse (p, "frobnicate(1)", &err);
    PN_CHECK (ast != NULL && err == NULL);
    ok = pn_var_store_evaluate (vars, ast, &out, &err);
    PN_CHECK_FALSE (ok);
    PN_CHECK (err != NULL && err->domain == PN_VAR_STORE_ERROR &&
              err->code == PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION);
    g_clear_error (&err);
    pn_expr_node_free (ast);

    /* Division by zero is a successful evaluation yielding infinity. */
    out = parse_eval (p, vars, "1 / 0", &ok);
    PN_CHECK (ok);
    PN_CHECK (isinf (out));

    g_object_unref (vars);
    g_object_unref (p);
}

/* Each parse failure maps to its specific error code, not just "an
 * error".  Covers the three general PnExprParserError codes plus the
 * distinct "missing ')' after a function argument" path; the fourth,
 * ARGUMENT_COUNT, has test_call_arity to itself. */
static void
check_parse_error (PnExprParser *p, const gchar *expr, gint code)
{
    GError     *err = NULL;
    PnExprNode *ast = pn_expr_parser_parse (p, expr, &err);

    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL && err->domain == PN_EXPR_PARSER_ERROR &&
              err->code == code);
    g_clear_error (&err);
}

/* The argument COUNT is checked at parse time, against the shared arity
 * table, so a miscounted call fails the moment it is typed rather than
 * at the next message (TODO #81.3).  A name the language does not know
 * has no arity to check against, so it still parses and is left to the
 * evaluator — the behaviour test_eval_semantics pins. */
static void
test_call_arity (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    GError       *err  = NULL;
    PnExprNode   *ast;
    gboolean      ok;

    check_parse_error (p, "atan2(1)",      PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "sin(1, 2)",     PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    /* The arities added by TODO #81.11 come from the same table and are
     * checked the same way, in both directions. */
    check_parse_error (p, "min(1)",        PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "hypot(3)",      PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "round(1, 2)",   PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "sign(1, 2)",    PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "sin(1, 2, 3)",  PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    /* A RANGED arity reports the range rather than a number (TODO
     * #83.2), and a three-argument call is checked like any other. */
    check_parse_error (p, "log(1, 2, 3)",   PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "clamp(1, 2)",    PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    check_parse_error (p, "clamp(1,2,3,4)", PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);
    {
        GError     *e2  = NULL;
        PnExprNode *bad = pn_expr_parser_parse (p, "log(1, 2, 3)", &e2);
        PN_CHECK (bad == NULL);
        PN_CHECK (e2 != NULL &&
                  strstr (e2->message, "1 or 2 arguments") != NULL);
        g_clear_error (&e2);
    }

    /* More arguments than ANY function takes is still rejected for a
     * name the table has never heard of.  The cap is a POLICY now rather
     * than a limit of the tree (83.1d), but it is still a cap. */
    check_parse_error (p, "frob(1, 2, 3, 4, 5)",
                       PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT);

    /* A trailing comma and an empty argument are ordinary syntax
     * errors: the parser asks for an expression and finds ')' or ','. */
    check_parse_error (p, "atan2(1,)",  PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "atan2(,1)",  PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "sin()",      PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "atan2(1, 2", PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);

    /* An unknown name with a plausible count parses — at three arguments
     * as well as two, now that the tree can hold them; the failure is the
     * evaluator's UNKNOWN_FUNCTION, not the parser's. */
    ast = pn_expr_parser_parse (p, "frobnicate(1, 2, 3)", &err);
    PN_CHECK (ast != NULL && err == NULL);
    if (ast != NULL)
    {
        gdouble out = 0.0;
        PN_CHECK_FALSE (pn_var_store_evaluate (vars, ast, &out, &err));
        PN_CHECK (err != NULL && err->domain == PN_VAR_STORE_ERROR &&
                  err->code == PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION);
        g_clear_error (&err);
        pn_expr_node_free (ast);
    }

    /* A comma outside a call's parentheses is not an operator. */
    check_parse_error (p, "1, 2",       PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "(1, 2)",     PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);

    /* Every existing one-argument call still means what it meant. */
    PN_CHECK_NEAR (parse_eval (p, vars, "sqrt(16)", &ok), 4.0, 1e-9);
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

/* The language's constants (TODO #81.6): `pi` and `e` read as values
 * with no binding at all, but a binding of the same name SHADOWS the
 * constant rather than colliding with it. */
static void
test_constants (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "pi",        &ok), G_PI, 1e-12);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "e",         &ok), G_E,  1e-12);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "cos(pi)",   &ok), -1.0, 1e-12);
    PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "log(e)",    &ok),  1.0, 1e-12);
    PN_CHECK (ok);

    /* An explicit binding wins: the constant is only a fallback. */
    pn_var_store_set (vars, "pi", 3.0);
    PN_CHECK_NEAR (parse_eval (p, vars, "pi", &ok), 3.0, 1e-12);
    PN_CHECK (ok);

    /* …and the constant comes back once the binding is gone, which a
     * pre-bound constant could not do (pn_var_store_clear drops every
     * binding). */
    pn_var_store_clear (vars);
    PN_CHECK_NEAR (parse_eval (p, vars, "pi", &ok), G_PI, 1e-12);
    PN_CHECK (ok);

    g_object_unref (vars);
    g_object_unref (p);
}

static void
test_parse_error_codes (void)
{
    PnExprParser *p = pn_expr_parser_new ();

    check_parse_error (p, "1 +",     PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF);
    check_parse_error (p, "",        PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF);
    check_parse_error (p, "1 2",     PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "(1 + 2",  PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "sin(1",   PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "1 @ 2",   PN_EXPR_PARSER_ERROR_SYNTAX);
    /* '=' assigns, but only to an identifier: a number on the left
     * parses as a complete statement, leaving the '=' as junk after it. */
    check_parse_error (p, "1 = 2",   PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    /* A lone '!' is rejected by the lexer: there is '!=' but no not. */
    check_parse_error (p, "1 ! 2",   PN_EXPR_PARSER_ERROR_SYNTAX);
    /* An assignment with no value runs out of input. */
    check_parse_error (p, "x =",     PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF);

    g_object_unref (p);
}

static void
test_parse_errors (void)
{
    PnExprParser *p   = pn_expr_parser_new ();
    GError       *err = NULL;
    PnExprNode   *ast;

    /* Operator with no right-hand operand: runs out of input. */
    ast = pn_expr_parser_parse (p, "1 +", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

    /* Two numbers with no operator: trailing input after a full parse. */
    ast = pn_expr_parser_parse (p, "1 2", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

    /* Unbalanced parenthesis. */
    ast = pn_expr_parser_parse (p, "(1 + 2", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

    /* Empty expression. */
    ast = pn_expr_parser_parse (p, "", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

    /* Stray character the lexer rejects. */
    ast = pn_expr_parser_parse (p, "1 @ 2", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

    g_object_unref (p);
}

/* Pathological nesting must fail with a clean SYNTAX error rather than
 * overflowing the C stack.  Both unbounded recursion paths are exercised:
 * a deep parenthesis stack and a long unary-minus chain.  A moderately
 * nested expression (well under the limit) must still parse and evaluate. */
static void
test_recursion_depth_limit (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    GError       *err  = NULL;
    PnExprNode   *ast;
    gboolean      ok;
    gchar        *deep;
    GString      *s;
    guint         i;
    const guint   n = 5000;     /* far past the 256-level cap */

    /* (((( … 1 … )))) — n open + n close parens around a literal. */
    deep = g_malloc (2 * n + 2);
    memset (deep, '(', n);
    deep[n] = '1';
    memset (deep + n + 1, ')', n);
    deep[2 * n + 1] = '\0';
    ast = pn_expr_parser_parse (p, deep, &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL && err->domain == PN_EXPR_PARSER_ERROR &&
              err->code == PN_EXPR_PARSER_ERROR_SYNTAX);
    g_clear_error (&err);
    g_free (deep);

    /* A long unary-minus chain recurses parse_factor->parse_factor directly,
     * not through parens; it must be capped too. */
    s = g_string_new (NULL);
    for (i = 0; i < n; i++)
        g_string_append_c (s, '-');
    g_string_append_c (s, '1');
    ast = pn_expr_parser_parse (p, s->str, &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL && err->domain == PN_EXPR_PARSER_ERROR &&
              err->code == PN_EXPR_PARSER_ERROR_SYNTAX);
    g_clear_error (&err);
    g_string_free (s, TRUE);

    /* Nesting comfortably under the limit still works. */
    s = g_string_new (NULL);
    for (i = 0; i < 100; i++)
        g_string_append_c (s, '(');
    g_string_append_c (s, '7');
    for (i = 0; i < 100; i++)
        g_string_append_c (s, ')');
    PN_CHECK_NEAR (parse_eval (p, vars, s->str, &ok), 7.0, 1e-9);
    PN_CHECK (ok);
    g_string_free (s, TRUE);

    g_object_unref (vars);
    g_object_unref (p);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-expr-parser");
    pn_test_add ("arithmetic_precedence", test_arithmetic_and_precedence);
    pn_test_add ("unary_sign",            test_unary_sign);
    pn_test_add ("variables_functions",   test_variables_and_functions);
    pn_test_add ("number_literals",       test_number_literals);
    pn_test_add ("all_builtin_functions", test_all_builtin_functions);
    pn_test_add ("functions_nested",      test_functions_nested_and_arg_expr);
    pn_test_add ("call_arity",            test_call_arity);
    pn_test_add ("constants",             test_constants);
    pn_test_add ("unary_chains",          test_unary_chains);
    pn_test_add ("identifier_forms",      test_identifier_forms);
    pn_test_add ("left_assoc_division",   test_left_associative_division);
    pn_test_add ("comparisons",           test_comparisons);
    pn_test_add ("comparison_precedence", test_comparison_precedence);
    pn_test_add ("modulo",                test_modulo);
    pn_test_add ("bitwise",               test_bitwise);
    pn_test_add ("bitwise_precedence",    test_bitwise_precedence);
    pn_test_add ("statements_assignment", test_statements_and_assignment);
    pn_test_add ("eval_semantics",        test_eval_semantics);
    pn_test_add ("parse_error_codes",     test_parse_error_codes);
    pn_test_add ("parse_errors",          test_parse_errors);
    pn_test_add ("recursion_depth_limit", test_recursion_depth_limit);
    return pn_test_run ();
}
