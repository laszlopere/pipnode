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

/* Unit tests for PnEdge: forward a message only when the boolean
 * data.success differs from the previously latched value.  The first
 * well-formed message latches silently; messages without a boolean
 * "success" are ignored entirely (they neither latch nor forward). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-edge.h"

static PnNode *
make_node (guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_EDGE, NULL);

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
test_first_message_latches_silently (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* No prior state to compare the first message to, so it latches the
     * value and forwards nothing. */
    send_success (node, TRUE);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

static void
test_emits_only_on_change (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    send_success (node, TRUE);    /* latch TRUE        -> silent  */
    PN_CHECK_CMPINT (emits, ==, 0);

    send_success (node, TRUE);    /* unchanged         -> dropped */
    PN_CHECK_CMPINT (emits, ==, 0);

    send_success (node, FALSE);   /* TRUE -> FALSE     -> forward */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_success (node, FALSE);   /* unchanged         -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_success (node, TRUE);    /* FALSE -> TRUE     -> forward */
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

static void
test_messages_without_success_are_ignored (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* A message with no boolean "success" cannot latch state: the node
     * stays "unseen", so the next real message is still the first one
     * and latches silently rather than being treated as a transition. */
    send_bare (node);
    PN_CHECK_CMPINT (emits, ==, 0);

    send_success (node, TRUE);    /* this is the first latch -> silent */
    PN_CHECK_CMPINT (emits, ==, 0);

    send_success (node, FALSE);   /* genuine change          -> forward */
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-edge");
    pn_test_add ("first_latches_silent",   test_first_message_latches_silently);
    pn_test_add ("emits_on_change",        test_emits_only_on_change);
    pn_test_add ("ignores_non_boolean",    test_messages_without_success_are_ignored);
    return pn_test_run ();
}
