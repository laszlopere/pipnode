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

/* Unit tests for PnDial.  The needle animation only matters on screen,
 * so the headless tests cover the receive() contract that holds without
 * a display: it is a pure sink (never forwards), it resolves its #key
 * path against the message and surfaces the raw reading on the
 * read-only "value" property (un-clamped — clamping is display-only),
 * an unparseable / missing value or an empty key is a safe no-op, and a
 * repeated value is deduplicated.  These pin the logic half that the
 * headless/core split (TODO #23) must keep loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-dial.h"

/* Read the read-only "value" property the needle was last sent to. */
static gdouble
dial_value (PnNode *node)
{
    gdouble v = -1.0;
    g_object_get (node, "value", &v, NULL);
    return v;
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_dial_new ());
    PnMessage *msg   = pn_message_new (NULL, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Receiving moves the needle but produces no downstream message. */
    pn_message_set_double (msg, "value", 42.0);
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_key_default (void)
{
    PnNode *node = PN_NODE (pn_dial_new ());
    gchar  *key  = NULL;

    /* The default path matches the data.value convention every other
     * keyed node uses, so a bare numeric feed wires in unchanged. */
    g_object_get (node, "key", &key, NULL);
    PN_CHECK_CMPSTR (key, ==, "data/value");

    g_free (key);
    g_object_unref (node);
}

static void
test_surfaces_received_value (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* Fresh dial parks at 0 until it sees a reading. */
    PN_CHECK_NEAR (dial_value (node), 0.0, 1e-9);

    pn_message_set_double (msg, "value", 42.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_NEAR (dial_value (node), 42.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_value_is_raw_not_clamped (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* The default range is [0, 120]; a reading past the top still
     * surfaces verbatim on the property -- the clamp lives in the
     * (display-only) needle geometry, not in the recorded value. */
    pn_message_set_double (msg, "value", 999.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_NEAR (dial_value (node), 999.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_missing_value_is_noop (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *seed = pn_message_new (NULL, NULL);
    PnMessage *bare = pn_message_new (NULL, NULL);

    pn_message_set_double (seed, "value", 42.0);
    pn_node_receive_message (node, seed);
    PN_CHECK_NEAR (dial_value (node), 42.0, 1e-9);

    /* A message with nothing at data/value leaves the needle where it
     * was rather than snapping it to zero. */
    pn_node_receive_message (node, bare);
    PN_CHECK_NEAR (dial_value (node), 42.0, 1e-9);

    g_object_unref (seed);
    g_object_unref (bare);
    g_object_unref (node);
}

static void
test_reads_custom_key_path (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* Point the dial at a non-default path; the needle then reads the
     * value sitting there rather than at data.value. */
    g_object_set (node, "key", "data/rpm", NULL);
    pn_message_set_double (msg, "rpm", 3000.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_NEAR (dial_value (node), 3000.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_string_value_drives_needle (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A numeric quantity arriving as a JSON string (as JSON-RPC feeds
     * often do) is parsed and still moves the needle. */
    pn_message_set_string (msg, "value", "55");
    pn_node_receive_message (node, msg);
    PN_CHECK_NEAR (dial_value (node), 55.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* Bumped by every notify::value, so a test can count how many times the
 * needle was actually retargeted. */
static void
on_value_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void) obj; (void) pspec;
    (*(guint *) user_data)++;
}

static void
test_repeated_value_deduplicated (void)
{
    PnNode    *node    = PN_NODE (pn_dial_new ());
    guint      notifies = 0;
    PnMessage *msg;

    g_signal_connect (node, "notify::value",
                      G_CALLBACK (on_value_notify), &notifies);

    /* First reading retargets the needle and notifies once. */
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 42.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
    PN_CHECK_CMPINT (notifies, ==, 1);

    /* The same reading again is a no-op: no retarget, no extra notify. */
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 42.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
    PN_CHECK_CMPINT (notifies, ==, 1);

    g_object_unref (node);
}

static void
test_empty_key_gates_receive (void)
{
    PnNode    *node = PN_NODE (pn_dial_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* With no key configured the dial has nothing to read, so receive
     * is a no-op and the value stays parked at zero. */
    g_object_set (node, "key", "", NULL);
    pn_message_set_double (msg, "value", 5.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_NEAR (dial_value (node), 0.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-dial");
    pn_test_add ("is_a_sink",               test_is_a_sink);
    pn_test_add ("key_default",             test_key_default);
    pn_test_add ("surfaces_received_value", test_surfaces_received_value);
    pn_test_add ("value_is_raw_not_clamped", test_value_is_raw_not_clamped);
    pn_test_add ("missing_value_is_noop",   test_missing_value_is_noop);
    pn_test_add ("reads_custom_key",        test_reads_custom_key_path);
    pn_test_add ("string_value_drives",     test_string_value_drives_needle);
    pn_test_add ("repeated_value_dedup",    test_repeated_value_deduplicated);
    pn_test_add ("empty_key_gates_receive", test_empty_key_gates_receive);
    return pn_test_run ();
}
