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

/* Unit tests for PnTextView.  The node displays each message's
 * data.output line in its on-canvas pane, but it also sits transparently
 * mid-pipeline: every received message is forwarded verbatim so a text
 * view can be dropped in as an inline inspector without breaking the
 * chain.  The display side needs no GUI here — the test asserts the
 * pass-through contract, which holds regardless of the (unrealised)
 * widget. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-text-view.h"

typedef struct
{
    guint      count;
    PnMessage *last;     /* most recent forwarded message (strong ref) */
} Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;

    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

static PnNode *
make_node (Capture *cap)
{
    PnNode *node = PN_NODE (pn_text_view_new ());

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

static void
test_forwards_message_with_output (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "line one");
    pn_node_receive_message (node, msg);

    /* Forwarded exactly once, and it is the very same message object
     * (verbatim pass-through, not a rebuilt copy). */
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (cap.last == msg);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "line one");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_forwards_message_without_output (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* No data.output: nothing to show, but the message must still pass
     * through so downstream nodes are not starved. */
    pn_message_set_double (msg, "value", 42.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 42.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_forwards_each_message (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *a    = pn_message_new (NULL, NULL);
    PnMessage *b    = pn_message_new (NULL, NULL);

    pn_message_set_string (a, "output", "first");
    pn_message_set_string (b, "output", "second");

    pn_node_receive_message (node, a);
    pn_node_receive_message (node, b);

    /* One emit per input, in order; the last one wins the display. */
    PN_CHECK_CMPINT (cap.count, ==, 2);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "second");

    g_clear_object (&cap.last);
    g_object_unref (a);
    g_object_unref (b);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-text-view");
    pn_test_add ("forwards_with_output",   test_forwards_message_with_output);
    pn_test_add ("forwards_no_output",     test_forwards_message_without_output);
    pn_test_add ("forwards_each",          test_forwards_each_message);
    return pn_test_run ();
}
