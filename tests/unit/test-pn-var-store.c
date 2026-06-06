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

static PnExprNode
assign (const gchar *name, PnExprNode *value)
{
    PnExprNode n = { 0 };
    n.type = PN_EXPR_NODE_ASSIGN;
    n.name = (gchar *) name;   /* borrowed; evaluator only reads it */
    n.left = value;
    return n;
}

static PnExprNode
seq (PnExprNode *stmt, PnExprNode *rest)
{
    PnExprNode n = { 0 };
    n.type  = PN_EXPR_NODE_SEQ;
    n.left  = stmt;
    n.right = rest;
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

/* The six comparison operators dispatch on their single-character op
 * codes (see pn-expr-parser.h: '<' '>' 'L' '<=' 'G' '>=' '=' '!') and
 * return a boolean encoded as 1.0 / 0.0.  Built straight from the AST so
 * the evaluator's op-code contract is pinned independently of the parser
 * that normally assigns those codes. */
static void
test_eval_comparison_ops (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;

    PnExprNode lo = num (4.0);
    PnExprNode hi = num (6.0);
    PnExprNode eq = num (4.0);

    struct { gchar op; PnExprNode *l, *r; gdouble want; } cases[] = {
        { '<', &lo, &hi, 1.0 }, { '<', &hi, &lo, 0.0 },
        { '>', &hi, &lo, 1.0 }, { '>', &lo, &hi, 0.0 },
        { 'L', &lo, &eq, 1.0 }, { 'L', &hi, &lo, 0.0 },  /* <= */
        { 'G', &lo, &eq, 1.0 }, { 'G', &lo, &hi, 0.0 },  /* >= */
        { '=', &lo, &eq, 1.0 }, { '=', &lo, &hi, 0.0 },  /* == */
        { '!', &lo, &hi, 1.0 }, { '!', &lo, &eq, 0.0 },  /* != */
    };

    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
        PnExprNode cmp = binary (cases[i].op, cases[i].l, cases[i].r);
        PN_CHECK (pn_var_store_evaluate (s, &cmp, &out, NULL));
        PN_CHECK_NEAR (out, cases[i].want, 1e-9);
    }

    g_object_unref (s);
}

/* Capture callback for pn_var_store_foreach_assignment: copy each
 * reported (name, value) into a hash the test can assert against. */
static void
collect_assignment (const gchar *name, gdouble value, gpointer user_data)
{
    GHashTable *seen  = user_data;
    gdouble    *boxed = g_new (gdouble, 1);
    *boxed = value;
    g_hash_table_insert (seen, g_strdup (name), boxed);
}

/* An ASSIGN node binds a name (visible to the rest of the tree and
 * reported as an assignment) and evaluates to the bound value; a SEQ
 * node evaluates its statement for effect then yields the rest's value.
 * Together they model "a = 5; a + 1". */
static void
test_eval_assign_and_seq (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;

    PnExprNode five = num (5.0);
    PnExprNode aset = assign ("a", &five);   /* a = 5      */
    PnExprNode avar = var ("a");
    PnExprNode one  = num (1.0);
    PnExprNode add  = binary ('+', &avar, &one); /* a + 1  */
    PnExprNode prog = seq (&aset, &add);         /* a = 5 ; a + 1 */

    PN_CHECK (pn_var_store_evaluate (s, &prog, &out, NULL));
    PN_CHECK_NEAR (out, 6.0, 1e-9);

    /* The assignment bound `a`, so it reads back as a variable. */
    {
        gdouble v = 0.0;
        PN_CHECK (pn_var_store_get (s, "a", &v));
        PN_CHECK_NEAR (v, 5.0, 1e-9);
    }

    /* foreach reports exactly the assigned name and its value. */
    {
        GHashTable *seen  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, g_free);
        gdouble    *boxed;

        pn_var_store_foreach_assignment (s, collect_assignment, seen);
        PN_CHECK_CMPINT (g_hash_table_size (seen), ==, 1);
        boxed = g_hash_table_lookup (seen, "a");
        PN_CHECK (boxed != NULL);
        PN_CHECK_NEAR (*boxed, 5.0, 1e-9);
        g_hash_table_unref (seen);
    }

    /* A plain pn_var_store_set() binding is NOT counted as an
     * assignment: foreach still reports only `a`. */
    {
        GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_free);
        pn_var_store_set (s, "input", 9.0);
        pn_var_store_foreach_assignment (s, collect_assignment, seen);
        PN_CHECK_CMPINT (g_hash_table_size (seen), ==, 1);
        PN_CHECK (g_hash_table_lookup (seen, "input") == NULL);
        g_hash_table_unref (seen);
    }

    /* clear() drops the assignment record along with the bindings. */
    {
        GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_free);
        pn_var_store_clear (s);
        pn_var_store_foreach_assignment (s, collect_assignment, seen);
        PN_CHECK_CMPINT (g_hash_table_size (seen), ==, 0);
        g_hash_table_unref (seen);
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

