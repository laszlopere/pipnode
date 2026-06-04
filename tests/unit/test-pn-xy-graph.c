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

/* Unit tests for PnXYGraph.  The plotting only matters on screen, so the
 * headless tests cover the receive() contract: it is a pure sink (never
 * forwards), it resolves both its #x-key and #key paths and only opens a
 * series when both yield a finite number, the per-topic fan-out is capped
 * at PN_XY_GRAPH_MAX_SERIES (12), and the chronological reader returns the
 * most recent samples oldest-first.  Series creation is observed through
 * pn_xy_graph_get_series_count since the sample store is private.  These
 * pin the logic half the headless/core split (TODO #23) keeps loadable
 * without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-xy-graph.h"

/* Feed one (x, y) sample carried under @topic into @node. */
static void
feed (PnNode *node, const gchar *topic, gdouble x, gdouble y)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_message_set_double (msg, "x", x);
    pn_message_set_double (msg, "value", y);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_is_a_sink (void)
{
    guint   emits = 0;
    PnNode *node  = PN_NODE (pn_xy_graph_new ());

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, "t", 1.0, 10.0);
    feed (node, "t", 2.0, 20.0);

    PN_CHECK_CMPINT (emits, ==, 0);
    g_object_unref (node);
}

static void
test_key_defaults (void)
{
    PnNode *node  = PN_NODE (pn_xy_graph_new ());
    gchar  *ykey  = NULL;
    gchar  *xkey  = NULL;

    g_object_get (node, "key", &ykey, "x-key", &xkey, NULL);
    PN_CHECK_CMPSTR (ykey, ==, "data/value");
    PN_CHECK_CMPSTR (xkey, ==, "data/x");

    g_free (ykey);
    g_free (xkey);
    g_object_unref (node);
}

static void
test_series_per_topic (void)
{
    PnXYGraph *graph = pn_xy_graph_new ();
    PnNode    *node  = PN_NODE (graph);

    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 0u);

    feed (node, "alpha", 1.0, 1.0);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 1u);
    feed (node, "alpha", 2.0, 2.0);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 1u);
    feed (node, "beta", 3.0, 3.0);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 2u);

    g_object_unref (node);
}

static void
test_missing_coord_is_noop (void)
{
    PnXYGraph *graph = pn_xy_graph_new ();
    PnNode    *node  = PN_NODE (graph);
    PnMessage *only_y;
    PnMessage *only_x;

    /* A message carrying only Y (no data/x) opens no series... */
    only_y = pn_message_new (NULL, "alpha");
    pn_message_set_double (only_y, "value", 5.0);
    pn_node_receive_message (node, only_y);
    g_object_unref (only_y);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 0u);

    /* ...nor does one carrying only X. */
    only_x = pn_message_new (NULL, "alpha");
    pn_message_set_double (only_x, "x", 5.0);
    pn_node_receive_message (node, only_x);
    g_object_unref (only_x);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 0u);

    g_object_unref (node);
}

static void
test_empty_key_is_noop (void)
{
    PnXYGraph *graph = pn_xy_graph_new ();
    PnNode    *node  = PN_NODE (graph);

    /* Clearing either key disables the node: nothing to read. */
    g_object_set (graph, "key", "", NULL);
    feed (node, "alpha", 1.0, 1.0);
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 0u);

    g_object_unref (node);
}

static void
test_series_fan_out_is_capped (void)
{
    PnXYGraph *graph = pn_xy_graph_new ();
    PnNode    *node  = PN_NODE (graph);
    guint      i;

    /* Thirteen distinct topics, but the fan-out tops out at the
     * PN_XY_GRAPH_MAX_SERIES (12) ceiling; the surplus is dropped. */
    for (i = 0; i < 13; i++)
    {
        gchar *topic = g_strdup_printf ("topic-%u", i);
        feed (node, topic, (gdouble) i, (gdouble) i);
        g_free (topic);
    }
    PN_CHECK_CMPINT (pn_xy_graph_get_series_count (graph), ==, 12u);

    g_object_unref (node);
}

static void
test_chrono_is_oldest_first (void)
{
    PnXYGraph           *graph = pn_xy_graph_new ();
    PnNode              *node  = PN_NODE (graph);
    PnXYGraphSeriesView *views;
    guint                nseries = 0;
    gdouble              xs[8], ys[8];
    guint                n;

    feed (node, "t", 1.0, 10.0);
    feed (node, "t", 2.0, 20.0);
    feed (node, "t", 3.0, 30.0);

    views = pn_xy_graph_collect_series_sorted (graph, &nseries);
    PN_CHECK_CMPINT (nseries, ==, 1u);

    /* Cap below the sample count keeps only the most recent two,
     * oldest-first: (2,20) then (3,30). */
    n = pn_xy_graph_series_chrono (views[0].series, 2, xs, ys);
    PN_CHECK_CMPINT (n, ==, 2u);
    PN_CHECK_NEAR (xs[0], 2.0,  1e-9);
    PN_CHECK_NEAR (ys[0], 20.0, 1e-9);
    PN_CHECK_NEAR (xs[1], 3.0,  1e-9);
    PN_CHECK_NEAR (ys[1], 30.0, 1e-9);

    g_free (views);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-xy-graph");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("key_defaults",           test_key_defaults);
    pn_test_add ("series_per_topic",       test_series_per_topic);
    pn_test_add ("missing_coord_is_noop",  test_missing_coord_is_noop);
    pn_test_add ("empty_key_is_noop",      test_empty_key_is_noop);
    pn_test_add ("series_fan_out_capped",  test_series_fan_out_is_capped);
    pn_test_add ("chrono_oldest_first",    test_chrono_is_oldest_first);
    return pn_test_run ();
}
