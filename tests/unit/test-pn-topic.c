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

/* Unit tests for PnTopic: rewrite a message's topic from the node's own
 * base "topic" property, expanding ${nodeclass}/${nodename}/${hostname}
 * the same way every emitter does; the rest of the message passes through. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-topic.h"

static PnNode *
make_node (const gchar *topic, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_TOPIC, "topic", topic, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* A literal topic (no placeholders) replaces the incoming topic and the
 * message is forwarded once. */
static void
test_replaces_topic (void)
{
    guint      emits;
    PnNode    *node = make_node ("sensors/kitchen", &emits);
    PnMessage *msg  = pn_message_new (NULL, "old/topic");

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "sensors/kitchen");

    g_object_unref (msg);
    g_object_unref (node);
}

/* The ${nodeclass} placeholder in the topic template expands to the
 * node's class name ("Topic"), proving the rewrite goes through the
 * shared pn_node_resolve_topic() path rather than a raw string copy. */
static void
test_expands_nodeclass_placeholder (void)
{
    guint      emits;
    PnNode    *node = make_node ("x/${nodeclass}/y", &emits);
    PnMessage *msg  = pn_message_new (NULL, "old/topic");

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "x/Topic/y");

    g_object_unref (msg);
    g_object_unref (node);
}

/* Only the topic is touched; the data bag passes through untouched. */
static void
test_passes_data_through (void)
{
    guint      emits;
    PnNode    *node = make_node ("new/topic", &emits);
    PnMessage *msg  = pn_message_new (NULL, "old/topic");

    pn_message_set_double (msg, "value",  42.0);
    pn_message_set_string (msg, "output", "payload");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "new/topic");
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 42.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "payload");

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-topic");
    pn_test_add ("replaces_topic",      test_replaces_topic);
    pn_test_add ("expands_placeholder", test_expands_nodeclass_placeholder);
    pn_test_add ("passes_data_through", test_passes_data_through);
    return pn_test_run ();
}
