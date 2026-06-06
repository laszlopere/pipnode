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

/* Unit tests for PnExpression: evaluate an arithmetic expression over
 * the numeric members of the incoming message and stamp the result
 * onto data.value / data.success / data.output. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-expression.h"
#include "pn-vector.h"

/* Build a PnExpression with @expr installed (NULL keeps the empty
 * default).  @out_emits is wired to the node's output and counts the
 * messages it forwards. */
static PnNode *
make_node (const gchar *expr, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_EXPRESSION, NULL);

    if (expr != NULL)
        g_object_set (node, "expression", expr, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
test_eval_arithmetic (void)
{
    guint      emits;
    PnNode    *node = make_node ("value * 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 21.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 42.0, 1e-9);
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "42");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_eval_siblings (void)
{
    guint      emits;
    PnNode    *node = make_node ("a + b", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "a", 2.0);
    pn_message_set_double (msg, "b", 3.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 5.0, 1e-9);
    PN_CHECK        (pn_test_bool (msg, "success"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_binds_integer_member (void)
{
    guint      emits;
    PnNode    *node = make_node ("value * 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* An integer-typed member binds as a variable just like a double —
     * the receive loop accepts G_TYPE_INT64 alongside G_TYPE_DOUBLE. */
    pn_message_set_int (msg, "value", 21);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 42.0, 1e-9);
    PN_CHECK        (pn_test_bool (msg, "success"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_eval_comparison_boolean (void)
{
    guint      emits;
    /* A comparison expression: the node stamps data.value with the
     * boolean result encoded as 1.0 (true) / 0.0 (false). */
    PnNode    *node = make_node ("value > 10", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 21.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "1");

    /* Same node, a value that fails the test -> 0.0. */
    pn_message_set_double (msg, "value", 3.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 0.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "0");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_multi_statement_surfaces_assignments (void)
{
    guint      emits;
    /* Three statements: two assignments plus a final bare expression.
     * data.value is the last line; the assigned names are also written
     * onto the outgoing message as numeric members. */
    PnNode    *node = make_node ("f = value * 1.8 + 32\nk = value + 273.15\nf",
                                 &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 100.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (pn_test_bool (msg, "success"));
    /* data.value == the last statement (f). */
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 212.0, 1e-9);
    /* Both assigned names surfaced as data-bag members. */
    PN_CHECK_NEAR   (pn_test_num (msg, "f"), 212.0,    1e-9);
    PN_CHECK_NEAR   (pn_test_num (msg, "k"), 373.15,   1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_eval_failure_preserves_value (void)
{
    guint      emits;
    /* A valid expression that references a variable the message does not
     * carry: this parses fine but fails at evaluation time — a different
     * path from the empty-expression (AST == NULL) case. */
    PnNode    *node = make_node ("missing * 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 7.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (msg, "success"));
    PN_CHECK        (pn_test_has  (msg, "error"));
    /* On evaluation failure data.value is left exactly as it arrived. */
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 7.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* A `$pnvector` input is read back as a vector variable, the expression
 * broadcasts a scalar over it, and the result is written to data.value as
 * a new `$pnvector` marker — the full read/evaluate/write wiring (TODO
 * #43.7). */
static void
test_eval_vector_broadcast (void)
{
    guint      emits;
    PnNode    *node = make_node ("value * 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    gdouble    in[] = { 1.0, 2.0, 3.0 };
    PnVector  *vec  = pn_vector_new_copy (in, 3);
    PnVector  *out;

    pn_message_set_vector (msg, "value", vec);
    g_object_unref (vec);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (pn_test_bool (msg, "success"));

    /* data.value is a vector marker resolving to [2, 4, 6]. */
    out = pn_message_resolve_vector (msg, pn_message_get_member (msg, "value"));
    PN_CHECK (out != NULL);
    if (out != NULL)
    {
        const gdouble *d = pn_vector_get_data (out);
        PN_CHECK_CMPINT ((gint) pn_vector_get_len (out), ==, 3);
        PN_CHECK_NEAR (d[0], 2.0, 1e-9);
        PN_CHECK_NEAR (d[1], 4.0, 1e-9);
        PN_CHECK_NEAR (d[2], 6.0, 1e-9);
    }

    g_object_unref (msg);
    g_object_unref (node);
}

/* A comparison over a vector collapses to a scalar 0/1 (all()-semantics),
 * so a vector-fed comparison still drives data.value as a plain boolean. */
static void
test_eval_vector_comparison_scalar (void)
{
    guint      emits;
    PnNode    *node = make_node ("value > 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    gdouble    in[] = { 3.0, 5.0, 4.0 };       /* all > 2 */
    PnVector  *vec  = pn_vector_new_copy (in, 3);

    pn_message_set_vector (msg, "value", vec);
    g_object_unref (vec);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (pn_test_bool (msg, "success"));
    /* Reduced to a scalar 1.0, not a vector marker. */
    PN_CHECK (pn_message_resolve_vector (msg,
                  pn_message_get_member (msg, "value")) == NULL);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "1");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_expression_forwards_failure (void)
{
    guint      emits;
    /* The default expression is "value" (pass-through); clearing it to
     * the empty string is what leaves the node with nothing to do. */
    PnNode    *node = make_node ("", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);

    /* Still forwarded so the downstream chain keeps flowing, but the
     * message is flagged as a failure with an error explaining why. */
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (msg, "success"));
    PN_CHECK        (pn_test_has  (msg, "error"));

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-expression");
    pn_test_add ("eval_arithmetic",            test_eval_arithmetic);
    pn_test_add ("eval_siblings",              test_eval_siblings);
    pn_test_add ("binds_integer_member",       test_binds_integer_member);
    pn_test_add ("eval_comparison_boolean",    test_eval_comparison_boolean);
    pn_test_add ("multi_statement_surfaces",   test_multi_statement_surfaces_assignments);
    pn_test_add ("eval_failure_keeps_value",   test_eval_failure_preserves_value);
    pn_test_add ("eval_vector_broadcast",      test_eval_vector_broadcast);
    pn_test_add ("eval_vector_comparison",     test_eval_vector_comparison_scalar);
    pn_test_add ("no_expression_forwards",     test_no_expression_forwards_failure);
    return pn_test_run ();
}
