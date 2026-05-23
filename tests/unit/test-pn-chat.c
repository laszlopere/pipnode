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

/* Unit tests for PnChat.  The bubble layout and on-canvas text entry
 * only matter on screen, so the headless tests cover the contract that
 * holds without a display: receiving a message resolves its text/sender
 * paths and buffers a bubble (capped at #limit), an empty text or a
 * self-loop message (source == this node) buffers nothing, receive()
 * never forwards downstream, and the focus flag round-trips.  Bubble
 * accumulation is observed through pn_chat_get_bubble_count since the
 * bubble store is private.  These pin the logic half the headless/core
 * split (TODO #23) must keep loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-chat.h"

/* Deliver a message carrying @text under the default text path, from an
 * arbitrary @source (NULL = "someone else"). */
static void
deliver (PnNode *node, PnNode *source, const gchar *text)
{
    PnMessage *msg = pn_message_new (source, NULL);
    if (text != NULL)
        pn_message_set_string (msg, "output", text);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_receive_is_a_sink (void)
{
    guint    emits = 0;
    PnChat  *chat  = pn_chat_new ();
    PnNode  *node  = PN_NODE (chat);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Receiving buffers a bubble but never forwards a message. */
    deliver (node, NULL, "hello");
    deliver (node, NULL, "world");

    PN_CHECK_CMPINT (emits, ==, 0);
    g_object_unref (node);
}

static void
test_path_defaults (void)
{
    PnChat *chat   = pn_chat_new ();
    gchar  *text   = NULL;
    gchar  *sender = NULL;

    g_object_get (chat, "text-path", &text, "sender-path", &sender, NULL);
    PN_CHECK_CMPSTR (text,   ==, "data/output");
    PN_CHECK_CMPSTR (sender, ==, "data/from_long_name");

    g_free (text);
    g_free (sender);
    g_object_unref (chat);
}

static void
test_receive_pushes_bubble (void)
{
    PnChat *chat = pn_chat_new ();
    PnNode *node = PN_NODE (chat);

    PN_CHECK_CMPINT (pn_chat_get_bubble_count (chat), ==, 0u);
    deliver (node, NULL, "hi there");
    PN_CHECK_CMPINT (pn_chat_get_bubble_count (chat), ==, 1u);

    g_object_unref (node);
}

static void
test_empty_text_buffers_nothing (void)
{
    PnChat *chat = pn_chat_new ();
    PnNode *node = PN_NODE (chat);

    /* No text payload at all, and an explicit empty string, both leave
     * the history untouched. */
    deliver (node, NULL, NULL);
    deliver (node, NULL, "");
    PN_CHECK_CMPINT (pn_chat_get_bubble_count (chat), ==, 0u);

    g_object_unref (node);
}

static void
test_self_loop_is_suppressed (void)
{
    PnChat *chat = pn_chat_new ();
    PnNode *node = PN_NODE (chat);

    /* A message whose source is this very node is the loop-back of its
     * own send; the send path already buffered the bubble locally, so
     * receive() must drop it rather than duplicate it. */
    deliver (node, node, "echo");
    PN_CHECK_CMPINT (pn_chat_get_bubble_count (chat), ==, 0u);

    g_object_unref (node);
}

static void
test_buffer_trims_to_limit (void)
{
    PnChat *chat = pn_chat_new ();
    PnNode *node = PN_NODE (chat);

    g_object_set (chat, "limit", 2u, NULL);
    deliver (node, NULL, "one");
    deliver (node, NULL, "two");
    deliver (node, NULL, "three");
    PN_CHECK_CMPINT (pn_chat_get_bubble_count (chat), ==, 2u);

    g_object_unref (node);
}

static void
test_focus_round_trips (void)
{
    PnChat *chat = pn_chat_new ();

    /* A fresh chat is not focused; the flag round-trips both ways. */
    PN_CHECK_FALSE (pn_chat_get_focused (chat));
    pn_chat_set_focused (chat, TRUE);
    PN_CHECK (pn_chat_get_focused (chat));
    pn_chat_set_focused (chat, FALSE);
    PN_CHECK_FALSE (pn_chat_get_focused (chat));

    g_object_unref (chat);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-chat");
    pn_test_add ("receive_is_a_sink",        test_receive_is_a_sink);
    pn_test_add ("path_defaults",            test_path_defaults);
    pn_test_add ("receive_pushes_bubble",    test_receive_pushes_bubble);
    pn_test_add ("empty_text_buffers_nothing", test_empty_text_buffers_nothing);
    pn_test_add ("self_loop_is_suppressed",  test_self_loop_is_suppressed);
    pn_test_add ("buffer_trims_to_limit",    test_buffer_trims_to_limit);
    pn_test_add ("focus_round_trips",        test_focus_round_trips);
    return pn_test_run ();
}
