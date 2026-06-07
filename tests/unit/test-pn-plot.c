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

/* Unit tests for PnPlot.  The PLplot rendering only matters on screen, so
 * the headless tests cover the GTK-free core contract: PnPlot is a pure
 * sink that takes a whole $pnvector on data.value in one message (the
 * snapshot path) and distributes its N elements across M = "x-buckets"
 * consecutive buckets, aggregating each run into the same per-bucket
 * running totals (count / mean / sd / min / max) PnGraph keeps per time
 * bin.  A bare scalar carries no spread to bucket and is ignored; a held
 * vector is re-bucketed live when the bucket count changes; and the raw
 * elements are mirrored into the sample ring for the distribution view.
 *
 * The box-and-whisker GLYPH geometry — including the neighbour-reaching
 * whisker stretch for single-sample buckets — is drawn by the gui tier
 * (pn_graph_collect_error_bars in pn-graph-gui.c, PLplot-typed and not
 * linkable here), so these tests pin the core aggregation it consumes,
 * and in particular the single-sample-bucket case (min == max, sd 0) that
 * is precisely the dash-collapse condition that whisker stretch exists to
 * cure.  This keeps the logic half the headless/core split (TODO #23)
 * loadable without GTK and the vector→bin mapping (TODO #48) characterised. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-plot.h"
#include "pn-vector.h"

#include <math.h>

/* Feed a value vector on data.value (the whole dataset for one message). */
static void
feed_vec (PnNode *node, const gdouble *y, gsize n)
{
    PnMessage *msg = pn_message_new (NULL, "t");
    PnVector  *v   = pn_vector_new_copy (y, n);
    pn_message_set_vector (msg, "value", v);
    g_object_unref (v);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Population mean / sd of a bucket, computed the way the gui error-bar
 * collector does (var = sum_sq/count - mean^2, clamped at 0). */
static gdouble
bin_mean (const PnGraphBin *b)
{
    return b->sum / (gdouble) b->count;
}

static gdouble
bin_sd (const PnGraphBin *b)
{
    gdouble m   = bin_mean (b);
    gdouble var = b->sum_sq / (gdouble) b->count - m * m;
    if (var < 0.0) var = 0.0;
    return sqrt (var);
}

static void
test_is_a_sink (void)
{
    guint   emits = 0;
    PnNode *node  = PN_NODE (pn_plot_new ());
    gdouble wave[4] = { 0.0, 1.0, 2.0, 3.0 };

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed_vec (node, wave, 4);

    PN_CHECK_CMPINT (emits, ==, 0);
    g_object_unref (node);
}

static void
test_defaults (void)
{
    PnPlot *plot = pn_plot_new ();
    gchar  *vkey = NULL;
    guint   bins = 0;

    g_object_get (plot, "value-key", &vkey, "x-buckets", &bins, NULL);
    PN_CHECK_CMPSTR (vkey, ==, "value");
    PN_CHECK_CMPINT (bins, ==, 16);                 /* PN_PLOT_DEF_BINS */
    PN_CHECK_CMPINT (pn_plot_get_n_bins (plot), ==, 16u);

    /* No vector yet → nothing to draw. */
    PN_CHECK (pn_plot_peek_series (plot) == NULL);

    g_free (vkey);
    g_object_unref (plot);
}

static void
test_scalar_is_ignored (void)
{
    PnPlot    *plot = pn_plot_new ();
    PnNode    *node = PN_NODE (plot);
    PnMessage *m;

    /* A bare scalar carries no spread to bucket — use a PnGraph for scalar
     * time-series — so it leaves the plot empty. */
    m = pn_message_new (NULL, "t");
    pn_message_set_double (m, "value", 42.0);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK (pn_plot_peek_series (plot) == NULL);

    /* A message with nothing under the value key is likewise a no-op. */
    m = pn_message_new (NULL, "t");
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK (pn_plot_peek_series (plot) == NULL);

    g_object_unref (node);
}

static void
test_empty_key_is_noop (void)
{
    PnPlot  *plot = pn_plot_new ();
    PnNode  *node = PN_NODE (plot);
    gdouble  wave[4] = { 1.0, 2.0, 3.0, 4.0 };

    /* Clearing the value key disables the node: no member is read, so no
     * vector is ever bucketed. */
    g_object_set (plot, "value-key", "", NULL);
    feed_vec (node, wave, 4);
    PN_CHECK (pn_plot_peek_series (plot) == NULL);

    g_object_unref (node);
}

static void
test_bucket_aggregation (void)
{
    PnPlot              *plot = pn_plot_new ();
    PnNode              *node = PN_NODE (plot);
    const PnGraphSeries *s;
    gdouble              vec[12];
    guint                b;

    for (b = 0; b < 12; b++)
        vec[b] = (gdouble) b;           /* 0,1,...,11 */

    /* Four buckets over twelve elements: each owns a run of three
     * consecutive integers [3b, 3b+3).  Bucket b is stored in slot b with
     * epoch b (the bucket index doubles as the bin epoch). */
    g_object_set (plot, "x-buckets", 4u, NULL);
    feed_vec (node, vec, 12);

    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);
    PN_CHECK_CMPINT (pn_plot_get_n_bins (plot), ==, 4u);

    for (b = 0; b < 4; b++)
    {
        const PnGraphBin *bin = &s->ring[b];

        PN_CHECK (bin->epoch == (gint64) b);
        PN_CHECK_CMPINT ((gint) bin->count, ==, 3);
        /* means 1, 4, 7, 10 */
        PN_CHECK_NEAR (bin_mean (bin), 1.0 + 3.0 * (gdouble) b, 1e-9);
        /* exact min / max of each three-integer run */
        PN_CHECK_NEAR (bin->min, 3.0 * (gdouble) b,       1e-9);
        PN_CHECK_NEAR (bin->max, 3.0 * (gdouble) b + 2.0, 1e-9);
        /* sd of {k, k+1, k+2} is sqrt(2/3), identical for every bucket */
        PN_CHECK_NEAR (bin_sd (bin), sqrt (2.0 / 3.0), 1e-9);
    }

    g_object_unref (node);
}

