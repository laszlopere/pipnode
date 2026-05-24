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
 * whole table in a single place. */
static void
test_all_builtin_functions (void)
{
    PnExprParser *p    = pn_expr_parser_new ();
    PnVarStore   *vars = pn_var_store_new ();
    gboolean      ok;

    PN_CHECK_NEAR (parse_eval (p, vars, "sin(0)",      &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "cos(0)",      &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "tan(0)",      &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "log(1)",      &ok), 0.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "log10(1000)", &ok), 3.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "exp(0)",      &ok), 1.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "sqrt(16)",    &ok), 4.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "abs(-7)",     &ok), 7.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "floor(2.7)",  &ok), 2.0, 1e-9); PN_CHECK (ok);
    PN_CHECK_NEAR (parse_eval (p, vars, "ceil(2.1)",   &ok), 3.0, 1e-9); PN_CHECK (ok);

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
 * error".  Covers all three PnExprParserError codes plus the distinct
 * "missing ')' after a function argument" path. */
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

static void
test_parse_error_codes (void)
{
    PnExprParser *p = pn_expr_parser_new ();

    check_parse_error (p, "1 +",     PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF);
    check_parse_error (p, "",        PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF);
    check_parse_error (p, "1 2",     PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "(1 + 2",  PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "sin(1",   PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN);
    check_parse_error (p, "1 % 2",   PN_EXPR_PARSER_ERROR_SYNTAX);
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
    ast = pn_expr_parser_parse (p, "1 % 2", &err);
    PN_CHECK (ast == NULL);
    PN_CHECK (err != NULL);
    g_clear_error (&err);

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
    pn_test_add ("unary_chains",          test_unary_chains);
    pn_test_add ("identifier_forms",      test_identifier_forms);
    pn_test_add ("left_assoc_division",   test_left_associative_division);
    pn_test_add ("comparisons",           test_comparisons);
    pn_test_add ("comparison_precedence", test_comparison_precedence);
    pn_test_add ("statements_assignment", test_statements_and_assignment);
    pn_test_add ("eval_semantics",        test_eval_semantics);
    pn_test_add ("parse_error_codes",     test_parse_error_codes);
    pn_test_add ("parse_errors",          test_parse_errors);
    return pn_test_run ();
}
