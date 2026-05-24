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

/* Unit tests for PnExpression2: evaluate an algebraic expression over
 * the numeric members of its two inputs, where input 1's members are
 * bound with a "1" suffix and input 2's with a "2" suffix.  The latest
 * value seen on each input is retained across messages, and any
 * unresolved variable forwards the message flagged as a failure. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-expression2.h"

static PnNode *
make_node (const gchar *expr, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_EXPRESSION2, NULL);

    if (expr != NULL)
        g_object_set (node, "expression", expr, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
test_single_input_expression (void)
{
    guint      emits;
    PnNode    *node = make_node ("value1 * 2", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A plain pn_node_receive_message() delivers on input 0, so the
     * sent value binds as value1. */
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
test_two_inputs_combined (void)
{
    guint      emits;
    PnNode    *node = make_node ("value1 + value2", &emits);  /* the default */
    PnMessage *m1   = pn_message_new (NULL, NULL);
    PnMessage *m2   = pn_message_new (NULL, NULL);

    /* Input 1 alone: value2 is still unbound, so evaluation fails and
     * the message is forwarded flagged as a failure. */
    pn_message_set_double (m1, "value", 2.0);
    pn_node_receive_message_on_input (node, m1, 0);
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (m1, "success"));

    /* Input 2 arrives: input 1's value is still remembered, so the sum
     * resolves and is written to the second message's data.value. */
    pn_message_set_double (m2, "value", 3.0);
    pn_node_receive_message_on_input (node, m2, 1);
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (m2, "value"), 5.0, 1e-9);
    PN_CHECK        (pn_test_bool (m2, "success"));

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

static void
test_named_siblings_get_input_suffix (void)
{
    guint      emits;
    /* The suffix scheme applies to every numeric member, not just
     * "value": a member `a` on input 1 binds as `a1`, a member `b` on
     * input 2 as `b2`. */
    PnNode    *node = make_node ("a1 * b2", &emits);
    PnMessage *m1   = pn_message_new (NULL, NULL);
    PnMessage *m2   = pn_message_new (NULL, NULL);

    pn_message_set_double (m1, "a", 4.0);
    pn_node_receive_message_on_input (node, m1, 0);   /* b2 still unbound */
    PN_CHECK_FALSE (pn_test_bool (m1, "success"));

    pn_message_set_double (m2, "b", 5.0);
    pn_node_receive_message_on_input (node, m2, 1);   /* now a1 * b2 = 20 */
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (m2, "value"), 20.0, 1e-9);
    PN_CHECK        (pn_test_bool (m2, "success"));

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

static void
test_invalid_expression_forwards_failure (void)
{
    guint      emits;
    PnNode    *node = make_node ("value1 +", &emits);   /* parse error */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);

    /* Still forwarded so a downstream chain keeps flowing, but flagged
     * as a failure with an explanatory error member. */
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (msg, "success"));
    PN_CHECK        (pn_test_has  (msg, "error"));

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-expression2");
    pn_test_add ("single_input",           test_single_input_expression);
    pn_test_add ("two_inputs_combined",    test_two_inputs_combined);
    pn_test_add ("named_siblings_suffix",  test_named_siblings_get_input_suffix);
    pn_test_add ("invalid_forwards_fail",  test_invalid_expression_forwards_failure);
    return pn_test_run ();
}