static void
test_single_sample_buckets (void)
{
    PnPlot              *plot = pn_plot_new ();
    PnNode              *node = PN_NODE (plot);
    const PnGraphSeries *s;
    gdouble              vec[12];
    guint                b;

    for (b = 0; b < 12; b++)
        vec[b] = (gdouble) b;

    /* Bucket count equal to the vector length collapses to one element per
     * bucket: count 1, min == max == the value, sd 0.  This is exactly the
     * dash-collapse case the gui's neighbour-reaching whisker stretch
     * exists to cure — pinned here at the core so the trigger condition is
     * characterised even though the stretch itself is PLplot-tier. */
    g_object_set (plot, "x-buckets", 12u, NULL);
    feed_vec (node, vec, 12);

    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);
    PN_CHECK_CMPINT (pn_plot_get_n_bins (plot), ==, 12u);

    for (b = 0; b < 12; b++)
    {
        const PnGraphBin *bin = &s->ring[b];

        PN_CHECK (bin->epoch == (gint64) b);
        PN_CHECK_CMPINT ((gint) bin->count, ==, 1);
        PN_CHECK_NEAR (bin->min, (gdouble) b, 1e-9);
        PN_CHECK_NEAR (bin->max, (gdouble) b, 1e-9);   /* min == max */
        PN_CHECK_NEAR (bin_mean (bin), (gdouble) b, 1e-9);
        PN_CHECK_NEAR (bin_sd (bin),   0.0,         1e-12);
    }

    g_object_unref (node);
}

