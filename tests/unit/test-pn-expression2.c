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

/* Unit tests for PnExpression2: evaluate an algebraic expression over its
 * two inputs.  Each input's last data.value is remembered (by the core's
 * input-value collation) and bound under that input's name — value1 /
 * value2 by default, or a renamed input's name.  Any other numeric member
 * of the message being processed is bound with the arriving input's number
 * as a suffix (a sibling data.temp as temp1/temp2), but these siblings are
 * NOT remembered across inputs.  An unresolved variable forwards the
 * message flagged as a failure. */

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
    /* A sibling numeric member of the message being processed is bound
     * with the arriving input's suffix: arriving on input 1, data.value
     * binds as `value1` (via its input name) and a sibling data.temp as
     * `temp1`. */
    PnNode    *node = make_node ("value1 * temp1", &emits);
    PnMessage *m1   = pn_message_new (NULL, NULL);

    pn_message_set_double (m1, "value", 4.0);
    pn_message_set_double (m1, "temp",  5.0);
    pn_node_receive_message_on_input (node, m1, 0);   /* value1 * temp1 = 20 */

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m1, "value"), 20.0, 1e-9);
    PN_CHECK        (pn_test_bool (m1, "success"));

    g_object_unref (m1);
    g_object_unref (node);
}

static void
test_renamed_inputs (void)
{
    guint      emits;
    /* Renaming the inputs changes the variable names the latched values
     * surface under, so the expression can refer to them directly. */
    PnNode    *node = make_node ("left + right", &emits);
    PnMessage *m1   = pn_message_new (NULL, NULL);
    PnMessage *m2   = pn_message_new (NULL, NULL);

    pn_node_set_input_name (node, 0, "left");
    pn_node_set_input_name (node, 1, "right");

    pn_message_set_double (m1, "value", 2.0);
    pn_node_receive_message_on_input (node, m1, 0);   /* right still unbound */
    PN_CHECK_FALSE (pn_test_bool (m1, "success"));

    pn_message_set_double (m2, "value", 3.0);
    pn_node_receive_message_on_input (node, m2, 1);   /* left remembered → 5 */
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (m2, "value"), 5.0, 1e-9);
    PN_CHECK        (pn_test_bool (m2, "success"));

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

static void
test_default_input_count (void)
{
    PnNode *node = g_object_new (PN_TYPE_EXPRESSION2, NULL);
    gint    inputs = 0;

    /* Fresh node starts with two inputs, matching the property default. */
    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 2);
    g_object_get (node, "inputs", &inputs, NULL);
    PN_CHECK_CMPINT (inputs, ==, 2);

    g_object_unref (node);
}

static void
test_configurable_input_count (void)
{
    PnNode *node   = g_object_new (PN_TYPE_EXPRESSION2, NULL);
    gint    inputs = 0;

    /* Setting the property resizes the node's live input ports. */
    g_object_set (node, "inputs", 5, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 5);
    g_object_get (node, "inputs", &inputs, NULL);
    PN_CHECK_CMPINT (inputs, ==, 5);

    /* Shrinking works too. */
    g_object_set (node, "inputs", 3, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 3);

    g_object_unref (node);
}

static void
test_three_inputs_combined (void)
{
    guint      emits;
    PnNode    *node = make_node ("value1 + value2 + value3", &emits);
    PnMessage *m1   = pn_message_new (NULL, NULL);
    PnMessage *m2   = pn_message_new (NULL, NULL);
    PnMessage *m3   = pn_message_new (NULL, NULL);

    g_object_set (node, "inputs", 3, NULL);

    /* Feed all three inputs; the collated latches make every value
     * available once each input has fired. */
    pn_message_set_double (m1, "value", 2.0);
    pn_node_receive_message_on_input (node, m1, 0);   /* value2/value3 unbound */
    PN_CHECK_FALSE (pn_test_bool (m1, "success"));

    pn_message_set_double (m2, "value", 3.0);
    pn_node_receive_message_on_input (node, m2, 1);   /* value3 still unbound */
    PN_CHECK_FALSE (pn_test_bool (m2, "success"));

    pn_message_set_double (m3, "value", 4.0);
    pn_node_receive_message_on_input (node, m3, 2);   /* 2 + 3 + 4 = 9 */
    PN_CHECK_CMPINT (emits, ==, 3);
    PN_CHECK_NEAR   (pn_test_num (m3, "value"), 9.0, 1e-9);
    PN_CHECK        (pn_test_bool (m3, "success"));

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (m3);
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
    pn_test_add ("default_input_count",    test_default_input_count);
    pn_test_add ("configurable_inputs",    test_configurable_input_count);
    pn_test_add ("two_inputs_combined",    test_two_inputs_combined);
    pn_test_add ("three_inputs_combined",  test_three_inputs_combined);
    pn_test_add ("named_siblings_suffix",  test_named_siblings_get_input_suffix);
    pn_test_add ("renamed_inputs",         test_renamed_inputs);
    pn_test_add ("invalid_forwards_fail",  test_invalid_expression_forwards_failure);
    return pn_test_run ();
}
