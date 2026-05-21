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

/* Unit tests for PnVarStore: the named-variable symbol table and the
 * recursive evaluator that walks a PnExprNode AST against it.  The AST
 * is a plain tagged-union struct, so the tests build little trees by
 * hand on the stack rather than going through the parser (that path is
 * covered by test-pn-expr-parser).  No filesystem, network, or GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-var-store.h"

#include <math.h>

/* ---- AST builders (stack-allocated; addresses stay valid for the
 *      lifetime of the enclosing test) ---- */

static PnExprNode
num (gdouble v)
{
    PnExprNode n = { 0 };
    n.type   = PN_EXPR_NODE_NUMBER;
    n.number = v;
    return n;
}

static PnExprNode
var (const gchar *name)
{
    PnExprNode n = { 0 };
    n.type = PN_EXPR_NODE_VARIABLE;
    n.name = (gchar *) name;   /* borrowed; evaluator only reads it */
    return n;
}

static PnExprNode
binary (gchar op, PnExprNode *l, PnExprNode *r)
{
    PnExprNode n = { 0 };
    n.type  = PN_EXPR_NODE_BINARY;
    n.op    = op;
    n.left  = l;
    n.right = r;
    return n;
}

static void
test_set_get_clear (void)
{
    PnVarStore *s = pn_var_store_new ();
    gdouble     v = 0.0;

    PN_CHECK_FALSE (pn_var_store_get (s, "x", &v));

    pn_var_store_set (s, "x", 3.0);
    PN_CHECK (pn_var_store_get (s, "x", &v));
    PN_CHECK_NEAR (v, 3.0, 1e-9);

    /* A second set replaces the binding. */
    pn_var_store_set (s, "x", 5.0);
    PN_CHECK (pn_var_store_get (s, "x", &v));
    PN_CHECK_NEAR (v, 5.0, 1e-9);

    /* NULL out_value is allowed for a presence check. */
    PN_CHECK (pn_var_store_get (s, "x", NULL));

    pn_var_store_clear (s);
    PN_CHECK_FALSE (pn_var_store_get (s, "x", NULL));

    g_object_unref (s);
}

static void
test_eval_number_and_unary (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

    PnExprNode lit = num (42.0);
    PN_CHECK (pn_var_store_evaluate (s, &lit, &out, &err));
    PN_CHECK_NEAR (out, 42.0, 1e-9);
    PN_CHECK (err == NULL);

    {
        PnExprNode operand = num (7.0);
        PnExprNode neg     = { 0 };
        neg.type = PN_EXPR_NODE_UNARY;
        neg.op   = '-';
        neg.left = &operand;

        PN_CHECK (pn_var_store_evaluate (s, &neg, &out, &err));
        PN_CHECK_NEAR (out, -7.0, 1e-9);
    }

    g_clear_error (&err);
    g_object_unref (s);
}

static void
test_eval_variables (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

    pn_var_store_set (s, "value", 10.0);

    {
        PnExprNode v = var ("value");
        PN_CHECK (pn_var_store_evaluate (s, &v, &out, &err));
        PN_CHECK_NEAR (out, 10.0, 1e-9);
    }

    /* An unbound variable fails the whole evaluation. */
    {
        PnExprNode v = var ("missing");
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &v, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE));
        g_clear_error (&err);
    }

    g_object_unref (s);
}

static void
test_eval_binary_ops (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;

    PnExprNode a = num (6.0);
    PnExprNode b = num (4.0);

    PnExprNode add = binary ('+', &a, &b);
    PnExprNode sub = binary ('-', &a, &b);
    PnExprNode mul = binary ('*', &a, &b);
    PnExprNode dvd = binary ('/', &a, &b);

    PN_CHECK (pn_var_store_evaluate (s, &add, &out, NULL));
    PN_CHECK_NEAR (out, 10.0, 1e-9);
    PN_CHECK (pn_var_store_evaluate (s, &sub, &out, NULL));
    PN_CHECK_NEAR (out, 2.0, 1e-9);
    PN_CHECK (pn_var_store_evaluate (s, &mul, &out, NULL));
    PN_CHECK_NEAR (out, 24.0, 1e-9);
    PN_CHECK (pn_var_store_evaluate (s, &dvd, &out, NULL));
    PN_CHECK_NEAR (out, 1.5, 1e-9);

    /* Division by zero yields IEEE infinity rather than failing. */
    {
        PnExprNode zero = num (0.0);
        PnExprNode one  = num (1.0);
        PnExprNode div0 = binary ('/', &one, &zero);
        PN_CHECK (pn_var_store_evaluate (s, &div0, &out, NULL));
        PN_CHECK (isinf (out));
    }

    g_object_unref (s);
}

static void
test_eval_functions (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

    /* sqrt(16) == 4 */
    {
        PnExprNode arg  = num (16.0);
        PnExprNode call = { 0 };
        call.type = PN_EXPR_NODE_CALL;
        call.name = (gchar *) "sqrt";
        call.left = &arg;
        PN_CHECK (pn_var_store_evaluate (s, &call, &out, &err));
        PN_CHECK_NEAR (out, 4.0, 1e-9);
    }

    /* abs(-3) == 3 (operand via unary minus) */
    {
        PnExprNode lit  = num (3.0);
        PnExprNode neg  = { 0 };
        PnExprNode call = { 0 };
        neg.type  = PN_EXPR_NODE_UNARY;
        neg.op    = '-';
        neg.left  = &lit;
        call.type = PN_EXPR_NODE_CALL;
        call.name = (gchar *) "abs";
        call.left = &neg;
        PN_CHECK (pn_var_store_evaluate (s, &call, &out, &err));
        PN_CHECK_NEAR (out, 3.0, 1e-9);
    }

    /* An unknown function name fails. */
    {
        PnExprNode arg  = num (1.0);
        PnExprNode call = { 0 };
        call.type = PN_EXPR_NODE_CALL;
        call.name = (gchar *) "frobnicate";
        call.left = &arg;
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &call, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION));
        g_clear_error (&err);
    }

    g_object_unref (s);
}

static void
test_eval_bad_ast (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

    /* A NULL tree is a bad AST. */
    PN_CHECK_FALSE (pn_var_store_evaluate (s, NULL, &out, &err));
    PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                               PN_VAR_STORE_ERROR_BAD_AST));
    g_clear_error (&err);

    /* So is a node with an out-of-range type tag. */
    {
        PnExprNode bad = { 0 };
        bad.type = (PnExprNodeType) 999;
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &bad, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        g_clear_error (&err);
    }

    g_object_unref (s);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-var-store");
    pn_test_add ("set_get_clear",      test_set_get_clear);
    pn_test_add ("eval_number_unary",  test_eval_number_and_unary);
    pn_test_add ("eval_variables",     test_eval_variables);
    pn_test_add ("eval_binary_ops",    test_eval_binary_ops);
    pn_test_add ("eval_functions",     test_eval_functions);
    pn_test_add ("eval_bad_ast",       test_eval_bad_ast);
    return pn_test_run ();
}