static void
test_rebucket_on_count_change (void)
{
    PnPlot              *plot = pn_plot_new ();
    PnNode              *node = PN_NODE (plot);
    const PnGraphSeries *s;
    gdouble              vec[12];
    guint                b;

    for (b = 0; b < 12; b++)
        vec[b] = (gdouble) b;

    /* Feed once at four buckets (three samples each)... */
    g_object_set (plot, "x-buckets", 4u, NULL);
    feed_vec (node, vec, 12);
    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);
    PN_CHECK_CMPINT ((gint) s->ring[0].count, ==, 3);

    /* ...then change the bucket count: the HELD vector is re-distributed in
     * place, with no fresh message, into twelve single-sample buckets. */
    g_object_set (plot, "x-buckets", 12u, NULL);
    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);
    PN_CHECK_CMPINT (pn_plot_get_n_bins (plot), ==, 12u);
    for (b = 0; b < 12; b++)
    {
        PN_CHECK_CMPINT ((gint) s->ring[b].count, ==, 1);
        PN_CHECK_NEAR (bin_mean (&s->ring[b]), (gdouble) b, 1e-9);
    }

    g_object_unref (node);
}

static void
test_more_buckets_than_samples (void)
{
    PnPlot              *plot = pn_plot_new ();
    PnNode              *node = PN_NODE (plot);
    const PnGraphSeries *s;
    gdouble              vec[3] = { 10.0, 20.0, 30.0 };
    guint                used  = 0;
    guint64              total = 0;
    guint                b;

    /* More buckets than elements: every element still lands in exactly one
     * bucket, the rest stay unused (epoch G_MININT64) so the painter skips
     * them just like a never-touched PnGraph time slot. */
    g_object_set (plot, "x-buckets", 8u, NULL);
    feed_vec (node, vec, 3);

    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);

    for (b = 0; b < 8; b++)
    {
        const PnGraphBin *bin = &s->ring[b];
        if (bin->epoch == G_MININT64)
            continue;                   /* unused slot */
        used  += 1;
        total += bin->count;
        PN_CHECK_CMPINT ((gint) bin->count, ==, 1);   /* one element each */
    }

    PN_CHECK_CMPINT (used, ==, 3u);     /* exactly the three elements */
    PN_CHECK_CMPINT ((gint) total, ==, 3);
    /* Bucket 0 owns the empty range [0, 0) and stays unused. */
    PN_CHECK (s->ring[0].epoch == G_MININT64);

    g_object_unref (node);
}

static void
test_distribution_sample_mirror (void)
{
    PnPlot              *plot = pn_plot_new ();
    PnNode              *node = PN_NODE (plot);
    const PnGraphSeries *s;
    gdouble              vec[5] = { 4.0, 1.0, 3.0, 1.0, 5.0 };
    guint                i;

    /* The raw elements are mirrored verbatim into the sample ring so the
     * distribution view bins the WHOLE vector by value.  Each is stamped
     * with the sentinel "always in window" time so none ages out. */
    feed_vec (node, vec, 5);

    s = pn_plot_peek_series (plot);
    PN_CHECK (s != NULL);
    PN_CHECK_CMPINT (s->sample_count, ==, 5u);
    for (i = 0; i < 5; i++)
    {
        PN_CHECK_NEAR (s->samples[i].value, vec[i], 1e-9);
        PN_CHECK (s->samples[i].time_us == G_MAXINT64);
    }

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-plot");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("defaults",               test_defaults);
    pn_test_add ("scalar_is_ignored",      test_scalar_is_ignored);
    pn_test_add ("empty_key_is_noop",      test_empty_key_is_noop);
    pn_test_add ("bucket_aggregation",     test_bucket_aggregation);
    pn_test_add ("single_sample_buckets",  test_single_sample_buckets);
    pn_test_add ("rebucket_on_change",     test_rebucket_on_count_change);
    pn_test_add ("more_buckets_than_n",    test_more_buckets_than_samples);
    pn_test_add ("distribution_mirror",    test_distribution_sample_mirror);
    return pn_test_run ();
}
