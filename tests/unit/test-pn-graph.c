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

/* Unit tests for PnGraph.  The plotting only matters on screen, so the
 * headless tests cover the receive() contract: it is a pure sink (never
 * forwards), it resolves its #key path and bins finite values into one
 * lazily-created series per message topic, an empty key / unparseable
 * value is a safe no-op, and the per-topic series fan-out is capped at
 * PN_GRAPH_MAX_SERIES (12).  Series creation is observed through
 * pn_graph_get_series_count since the bin store is private.  These pin
 * the logic half the headless/core split (TODO #23) must keep loadable
 * without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-graph.h"

/* Feed one finite reading carried under @topic into @node. */
static void
feed (PnNode *node, const gchar *topic, gdouble value)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_graph_new ());

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, "t", 1.0);
    feed (node, "t", 2.0);

    PN_CHECK_CMPINT (emits, ==, 0);
    g_object_unref (node);
}

static void
test_key_default (void)
{
    PnNode *node = PN_NODE (pn_graph_new ());
    gchar  *key  = NULL;

    g_object_get (node, "key", &key, NULL);
    PN_CHECK_CMPSTR (key, ==, "data/value");

    g_free (key);
    g_object_unref (node);
}

static void
test_series_per_topic (void)
{
    PnGraph *graph = pn_graph_new ();
    PnNode  *node  = PN_NODE (graph);

    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 0u);

    /* First topic creates a series; a second message on the same topic
     * reuses it; a distinct topic adds a second series. */
    feed (node, "alpha", 1.0);
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 1u);
    feed (node, "alpha", 2.0);
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 1u);
    feed (node, "beta", 3.0);
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 2u);

    g_object_unref (node);
}

static void
test_empty_key_is_noop (void)
{
    PnGraph *graph = pn_graph_new ();
    PnNode  *node  = PN_NODE (graph);

    /* No key configured: nothing to read, so no series is created. */
    g_object_set (graph, "key", "", NULL);
    feed (node, "alpha", 1.0);
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 0u);

    g_object_unref (node);
}

static void
test_missing_value_is_noop (void)
{
    PnGraph   *graph = pn_graph_new ();
    PnNode    *node  = PN_NODE (graph);
    PnMessage *msg   = pn_message_new (NULL, "alpha");

    /* A message with nothing at data/value never opens a series. */
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 0u);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_series_fan_out_is_capped (void)
{
    PnGraph *graph = pn_graph_new ();
    PnNode  *node  = PN_NODE (graph);
    guint    i;

    /* Thirteen distinct topics, but the fan-out tops out at the
     * PN_GRAPH_MAX_SERIES (12) ceiling; the surplus is dropped. */
    for (i = 0; i < 13; i++)
    {
        gchar *topic = g_strdup_printf ("topic-%u", i);
        feed (node, topic, (gdouble) i);
        g_free (topic);
    }
    PN_CHECK_CMPINT (pn_graph_get_series_count (graph), ==, 12u);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-graph");
    pn_test_add ("is_a_sink",               test_is_a_sink);
    pn_test_add ("key_default",             test_key_default);
    pn_test_add ("series_per_topic",        test_series_per_topic);
    pn_test_add ("empty_key_is_noop",       test_empty_key_is_noop);
    pn_test_add ("missing_value_is_noop",   test_missing_value_is_noop);
    pn_test_add ("series_fan_out_capped",   test_series_fan_out_is_capped);
    return pn_test_run ();
}