/* ---- Vector semantics (TODO #43.7) ---- */

static PnExprNode
unary (gchar op, PnExprNode *operand)
{
    PnExprNode n = { 0 };
    n.type = PN_EXPR_NODE_UNARY;
    n.op   = op;
    n.left = operand;
    return n;
}

static PnExprNode
call (const gchar *name, PnExprNode *arg)
{
    PnExprNode n = { 0 };
    n.type = PN_EXPR_NODE_CALL;
    n.name = (gchar *) name;   /* borrowed; evaluator only reads it */
    n.left = arg;
    return n;
}

/* Bind @name to a fresh vector copied from @vals; the store keeps its
 * own reference, so the local one is dropped on return. */
static void
bind_vec (PnVarStore *s, const gchar *name, const gdouble *vals, gsize n)
{
    PnVector *v = pn_vector_new_copy (vals, n);
    pn_var_store_set_vector (s, name, v);
    g_object_unref (v);
}

/* Assert @out is a vector matching @want[0..n). */
static void
check_vec (const PnExprValue *out, const gdouble *want, gsize n)
{
    PN_CHECK (out->vec != NULL);
    if (out->vec == NULL)
        return;
    PN_CHECK_CMPINT ((gint) pn_vector_get_len (out->vec), ==, (gint) n);
    if (pn_vector_get_len (out->vec) == n)
    {
        const gdouble *d = pn_vector_get_data (out->vec);
        for (gsize i = 0; i < n; i++)
            PN_CHECK_NEAR (d[i], want[i], 1e-9);
    }
}

/* scalar OP vector and vector OP scalar both broadcast the scalar over
 * every element, producing a vector. */
