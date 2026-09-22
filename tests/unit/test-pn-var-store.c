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
#include "pn-expr-funcs.h"

#include <math.h>
#include <string.h>

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

/* A call of two or more arguments.  Argument one is .left; the rest hang
 * off .right as a chain of ARG nodes (TODO #83.1), and a two-argument
 * call chains too — arity 2 is not special.  The chain nodes have to
 * outlive the call, so the caller lends a CallN to hold them. */
typedef struct
{
    PnExprNode call;
    PnExprNode arg[PN_EXPR_MAX_ARITY - 1];
} CallN;

static PnExprNode *
call_n (CallN *st, const gchar *name, PnExprNode **args, gint n)
{
    gint i;

    memset (st, 0, sizeof *st);
    st->call.type = PN_EXPR_NODE_CALL;
    st->call.name = (gchar *) name;  /* borrowed; evaluator only reads it */
    st->call.left = args[0];

    for (i = 1; i < n; i++)
    {
        st->arg[i - 1].type  = PN_EXPR_NODE_ARG;
        st->arg[i - 1].left  = args[i];
        st->arg[i - 1].right = (i + 1 < n) ? &st->arg[i] : NULL;
    }
    st->call.right = (n > 1) ? &st->arg[0] : NULL;

    return &st->call;
}

static PnExprNode *
call2 (CallN *st, const gchar *name, PnExprNode *a, PnExprNode *b)
{
    PnExprNode *args[2] = { a, b };
    return call_n (st, name, args, 2);
}

