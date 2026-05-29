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

/* Unit tests for the per-node in-memory log ring (pn_node_log et al.) —
 * the GTK-free storage half of the "nodes report diagnostics into a Log
 * dialog instead of stdout" feature.  The dialog itself is GUI and
 * exercised by hand; here we pin the core contract: entries accumulate
 * oldest-first, printf formatting works, the ring is capped (oldest
 * dropped), clear empties it, and both append and clear fire
 * "log-changed". */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"

/* Mirrors PN_NODE_LOG_MAX in pn-node.c — kept in sync by hand; if the
 * cap there changes, update this expectation. */
#define EXPECT_LOG_MAX 200

static void
test_basic_append (void)
{
    PnNode    *node = pn_node_new ();
    GPtrArray *log;

    log = pn_node_get_log (node);
    PN_CHECK (log != NULL);
    PN_CHECK_CMPINT (log->len, ==, 0);

    pn_node_log (node, PN_LOG_LEVEL_INFO, "hello %d", 42);
    pn_node_log_warning (node, "watch out: %s", "x");
    pn_node_log_error   (node, "boom");

    log = pn_node_get_log (node);
    PN_CHECK_CMPINT (log->len, ==, 3);

    {
        const PnLogEntry *e0 = g_ptr_array_index (log, 0);
        const PnLogEntry *e1 = g_ptr_array_index (log, 1);
        const PnLogEntry *e2 = g_ptr_array_index (log, 2);

        /* Oldest first. */
        PN_CHECK_CMPINT (pn_log_entry_get_level (e0), ==, PN_LOG_LEVEL_INFO);
        PN_CHECK_CMPSTR (pn_log_entry_get_message (e0), ==, "hello 42");
        PN_CHECK_CMPINT (pn_log_entry_get_level (e1), ==, PN_LOG_LEVEL_WARNING);
        PN_CHECK_CMPSTR (pn_log_entry_get_message (e1), ==, "watch out: x");
        PN_CHECK_CMPINT (pn_log_entry_get_level (e2), ==, PN_LOG_LEVEL_ERROR);
        PN_CHECK_CMPSTR (pn_log_entry_get_message (e2), ==, "boom");

        /* Timestamp is a real wall-clock value. */
        PN_CHECK (pn_log_entry_get_time (e2) > 0);
    }

    g_object_unref (node);
}

static void
test_level_strings (void)
{
    PN_CHECK_CMPSTR (pn_log_level_to_string (PN_LOG_LEVEL_INFO),    ==, "INFO");
    PN_CHECK_CMPSTR (pn_log_level_to_string (PN_LOG_LEVEL_WARNING), ==, "WARNING");
    PN_CHECK_CMPSTR (pn_log_level_to_string (PN_LOG_LEVEL_ERROR),   ==, "ERROR");
}

static void
test_ring_cap (void)
{
    PnNode    *node = pn_node_new ();
    GPtrArray *log;
    int        i;

    /* Overfill the ring by 50 entries.  The oldest 50 should be dropped,
     * leaving exactly EXPECT_LOG_MAX entries with #50 now at the head. */
    for (i = 0; i < EXPECT_LOG_MAX + 50; i++)
        pn_node_log (node, PN_LOG_LEVEL_INFO, "entry-%d", i);

    log = pn_node_get_log (node);
    PN_CHECK_CMPINT (log->len, ==, EXPECT_LOG_MAX);

    {
        const PnLogEntry *first = g_ptr_array_index (log, 0);
        const PnLogEntry *last  = g_ptr_array_index (log, log->len - 1);

        PN_CHECK_CMPSTR (pn_log_entry_get_message (first), ==, "entry-50");
        PN_CHECK_CMPSTR (pn_log_entry_get_message (last),  ==,
                         "entry-249");
    }

    g_object_unref (node);
}

static void
test_clear (void)
{
    PnNode *node = pn_node_new ();

    pn_node_log (node, PN_LOG_LEVEL_INFO, "a");
    pn_node_log (node, PN_LOG_LEVEL_INFO, "b");
    PN_CHECK_CMPINT (pn_node_get_log (node)->len, ==, 2);

    pn_node_clear_log (node);
    PN_CHECK_CMPINT (pn_node_get_log (node)->len, ==, 0);

    g_object_unref (node);
}

static void
on_log_changed (PnNode *node, gpointer user_data)
{
    int *count = user_data;
    (void) node;
    (*count)++;
}

static void
test_signal (void)
{
    PnNode *node  = pn_node_new ();
    int     count = 0;

    g_signal_connect (node, "log-changed",
                      G_CALLBACK (on_log_changed), &count);

    pn_node_log (node, PN_LOG_LEVEL_INFO, "one");
    pn_node_log (node, PN_LOG_LEVEL_INFO, "two");
    PN_CHECK_CMPINT (count, ==, 2);

    /* clear fires it once more; a no-op clear on an empty ring does not. */
    pn_node_clear_log (node);
    PN_CHECK_CMPINT (count, ==, 3);
    pn_node_clear_log (node);
    PN_CHECK_CMPINT (count, ==, 3);

    g_object_unref (node);
}

static void
test_entry_copy (void)
{
    PnNode     *node = pn_node_new ();
    PnLogEntry *copy;

    pn_node_log (node, PN_LOG_LEVEL_ERROR, "original");
    copy = pn_log_entry_copy (g_ptr_array_index (pn_node_get_log (node), 0));

    /* The copy outlives the node and reads back identically. */
    g_object_unref (node);

    PN_CHECK (copy != NULL);
    PN_CHECK_CMPINT (pn_log_entry_get_level (copy), ==, PN_LOG_LEVEL_ERROR);
    PN_CHECK_CMPSTR (pn_log_entry_get_message (copy), ==, "original");

    pn_log_entry_free (copy);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-node-log");
    pn_test_add ("basic_append",  test_basic_append);
    pn_test_add ("level_strings", test_level_strings);
    pn_test_add ("ring_cap",      test_ring_cap);
    pn_test_add ("clear",         test_clear);
    pn_test_add ("signal",        test_signal);
    pn_test_add ("entry_copy",    test_entry_copy);
    return pn_test_run ();
}