static void
test_vector_broadcast (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     arr[] = { 1.0, 2.0, 3.0 };

    bind_vec (s, "arr", arr, 3);

    /* 2 * arr -> [2, 4, 6] */
    {
        PnExprNode two = num (2.0), a = var ("arr");
        PnExprNode mul = binary ('*', &two, &a);
        gdouble    want[] = { 2.0, 4.0, 6.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &mul, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    /* arr - 1 -> [0, 1, 2]  (vector OP scalar keeps operand order) */
    {
        PnExprNode a = var ("arr"), one = num (1.0);
        PnExprNode sub = binary ('-', &a, &one);
        gdouble    want[] = { 0.0, 1.0, 2.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &sub, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    g_object_unref (s);
}

/* vector OP vector is elementwise; on a length mismatch the result takes
 * the longer length and the surviving tail passes through verbatim. */
static void
test_vector_elementwise (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     a[]  = { 2.0, 3.0 };
    gdouble     b[]  = { 3.0, 4.0, 5.0 };

    bind_vec (s, "a", a, 2);
    bind_vec (s, "b", b, 3);

    /* a * b -> [6, 12, 5]: index 2 has no counterpart in `a`, so b[2]=5
     * passes through unchanged (the 43.7 pass-through tail rule). */
    {
        PnExprNode av = var ("a"), bv = var ("b");
        PnExprNode mul = binary ('*', &av, &bv);
        gdouble    want[] = { 6.0, 12.0, 5.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &mul, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    /* b + a -> [5, 7, 5]: longer operand on the left, same tail rule. */
    {
        PnExprNode bv = var ("b"), av = var ("a");
        PnExprNode add = binary ('+', &bv, &av);
        gdouble    want[] = { 5.0, 7.0, 5.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &add, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    g_object_unref (s);
}

/* A unary math function maps element-by-element to a same-length vector;
 * unary minus negates every element. */
static void
test_vector_functions_and_unary (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     arr[] = { -1.0, -2.0, 3.0 };

    bind_vec (s, "arr", arr, 3);

    /* abs(arr) -> [1, 2, 3] */
    {
        PnExprNode a = var ("arr");
        PnExprNode c = call ("abs", &a);
        gdouble    want[] = { 1.0, 2.0, 3.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &c, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    /* -arr -> [1, 2, -3] */
    {
        PnExprNode a = var ("arr");
        PnExprNode neg = unary ('-', &a);
        gdouble    want[] = { 1.0, 2.0, -3.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, &neg, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    g_object_unref (s);
}

/* Comparisons always reduce a vector operand to a single scalar 0/1:
 * true iff EVERY compared element passes (all()-semantics), with an
 * unequal-length tail counting as vacuously true. */
static void
test_vector_comparison_reduces (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     hi[]  = { 3.0, 5.0, 4.0 };
    gdouble     mix[] = { 1.0, 5.0, 3.0 };
    gdouble     two[] = { 2.0, 2.0 };       /* shorter than hi */

    bind_vec (s, "hi",  hi,  3);
    bind_vec (s, "mix", mix, 3);
    bind_vec (s, "two", two, 2);

    /* hi > 2 -> 1.0 (all pass) */
    {
        PnExprNode v = var ("hi"), t = num (2.0);
        PnExprNode cmp = binary ('>', &v, &t);
        PN_CHECK (pn_var_store_evaluate_value (s, &cmp, &out, NULL));
        PN_CHECK (out.vec == NULL);
        PN_CHECK_NEAR (out.scalar, 1.0, 1e-9);
        pn_expr_value_clear (&out);
    }

    /* mix > 2 -> 0.0 (1.0 fails) */
    {
        PnExprNode v = var ("mix"), t = num (2.0);
        PnExprNode cmp = binary ('>', &v, &t);
        PN_CHECK (pn_var_store_evaluate_value (s, &cmp, &out, NULL));
        PN_CHECK_NEAR (out.scalar, 0.0, 1e-9);
        pn_expr_value_clear (&out);
    }

    /* hi > two -> 1.0: only the overlap (3>2, 5>2) is compared; hi[2]
     * has no counterpart and is vacuously true. */
    {
        PnExprNode v = var ("hi"), w = var ("two");
        PnExprNode cmp = binary ('>', &v, &w);
        PN_CHECK (pn_var_store_evaluate_value (s, &cmp, &out, NULL));
        PN_CHECK (out.vec == NULL);
        PN_CHECK_NEAR (out.scalar, 1.0, 1e-9);
        pn_expr_value_clear (&out);
    }

    g_object_unref (s);
}

/* The scalar-only pn_var_store_evaluate() refuses a vector result with
 * PN_VAR_STORE_ERROR_TYPE_MISMATCH rather than crashing or truncating. */
static void
test_scalar_sink_rejects_vector (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;
    gdouble     arr[] = { 1.0, 2.0 };

    bind_vec (s, "arr", arr, 2);

    {
        PnExprNode a = var ("arr");
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &a, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_TYPE_MISMATCH));
        g_clear_error (&err);
    }

    g_object_unref (s);
}

/* pn_var_store_value_to_string renders a scalar as %g and a vector as a
 * BOUNDED leading sample plus its element count. */
static void
test_value_to_string (void)
{
    PnExprValue v = { NULL, 0.0 };
    gchar      *s;

    /* scalar */
    v.scalar = 42.0;
    s = pn_var_store_value_to_string (&v);
    PN_CHECK_CMPSTR (s, ==, "42");
    g_free (s);

    /* short vector: every element shown */
    {
        gdouble d[] = { 1.0, 2.0, 3.0 };
        v.vec = pn_vector_new_copy (d, 3);
        v.scalar = 0.0;
        s = pn_var_store_value_to_string (&v);
        PN_CHECK_CMPSTR (s, ==, "[1, 2, 3] (3 values)");
        g_free (s);
        g_clear_object (&v.vec);
    }

    /* long vector: capped at 8 shown elements then an ellipsis, with the
     * true count in the trailer (bounded so a huge vector is safe). */
    {
        gdouble d[12];
        for (gsize i = 0; i < 12; i++)
            d[i] = (gdouble) i;
        v.vec = pn_vector_new_copy (d, 12);
        s = pn_var_store_value_to_string (&v);
        PN_CHECK_CMPSTR (s, ==,
                         "[0, 1, 2, 3, 4, 5, 6, 7, \xE2\x80\xA6] (12 values)");
        g_free (s);
        g_clear_object (&v.vec);
    }
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
    pn_test_add ("eval_comparison_ops", test_eval_comparison_ops);
    pn_test_add ("eval_assign_and_seq", test_eval_assign_and_seq);
    pn_test_add ("eval_functions",     test_eval_functions);
    pn_test_add ("vector_broadcast",   test_vector_broadcast);
    pn_test_add ("vector_elementwise", test_vector_elementwise);
    pn_test_add ("vector_fns_unary",   test_vector_functions_and_unary);
    pn_test_add ("vector_comparison",  test_vector_comparison_reduces);
    pn_test_add ("scalar_sink_vector", test_scalar_sink_rejects_vector);
    pn_test_add ("value_to_string",    test_value_to_string);
    pn_test_add ("eval_bad_ast",       test_eval_bad_ast);
    return pn_test_run ();
}
