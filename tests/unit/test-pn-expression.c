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
    pn_test_add ("no_expression_forwards",     test_no_expression_forwards_failure);
    return pn_test_run ();
}
