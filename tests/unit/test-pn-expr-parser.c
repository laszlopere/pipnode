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
    pn_test_add ("parse_errors",          test_parse_errors);
    return pn_test_run ();
}
