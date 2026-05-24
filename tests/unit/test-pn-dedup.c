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

/* Unit tests for PnDedup: forward a message only the first time the
 * value at its key path is seen; drop subsequent repeats.  The
 * timeout-based expiry (minutes) is never reached within a test run. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-dedup.h"

static PnNode *
make_node (const gchar *key, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_DEDUP, NULL);

    if (key != NULL)
        g_object_set (node, "key", key, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Send a message carrying data.value = @value through @node. */
static void
send_value (PnNode *node, const gchar *value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_drops_consecutive_duplicates (void)
{
    guint   emits;
    PnNode *node = make_node ("data/value", &emits);   /* the default key */

    send_value (node, "a");           /* first sight  -> forwarded */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, "a");           /* repeat       -> dropped   */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, "b");           /* new value    -> forwarded */
    PN_CHECK_CMPINT (emits, ==, 2);

    send_value (node, "a");           /* still remembered -> dropped */
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

/* Send a message carrying data.id = @id (and an unrelated data.value) so
 * a test can prove the dedup identity is taken from the configured key,
 * not from data.value. */
static void
send_id (PnNode *node, const gchar *id, gdouble value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "id", id);
    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_custom_key_dedups_on_that_path (void)
{
    guint   emits;
    PnNode *node = make_node ("data/id", &emits);

    /* Identity is data.id, so data.value moving while id repeats is
     * still a duplicate — the dedup follows the configured key. */
    send_id (node, "x", 1.0);          /* first id=x -> forwarded */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_id (node, "x", 2.0);          /* id repeats -> dropped   */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_id (node, "y", 2.0);          /* new id    -> forwarded  */
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

static void
test_missing_value_is_dropped (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/value", &emits);   /* default key */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* The watched path is absent from the message: there is no identity
     * to dedup against, so the node drops it rather than forwarding a
     * message it cannot reason about. */
    pn_message_set_string (msg, "output", "no value here");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_empty_key_forwards_nothing (void)
{
    guint   emits;
    PnNode *node = make_node ("", &emits);

    /* With no key configured the node has no identity to compare and
     * drops everything rather than forwarding blindly. */
    send_value (node, "a");
    send_value (node, "b");
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-dedup");
    pn_test_add ("drops_duplicates",       test_drops_consecutive_duplicates);
    pn_test_add ("custom_key_dedups",      test_custom_key_dedups_on_that_path);
    pn_test_add ("missing_value_dropped",  test_missing_value_is_dropped);
    pn_test_add ("empty_key_drops_all",    test_empty_key_forwards_nothing);
    return pn_test_run ();
}
