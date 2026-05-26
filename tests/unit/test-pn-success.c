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

/* Unit tests for PnSuccess: forward a message only when its boolean
 * data.success member is TRUE.  Messages without a boolean "success"
 * are dropped. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-success.h"

static PnNode *
make_node (guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_SUCCESS, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Send a message carrying data.success = @value through @node. */
static void
send_success (PnNode *node, gboolean value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_boolean (msg, "success", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Send a message with no boolean "success" member at all. */
static void
send_bare (PnNode *node)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_forwards_only_true (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    send_success (node, TRUE);
    PN_CHECK_CMPINT (emits, ==, 1);

    send_success (node, FALSE);
    PN_CHECK_CMPINT (emits, ==, 1);

    send_success (node, TRUE);
    PN_CHECK_CMPINT (emits, ==, 2);

    send_success (node, TRUE);
    PN_CHECK_CMPINT (emits, ==, 3);

    g_object_unref (node);
}

static void
test_messages_without_success_are_dropped (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* A message missing the contract field is dropped — the node
     * cannot decide success vs failure, so it refuses to pass it. */
    send_bare (node);
    PN_CHECK_CMPINT (emits, ==, 0);

    /* Subsequent well-formed traffic still flows normally. */
    send_success (node, TRUE);
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

static void
test_non_boolean_success_is_dropped (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* Wrong-typed contract field: a number where the spec says boolean.
     * Dropped, same as a missing field. */
    pn_message_set_double (msg, "success", 1.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-success");
    pn_test_add ("forwards_only_true",         test_forwards_only_true);
    pn_test_add ("drops_without_success",      test_messages_without_success_are_dropped);
    pn_test_add ("drops_non_boolean_success",  test_non_boolean_success_is_dropped);
    return pn_test_run ();
}
