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
test_series_from_label (void)
{
    PnGraph           *graph = pn_graph_new ();
    PnNode            *node  = PN_NODE (graph);
    PnNode            *src   = PN_NODE (pn_graph_new ());
    PnMessage         *msg;
    PnGraphSeriesView *views;
    guint              n     = 0;

    /* The series records the feeding node's name as its "from" label
     * (used to caption the colour key in the 3D views). */
    pn_node_set_name (src, "Eth5 Load");

    msg = pn_message_new (src, "alpha");
    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);

    views = pn_graph_collect_series_sorted (graph, &n);
    PN_CHECK_CMPINT (n, ==, 1u);
    PN_CHECK_CMPSTR (views[0].from, ==, "Eth5 Load");
    g_free (views);

    g_object_unref (src);
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

/* Chronological read-out of a series' raw-sample ring: the same walk the
 * distribution painter does, used here to check what a restore put back. */
static guint
series_values (const PnGraphSeries *s, gdouble *out, guint max)
{
    guint first = (s->sample_head + PN_GRAPH_SAMPLES - s->sample_count)
                  % PN_GRAPH_SAMPLES;
    guint n     = MIN (s->sample_count, max);
    guint i;

    for (i = 0; i < n; i++)
        out[i] = s->samples[(first + i) % PN_GRAPH_SAMPLES].value;
    return n;
}

/* Nothing is written into the worksheet unless the user asked for it:
 * "save-data" is off on a fresh node and the store stays empty however
 * much data the graph has collected. */
static void
test_save_data_off_by_default (void)
{
    PnNode   *node = PN_NODE (pn_graph_new ());
    gboolean  save = TRUE;
    gchar    *data = NULL;

    g_object_get (node, "save-data", &save, NULL);
    PN_CHECK_FALSE (save);

    feed (node, "t", 1.0);
    feed (node, "t", 2.0);

    g_object_get (node, "saved-data", &data, NULL);
    PN_CHECK_CMPSTR (data, ==, "");

    g_free (data);
    g_object_unref (node);
}

/* With "save-data" on, the samples survive a save/load round trip: a
 * fresh node handed the serialised store comes back with the same series
 * and the same values, in the same order, and with its time buckets
 * refolded (so the time-series view has something to draw). */
static void
test_saved_data_round_trip (void)
{
    PnGraph *src = pn_graph_new ();
    PnGraph *dst;
    gchar   *data = NULL;
    gdouble  vals[8];
    guint    n_views = 0;
    PnGraphSeriesView *views;

    g_object_set (src, "save-data", TRUE, NULL);
    feed (PN_NODE (src), "alpha", 1.5);
    feed (PN_NODE (src), "alpha", 2.5);
    feed (PN_NODE (src), "beta",  3.5);

    g_object_get (src, "saved-data", &data, NULL);
    PN_CHECK (data != NULL && *data != '\0');

    dst = pn_graph_new ();
    g_object_set (dst, "saved-data", data, NULL);

    PN_CHECK_CMPINT (pn_graph_get_series_count (dst), ==, 2u);

    views = pn_graph_collect_series_sorted (dst, &n_views);
    PN_CHECK_CMPINT (n_views, ==, 2u);
    PN_CHECK_CMPSTR (views[0].topic, ==, "alpha");
    PN_CHECK_CMPINT (series_values (views[0].series, vals, 8), ==, 2u);
    PN_CHECK_NEAR (vals[0], 1.5, 1e-9);
    PN_CHECK_NEAR (vals[1], 2.5, 1e-9);
    PN_CHECK_CMPSTR (views[1].topic, ==, "beta");
    PN_CHECK_CMPINT (series_values (views[1].series, vals, 8), ==, 1u);
    PN_CHECK_NEAR (vals[0], 3.5, 1e-9);

    /* The buckets are refolded on the spot, not only from the idle, so a
     * headless load has a populated ring too. */
    {
        const PnGraphSeries *s = views[0].series;
        guint  i, filled = 0;
        for (i = 0; i < PN_GRAPH_MAX_BINS; i++)
            if (s->ring[i].epoch != G_MININT64 && s->ring[i].count > 0)
                filled += s->ring[i].count;
        PN_CHECK_CMPINT (filled, ==, 2u);
    }

    g_free (views);
    g_free (data);
    g_object_unref (dst);
    g_object_unref (src);
}