static PnExprNode *
call3 (CallN *st, const gchar *name,
       PnExprNode *a, PnExprNode *b, PnExprNode *c)
{
    PnExprNode *args[3] = { a, b, c };
    return call_n (st, name, args, 3);
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

    /* atan2(1, 1) == pi/4: the second argument reached the C call, in
     * the right ORDER (atan2(1,1) and atan2(1,-1) differ). */
    {
        PnExprNode  y = num (1.0), x = num (1.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &y, &x);
        PN_CHECK (pn_var_store_evaluate (s, c, &out, &err));
        PN_CHECK_NEAR (out, G_PI / 4.0, 1e-12);
    }
    {
        PnExprNode  y = num (1.0), x = num (-1.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &y, &x);
        PN_CHECK (pn_var_store_evaluate (s, c, &out, &err));
        PN_CHECK_NEAR (out, 3.0 * G_PI / 4.0, 1e-12);
    }

    /* The parser never builds one, but a hand-made tree with the wrong
     * number of arguments is a BAD_AST rather than a wrong answer —
     * both directions. */
    {
        PnExprNode arg = num (1.0);
        PnExprNode c   = { 0 };
        c.type = PN_EXPR_NODE_CALL;
        c.name = (gchar *) "atan2";
        c.left = &arg;              /* no .right */
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        g_clear_error (&err);
    }
    {
        PnExprNode  a = num (1.0), b = num (2.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "sqrt", &a, &b);  /* one too many */
        PN_CHECK_FALSE (pn_var_store_evaluate (s, c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        g_clear_error (&err);
    }

    /* A chain that is not a chain: argument two must be an ARG node, so
     * a raw expression hanging off .right — which is what a two-argument
     * call looked like before TODO #83.1 — is a malformed argument list
     * rather than a silently accepted call. */
    {
        PnExprNode a = num (1.0), b = num (2.0);
        PnExprNode c = { 0 };
        c.type  = PN_EXPR_NODE_CALL;
        c.name  = (gchar *) "atan2";
        c.left  = &a;
        c.right = &b;
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        g_clear_error (&err);
    }

    g_object_unref (s);
}

/* The language's named constants (TODO #81.6).  They resolve in the
 * VARIABLE case as a FALLBACK, beneath the bindings, which is what
 * makes them survive pn_var_store_clear() and lets a data-bag member of
 * the same name shadow one. */
static void
test_constants (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

    /* Read with nothing bound at all. */
    {
        PnExprNode c = var ("pi");
        PN_CHECK (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK_NEAR (out, G_PI, 1e-12);
    }
    {
        PnExprNode c = var ("e");
        PN_CHECK (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK_NEAR (out, G_E, 1e-12);
    }

    /* A binding of the same name SHADOWS the constant... */
    pn_var_store_set (s, "pi", 3.0);
    {
        PnExprNode c = var ("pi");
        PN_CHECK (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK_NEAR (out, 3.0, 1e-12);
    }

    /* ...and clearing brings the constant back, which is the whole
     * reason it is not pre-bound: pn_var_store_clear() would have
     * dropped a pre-bound one for good. */
    pn_var_store_clear (s);
    {
        PnExprNode c = var ("pi");
        PN_CHECK (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK_NEAR (out, G_PI, 1e-12);
    }

    /* A constant is not a binding: pn_var_store_get() still says no,
     * and an unrelated name still fails as an unknown variable. */
    PN_CHECK_FALSE (pn_var_store_get (s, "pi", NULL));
    {
        PnExprNode c = var ("tau");
        PN_CHECK_FALSE (pn_var_store_evaluate (s, &c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE));
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

/* A TWO-argument function broadcasts exactly as the binary operators do
 * — that is the whole of TODO #81.5: scalar broadcasts, vector-with-
 * vector is elementwise, and a length mismatch takes the LONGER length
 * with the surviving tail passing through verbatim, the same 43.7 rule
 * test_vector_elementwise pins for `*`.  A function with its own
 * broadcasting rule would be a second rule for one idea. */
static void
test_vector_two_argument_function (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     a[] = { 0.0, 1.0 };
    gdouble     b[] = { 1.0, 1.0, 7.0 };

    bind_vec (s, "a", a, 2);
    bind_vec (s, "b", b, 3);

    /* scalar, vector -> broadcast: atan2(0, [1,1,7]) = [0, 0, 0]. */
    {
        PnExprNode zero = num (0.0), bv = var ("b");
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &zero, &bv);
        gdouble     want[] = { 0.0, 0.0, 0.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    /* vector, scalar -> broadcast the other way:
     * atan2([0,1], 1) = [0, pi/4]. */
    {
        PnExprNode av = var ("a"), one = num (1.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &av, &one);
        gdouble     want[] = { 0.0, G_PI / 4.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        check_vec (&out, want, 2);
        pn_expr_value_clear (&out);
    }

    /* vector, vector of UNEQUAL length: elementwise where both have an
     * element, then b[2]=7 passes through verbatim — the operators'
     * tail rule, not a truncation. */
    {
        PnExprNode av = var ("a"), bv = var ("b");
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &av, &bv);
        gdouble     want[] = { 0.0, G_PI / 4.0, 7.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        check_vec (&out, want, 3);
        pn_expr_value_clear (&out);
    }

    /* Longer operand on the LEFT: same rule, a[?] would be the tail. */
    {
        PnExprNode bv = var ("b"), av = var ("a");
        CallN       st;
        PnExprNode *c = call2 (&st, "atan2", &bv, &av);
        gdouble     want[] = { G_PI / 2.0, atan2 (1.0, 1.0), 7.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
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

/* ---- The rest of the built-in table (TODO #81.11) ---- */

/* One assertion per row added by TODO #81.11, plus the three decisions
 * that are not a libm call and so cannot be read off a man page:
 * `round` rounds halves AWAY FROM ZERO, `min`/`max` are fmin/fmax and
 * therefore SKIP a NaN operand, and `sign` is written out here — 0 for
 * either zero, NaN for NaN.  The `^` line is the reason `pow` exists:
 * in this language `^` is bitwise XOR, so `2 ^ 10` is 8. */
static void
test_builtin_functions (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;

#define CHECK_CALL1(name_, arg_, want_)                                 \
    G_STMT_START {                                                      \
        PnExprNode a_ = num (arg_);                                     \
        PnExprNode c_ = call (name_, &a_);                              \
        PN_CHECK (pn_var_store_evaluate (s, &c_, &out, NULL));          \
        PN_CHECK_NEAR (out, (want_), 1e-12);                            \
    } G_STMT_END

#define CHECK_CALL2(name_, x_, y_, want_)                               \
    G_STMT_START {                                                      \
        PnExprNode  x__ = num (x_), y__ = num (y_);                     \
        CallN       st_;                                                \
        PnExprNode *c_  = call2 (&st_, name_, &x__, &y__);              \
        PN_CHECK (pn_var_store_evaluate (s, c_, &out, NULL));           \
        PN_CHECK_NEAR (out, (want_), 1e-12);                            \
    } G_STMT_END

#define CHECK_CALL3(name_, x_, y_, z_, want_)                           \
    G_STMT_START {                                                      \
        PnExprNode  x__ = num (x_), y__ = num (y_), z__ = num (z_);     \
        CallN       st_;                                                \
        PnExprNode *c_  = call3 (&st_, name_, &x__, &y__, &z__);        \
        PN_CHECK (pn_var_store_evaluate (s, c_, &out, NULL));           \
        PN_CHECK_NEAR (out, (want_), 1e-12);                            \
    } G_STMT_END

#define CHECK_CALL1_NAN(name_, arg_)                                    \
    G_STMT_START {                                                      \
        PnExprNode a_ = num (arg_);                                     \
        PnExprNode c_ = call (name_, &a_);                              \
        PN_CHECK (pn_var_store_evaluate (s, &c_, &out, NULL));          \
        PN_CHECK (isnan (out));                                         \
    } G_STMT_END

    /* The inverse trig functions, each checked at a point where a wrong
     * row (asin for acos, say) would give a different answer. */
    CHECK_CALL1 ("asin", 1.0, G_PI / 2.0);
    CHECK_CALL1 ("asin", 0.0, 0.0);
    CHECK_CALL1 ("acos", 1.0, 0.0);
    CHECK_CALL1 ("acos", 0.0, G_PI / 2.0);
    CHECK_CALL1 ("atan", 1.0, G_PI / 4.0);

    /* Outside the domain libm returns NaN rather than raising, and the
     * language passes that straight through — a NaN value, not an
     * evaluation error. */
    CHECK_CALL1_NAN ("asin", 2.0);
    CHECK_CALL1_NAN ("acos", -2.0);

    /* round(): halves go AWAY from zero (C's round(), not the
     * ties-to-even some calculators use), which is the one thing about
     * it a reader cannot guess. */
    CHECK_CALL1 ("round",  2.4,  2.0);
    CHECK_CALL1 ("round",  0.5,  1.0);
    CHECK_CALL1 ("round", -0.5, -1.0);
    CHECK_CALL1 ("round",  2.5,  3.0);
    CHECK_CALL1 ("round", -2.5, -3.0);

    /* trunc() drops the fraction toward zero, which is where it differs
     * from floor() on a negative — the reason both exist. */
    CHECK_CALL1 ("trunc",  1.7,  1.0);
    CHECK_CALL1 ("trunc", -1.7, -1.0);
    CHECK_CALL1 ("floor", -1.7, -2.0);

    /* sign(): the two decisions 81.11 asked for, pinned. */
    CHECK_CALL1 ("sign",  3.5,  1.0);
    CHECK_CALL1 ("sign", -3.5, -1.0);
    CHECK_CALL1 ("sign",  0.0,  0.0);
    CHECK_CALL1 ("sign", -0.0,  0.0);
    CHECK_CALL1_NAN ("sign", NAN);

    /* min/max, including the ORDER-independent NaN behaviour fmin/fmax
     * give: a NaN operand is skipped, not propagated. */
    CHECK_CALL2 ("min", 2.0, 3.0, 2.0);
    CHECK_CALL2 ("min", 3.0, 2.0, 2.0);
    CHECK_CALL2 ("max", 2.0, 3.0, 3.0);
    CHECK_CALL2 ("max", -2.0, -3.0, -2.0);
    CHECK_CALL2 ("min", NAN, 3.0, 3.0);
    CHECK_CALL2 ("max", 3.0, NAN, 3.0);

    /* pow() and hypot(). */
    CHECK_CALL2 ("pow", 2.0, 10.0, 1024.0);
    CHECK_CALL2 ("pow", 9.0, 0.5, 3.0);
    CHECK_CALL2 ("pow", -2.0, 3.0, -8.0);
    CHECK_CALL2 ("hypot", 3.0, 4.0, 5.0);
    CHECK_CALL2 ("hypot", 0.0, -4.0, 4.0);

    /* Why pow() is in the table at all: `^` is XOR here, so `2 ^ 10` is
     * 8 and NOT 1024 (TODO #81.7 — no new operators).  Both spellings
     * asserted together so the trap is visible in one place. */
    {
        PnExprNode two = num (2.0), ten = num (10.0);
        PnExprNode x   = binary ('^', &two, &ten);
        PN_CHECK (pn_var_store_evaluate (s, &x, &out, NULL));
        PN_CHECK_NEAR (out, 8.0, 1e-12);
    }

    /* A new two-argument name broadcasts over vectors exactly as atan2
     * does: one zip_value(), so it cannot drift (TODO #81.5). */
    {
        PnExprValue v   = { NULL, 0.0 };
        gdouble     a[] = { 1.0, 5.0 };
        gdouble     b[] = { 4.0, 2.0, 9.0 };
        gdouble     want[] = { 1.0, 2.0, 9.0 };
        PnExprNode  av, bv;
        PnExprNode *c;
        CallN       st;

        bind_vec (s, "a", a, 2);
        bind_vec (s, "b", b, 3);
        av = var ("a");
        bv = var ("b");
        c  = call2 (&st, "min", &av, &bv);
        PN_CHECK (pn_var_store_evaluate_value (s, c, &v, NULL));
        check_vec (&v, want, 3);
        pn_expr_value_clear (&v);
    }

#undef CHECK_CALL1
#undef CHECK_CALL2
#undef CHECK_CALL3
#undef CHECK_CALL1_NAN

    g_object_unref (s);
}

/* ---- The NaN and domain policy (TODO #83.18) ---- */

/* Settled once for the whole table and asserted here rather than left in
 * prose: a result mathematics does not define is a VALUE, not an error.
 * Nothing in the table raises; a NaN or an infinity travels down the
 * wire like any other number, exactly as `1 / 0` already did. */
static void
test_domain_policy (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

#define EVAL1(name_, arg_)                                              \
    G_STMT_START {                                                      \
        PnExprNode a_ = num (arg_);                                     \
        PnExprNode c_ = call (name_, &a_);                              \
        PN_CHECK (pn_var_store_evaluate (s, &c_, &out, &err));          \
        PN_CHECK (err == NULL);                                         \
    } G_STMT_END

    /* Outside the domain: NaN, and the evaluation SUCCEEDS. */
    EVAL1 ("acosh", 0.5);   PN_CHECK (isnan (out));
    EVAL1 ("atanh", 2.0);   PN_CHECK (isnan (out));
    EVAL1 ("sqrt", -1.0);   PN_CHECK (isnan (out));
    EVAL1 ("log",  -1.0);   PN_CHECK (isnan (out));

    /* At the edge of the domain, and at a pole: infinite, also a value.
     * cot and csc blow up at every multiple of pi; atanh at ±1. */
    EVAL1 ("atanh", 1.0);   PN_CHECK (isinf (out) && out > 0.0);
    EVAL1 ("log",   0.0);   PN_CHECK (isinf (out) && out < 0.0);
    EVAL1 ("cot",   0.0);   PN_CHECK (isinf (out) && out > 0.0);
    EVAL1 ("csc",   0.0);   PN_CHECK (isinf (out) && out > 0.0);

    /* `sec` is a quarter turn away from those two — and its pole is not
     * reachable from a program, because pi/2 is not exactly
     * representable: the answer is enormous rather than infinite, which
     * is the honest thing to assert. */
    EVAL1 ("sec", G_PI / 2.0);
    PN_CHECK (isfinite (out) && fabs (out) > 1e15);

#undef EVAL1

    g_object_unref (s);
}

/* ---- Trigonometry, hyperbolics and the angle pair ---- */

/* The rows TODO #83.3, #83.4, #83.5 and #83.6 add, checked where a
 * mis-wired row would give a different answer rather than at 0 where
 * several agree. */
static void
test_trig_and_hyperbolic_rows (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;

#define CHECK1(name_, arg_, want_)                                      \
    G_STMT_START {                                                      \
        PnExprNode a_ = num (arg_);                                     \
        PnExprNode c_ = call (name_, &a_);                              \
        PN_CHECK (pn_var_store_evaluate (s, &c_, &out, NULL));          \
        PN_CHECK_NEAR (out, (want_), 1e-12);                            \
    } G_STMT_END

    /* Hyperbolics: the definitions, so a row pointing at the wrong libm
     * function is caught. */
    CHECK1 ("sinh", 1.0, (exp (1.0) - exp (-1.0)) / 2.0);
    CHECK1 ("cosh", 1.0, (exp (1.0) + exp (-1.0)) / 2.0);
    CHECK1 ("tanh", 1.0, sinh (1.0) / cosh (1.0));
    CHECK1 ("cosh", 0.0, 1.0);          /* the catenary's lowest point */

    /* Inverse hyperbolics, each against its own forward function. */
    CHECK1 ("asinh", sinh (0.7), 0.7);
    CHECK1 ("acosh", cosh (0.7), 0.7);
    CHECK1 ("atanh", tanh (0.7), 0.7);

    /* Reciprocal trig: each is the reciprocal it claims to be, at an
     * angle where all three differ. */
    CHECK1 ("cot", 0.7, cos (0.7) / sin (0.7));
    CHECK1 ("sec", 0.7, 1.0 / cos (0.7));
    CHECK1 ("csc", 0.7, 1.0 / sin (0.7));
    CHECK1 ("cot", G_PI / 4.0, 1.0);

    /* The angle pair, in both directions and round trip.  radians() is
     * exactly what `angle * pi / 180` was, which is what two example
     * worksheets were writing by hand. */
    CHECK1 ("degrees", G_PI, 180.0);
    CHECK1 ("degrees", G_PI / 2.0, 90.0);
    CHECK1 ("radians", 180.0, G_PI);
    CHECK1 ("radians", 90.0, G_PI / 2.0);
    CHECK1 ("degrees", 0.0, 0.0);

    {
        PnExprNode a = num (37.0);
        PnExprNode inner = call ("radians", &a);
        PnExprNode outer = call ("degrees", &inner);
        PN_CHECK (pn_var_store_evaluate (s, &outer, &out, NULL));
        PN_CHECK_NEAR (out, 37.0, 1e-12);
    }

#undef CHECK1

    g_object_unref (s);
}

/* ---- Three arguments and a ranged arity (TODO #83.1, #83.2) ---- */

/* `clamp` is the first three-argument function and the specimen for the
 * argument chain; `log(x[, base])` is the first whose arity is a RANGE.
 * Between them they exercise both new paths in the table. */
static void
test_arity_three_and_range (void)
{
    PnVarStore *s   = pn_var_store_new ();
    gdouble     out = 0.0;
    GError     *err = NULL;

#define CHECK3(name_, x_, y_, z_, want_)                                \
    G_STMT_START {                                                      \
        PnExprNode  x__ = num (x_), y__ = num (y_), z__ = num (z_);     \
        CallN       st_;                                                \
        PnExprNode *c_ = call3 (&st_, name_, &x__, &y__, &z__);         \
        PN_CHECK (pn_var_store_evaluate (s, c_, &out, NULL));           \
        PN_CHECK_NEAR (out, (want_), 1e-12);                            \
    } G_STMT_END

    /* Inside, below, above — and the argument ORDER, which is the whole
     * reason clamp exists rather than min(max(x, lo), hi). */
    CHECK3 ("clamp",  5.0, 0.0, 10.0,  5.0);
    CHECK3 ("clamp", -3.0, 0.0, 10.0,  0.0);
    CHECK3 ("clamp", 42.0, 0.0, 10.0, 10.0);
    CHECK3 ("clamp",  0.0, 0.0, 10.0,  0.0);   /* the bounds themselves */
    CHECK3 ("clamp", 10.0, 0.0, 10.0, 10.0);

    /* CROSSED BOUNDS: the lower one wins, whichever side x is on.  A
     * decision, not an accident (TODO #83.11b). */
    CHECK3 ("clamp",  5.0, 10.0, 0.0, 10.0);
    CHECK3 ("clamp", -5.0, 10.0, 0.0, 10.0);

    /* A NaN VALUE stays NaN rather than becoming a bound, which is what
     * the fmin/fmax spelling would have done silently. */
    {
        PnExprNode  x = num (NAN), lo = num (0.0), hi = num (1.0);
        CallN       st;
        PnExprNode *c = call3 (&st, "clamp", &x, &lo, &hi);
        PN_CHECK (pn_var_store_evaluate (s, c, &out, NULL));
        PN_CHECK (isnan (out));
    }

    /* The ranged row: one argument is the natural log, two is the log to
     * that base, and both come from the SAME row. */
    {
        PnExprNode  x = num (G_E);
        PnExprNode  c1 = call ("log", &x);
        PN_CHECK (pn_var_store_evaluate (s, &c1, &out, NULL));
        PN_CHECK_NEAR (out, 1.0, 1e-12);
    }
    {
        PnExprNode  x = num (8.0), b = num (2.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "log", &x, &b);
        PN_CHECK (pn_var_store_evaluate (s, c, &out, NULL));
        PN_CHECK_NEAR (out, 3.0, 1e-12);
    }
    {
        PnExprNode  x = num (1000.0), b = num (10.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "log", &x, &b);
        PN_CHECK (pn_var_store_evaluate (s, c, &out, NULL));
        PN_CHECK_NEAR (out, 3.0, 1e-12);
    }

    /* Outside the range it is a BAD_AST, and the message comes from the
     * table so it reads the way the parser's does. */
    {
        PnExprNode  a = num (1.0), b = num (2.0), c3 = num (3.0);
        CallN       st;
        PnExprNode *c = call3 (&st, "log", &a, &b, &c3);
        PN_CHECK_FALSE (pn_var_store_evaluate (s, c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        PN_CHECK (err != NULL && strstr (err->message, "1 or 2") != NULL);
        g_clear_error (&err);
    }
    {
        PnExprNode  a = num (1.0), b = num (2.0);
        CallN       st;
        PnExprNode *c = call2 (&st, "clamp", &a, &b);  /* one short */
        PN_CHECK_FALSE (pn_var_store_evaluate (s, c, &out, &err));
        PN_CHECK (g_error_matches (err, PN_VAR_STORE_ERROR,
                                   PN_VAR_STORE_ERROR_BAD_AST));
        PN_CHECK (err != NULL && strstr (err->message, "3 arguments") != NULL);
        g_clear_error (&err);
    }

#undef CHECK3

    g_object_unref (s);
}

/* The N-operand broadcast (TODO #83.19).  Three operands follow the same
 * rule two do — scalars broadcast, vectors are elementwise, the result
 * takes the LONGEST length — and where an operand has run out the first
 * one that still has an element passes through verbatim.  With three
 * that means the VALUE passes through unclamped, which is the same
 * promise `[2,3] * [3,4,5]` makes. */
static void
test_vector_three_argument_function (void)
{
    PnVarStore *s   = pn_var_store_new ();
    PnExprValue out = { NULL, 0.0 };
    gdouble     xs[] = { -5.0, 0.5, 9.0, 20.0 };
    gdouble     los[] = { 0.0, 0.0 };

    bind_vec (s, "xs",  xs,  4);
    bind_vec (s, "los", los, 2);

    /* vector value, scalar bounds: every element clamped. */
    {
        PnExprNode  xv = var ("xs"), lo = num (0.0), hi = num (10.0);
        CallN       st;
        PnExprNode *c = call3 (&st, "clamp", &xv, &lo, &hi);
        gdouble     want[] = { 0.0, 0.5, 9.0, 10.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        check_vec (&out, want, 4);
        pn_expr_value_clear (&out);
    }

    /* A SHORTER bound vector: the first two elements are clamped against
     * it, and where it has run out the value passes through verbatim —
     * unclamped, not clamped against nothing. */
    {
        PnExprNode  xv = var ("xs"), lov = var ("los"), hi = num (10.0);
        CallN       st;
        PnExprNode *c = call3 (&st, "clamp", &xv, &lov, &hi);
        gdouble     want[] = { 0.0, 0.5, 9.0, 20.0 };
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        check_vec (&out, want, 4);
        pn_expr_value_clear (&out);
    }

    /* All three scalars: still a scalar out, no vector allocated. */
    {
        PnExprNode  x = num (42.0), lo = num (0.0), hi = num (10.0);
        CallN       st;
        PnExprNode *c = call3 (&st, "clamp", &x, &lo, &hi);
        PN_CHECK (pn_var_store_evaluate_value (s, c, &out, NULL));
        PN_CHECK (out.vec == NULL);
        PN_CHECK_NEAR (out.scalar, 10.0, 1e-12);
        pn_expr_value_clear (&out);
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
    pn_test_add ("constants",          test_constants);
    pn_test_add ("vector_broadcast",   test_vector_broadcast);
    pn_test_add ("vector_elementwise", test_vector_elementwise);
    pn_test_add ("vector_fns_unary",   test_vector_functions_and_unary);
    pn_test_add ("vector_fn_2arg",     test_vector_two_argument_function);
    pn_test_add ("vector_comparison",  test_vector_comparison_reduces);
    pn_test_add ("scalar_sink_vector", test_scalar_sink_rejects_vector);
    pn_test_add ("value_to_string",    test_value_to_string);
    pn_test_add ("builtin_functions",  test_builtin_functions);
    pn_test_add ("domain_policy",      test_domain_policy);
    pn_test_add ("trig_hyperbolic",    test_trig_and_hyperbolic_rows);
    pn_test_add ("arity_three_range",  test_arity_three_and_range);
    pn_test_add ("vector_fn_3arg",     test_vector_three_argument_function);
    pn_test_add ("eval_bad_ast",       test_eval_bad_ast);
    return pn_test_run ();
}