/* The store is bounded: however long the graph has been running, only
 * the newest PN_GRAPH_PERSIST_SAMPLES per series are written out. */
static void
test_saved_data_is_capped (void)
{
    PnGraph *src = pn_graph_new ();
    PnGraph *dst;
    gchar   *data = NULL;
    guint    i;
    guint    n_views = 0;
    PnGraphSeriesView *views;

    g_object_set (src, "save-data", TRUE, NULL);
    for (i = 0; i < PN_GRAPH_PERSIST_SAMPLES + 100; i++)
        feed (PN_NODE (src), "t", (gdouble) i);

    g_object_get (src, "saved-data", &data, NULL);

    dst = pn_graph_new ();
    g_object_set (dst, "saved-data", data, NULL);

    views = pn_graph_collect_series_sorted (dst, &n_views);
    PN_CHECK_CMPINT (n_views, ==, 1u);
    PN_CHECK_CMPINT (views[0].series->sample_count, ==,
                     (guint) PN_GRAPH_PERSIST_SAMPLES);

    /* …and it is the NEWEST that survive: the last value fed is the last
     * value restored. */
    {
        guint   last = (views[0].series->sample_head + PN_GRAPH_SAMPLES - 1)
                       % PN_GRAPH_SAMPLES;
        gdouble v    = views[0].series->samples[last].value;

        PN_CHECK_NEAR (v, (gdouble) (PN_GRAPH_PERSIST_SAMPLES + 99), 1e-9);
    }

    g_free (views);
    g_free (data);
    g_object_unref (dst);
    g_object_unref (src);
}

/* Swallows the warning the decoder logs for a malformed store. */
static void
swallow_log (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        user_data)
{
    (void) domain; (void) level; (void) message; (void) user_data;
}

/* A hand-mangled or foreign store is ignored rather than fatal, and an
 * empty one leaves whatever the graph already holds alone. */
static void
test_saved_data_bad_input_is_safe (void)
{
    PnGraph  *node = pn_graph_new ();
    GLogFunc  prev;

    /* The decoder warns about the unparseable store; swallow it so the
     * expected-failure case does not spew to stderr.  (The harness does
     * not run under g_test_init(), so g_test_expect_message() is
     * unavailable here.) */
    prev = g_log_set_default_handler (swallow_log, NULL);
    g_object_set (node, "saved-data", "{ not json", NULL);
    g_log_set_default_handler (prev, NULL);
    PN_CHECK_CMPINT (pn_graph_get_series_count (node), ==, 0u);

    g_object_set (node, "saved-data", "{}", NULL);
    PN_CHECK_CMPINT (pn_graph_get_series_count (node), ==, 0u);

    /* An empty store never wipes live data. */
    feed (PN_NODE (node), "t", 1.0);
    g_object_set (node, "saved-data", "", NULL);
    PN_CHECK_CMPINT (pn_graph_get_series_count (node), ==, 1u);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-graph");
    pn_test_add ("is_a_sink",               test_is_a_sink);
    pn_test_add ("key_default",             test_key_default);
    pn_test_add ("series_per_topic",        test_series_per_topic);
    pn_test_add ("series_from_label",       test_series_from_label);
    pn_test_add ("empty_key_is_noop",       test_empty_key_is_noop);
    pn_test_add ("missing_value_is_noop",   test_missing_value_is_noop);
    pn_test_add ("series_fan_out_capped",   test_series_fan_out_is_capped);
    pn_test_add ("save_data_off_by_default", test_save_data_off_by_default);
    pn_test_add ("saved_data_round_trip",    test_saved_data_round_trip);
    pn_test_add ("saved_data_is_capped",     test_saved_data_is_capped);
    pn_test_add ("saved_data_bad_input",     test_saved_data_bad_input_is_safe);
    return pn_test_run ();
}
