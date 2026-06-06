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

/* Unit tests for PnOscilloscope.  The CRT only matters on screen, so the
 * headless tests cover the receive()/trace contract that the GTK-free
 * core owns: it is a pure sink, a scalar value is the single current
 * point (a dot — no history is kept) while a $pnvector value snapshots
 * the whole waveform (and the two modes never blend), the X value is
 * optional (sample index for a vector, 0 for a lone scalar), and the read
 * seam returns the trace in display order with exact bounds and a
 * bounded, extrema-preserving decimation for oversized waveforms.  These
 * pin the logic half the headless/core split (TODO #23) keeps loadable
 * without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-oscilloscope.h"
#include "pn-vector.h"

/* Feed one scalar (x, value) sample. */
static void
feed_scalar (PnNode *node, gdouble x, gdouble y)
{
    PnMessage *msg = pn_message_new (NULL, "t");
    pn_message_set_double (msg, "x", x);
    pn_message_set_double (msg, "value", y);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Feed a value vector with no X (plotted against the sample index). */
static void
feed_vec_y (PnNode *node, const gdouble *y, gsize n)
{
    PnMessage *msg = pn_message_new (NULL, "t");
    PnVector  *v   = pn_vector_new_copy (y, n);
    pn_message_set_vector (msg, "value", v);
    g_object_unref (v);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Feed both an X vector and a value vector. */
static void
feed_vec_xy (PnNode *node, const gdouble *x, const gdouble *y, gsize n)
{
    PnMessage *msg = pn_message_new (NULL, "t");
    PnVector  *vx  = pn_vector_new_copy (x, n);
    PnVector  *vy  = pn_vector_new_copy (y, n);
    pn_message_set_vector (msg, "x", vx);
    pn_message_set_vector (msg, "value", vy);
    g_object_unref (vx);
    g_object_unref (vy);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_is_a_sink (void)
{
    guint   emits = 0;
    PnNode *node  = PN_NODE (pn_oscilloscope_new ());

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed_scalar (node, 1.0, 10.0);
    {
        gdouble wave[4] = { 0.0, 1.0, 2.0, 3.0 };
        feed_vec_y (node, wave, 4);
    }

    PN_CHECK_CMPINT (emits, ==, 0);
    g_object_unref (node);
}

static void
test_key_defaults (void)
{
    PnNode *node = PN_NODE (pn_oscilloscope_new ());
    gchar  *vkey = NULL;
    gchar  *xkey = NULL;

    g_object_get (node, "value-key", &vkey, "x-key", &xkey, NULL);
    PN_CHECK_CMPSTR (vkey, ==, "value");
    PN_CHECK_CMPSTR (xkey, ==, "x");

    g_free (vkey);
    g_free (xkey);
    g_object_unref (node);
}

static void
test_scalar_single_point (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);

    /* No history: each scalar replaces the last, so the point count never
     * climbs above one. */
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 0u);
    feed_scalar (node, 1.0, 10.0);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 1u);
    feed_scalar (node, 2.0, 20.0);
    feed_scalar (node, 3.0, 30.0);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 1u);

    g_object_unref (node);
}

static void
test_scalar_trace_and_bounds (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         tx[16], ty[16];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;

    feed_scalar (node, 1.0, 10.0);
    feed_scalar (node, 2.0, 20.0);
    feed_scalar (node, 3.0, 30.0);

    n = pn_oscilloscope_read_trace (osc, 16, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);

    /* Only the latest point is drawn, but the bounds frame the WHOLE
     * explored extent so the dot has a stable frame to move within. */
    PN_CHECK_CMPINT (n, ==, 1u);
    PN_CHECK_NEAR (tx[0], 3.0,  1e-9);
    PN_CHECK_NEAR (ty[0], 30.0, 1e-9);
    PN_CHECK_NEAR (xmin, 1.0,  1e-9);
    PN_CHECK_NEAR (xmax, 3.0,  1e-9);
    PN_CHECK_NEAR (ymin, 10.0, 1e-9);
    PN_CHECK_NEAR (ymax, 30.0, 1e-9);

    g_object_unref (node);
}

static void
test_scalar_dot_moves_in_envelope (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         tx[8], ty[8];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;

    /* Non-monotonic motion: the drawn point is always the latest, while
     * the frame is the min/max envelope over all points (not just the
     * last, and not shrinking back). */
    feed_scalar (node, 1.0, 10.0);
    feed_scalar (node, 3.0, 30.0);
    feed_scalar (node, 2.0,  5.0);

    n = pn_oscilloscope_read_trace (osc, 8, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);

    PN_CHECK_CMPINT (n, ==, 1u);
    PN_CHECK_NEAR (tx[0], 2.0, 1e-9);   /* the latest dot */
    PN_CHECK_NEAR (ty[0], 5.0, 1e-9);
    PN_CHECK_NEAR (xmin, 1.0,  1e-9);   /* envelope over all three */
    PN_CHECK_NEAR (xmax, 3.0,  1e-9);
    PN_CHECK_NEAR (ymin, 5.0,  1e-9);
    PN_CHECK_NEAR (ymax, 30.0, 1e-9);

    g_object_unref (node);
}

static void
test_scalar_no_x_is_zero (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         tx[8], ty[8];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;

    /* No "x" member: every scalar's X is 0, so the X frame stays
     * degenerate (the dot rides the centre line) while Y tracks the
     * values.  Only the latest value is drawn. */
    {
        PnMessage *m;
        gint       i;
        for (i = 0; i < 3; i++)
        {
            m = pn_message_new (NULL, "t");
            pn_message_set_double (m, "value", (gdouble) (i * i));
            pn_node_receive_message (node, m);
            g_object_unref (m);
        }
    }

    n = pn_oscilloscope_read_trace (osc, 8, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);
    PN_CHECK_CMPINT (n, ==, 1u);
    PN_CHECK_NEAR (tx[0], 0.0, 1e-9);
    PN_CHECK_NEAR (ty[0], 4.0, 1e-9);   /* the last value: 2 * 2 */
    PN_CHECK_NEAR (xmin, 0.0, 1e-9);    /* X frame degenerate at 0 */
    PN_CHECK_NEAR (xmax, 0.0, 1e-9);
    PN_CHECK_NEAR (ymin, 0.0, 1e-9);    /* Y envelope 0..4 */
    PN_CHECK_NEAR (ymax, 4.0, 1e-9);

    g_object_unref (node);
}

static void
test_afterglow_trail (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         ax[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    gdouble         ay[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    gdouble         al[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    guint           m;

    /* The first point has nothing to trail away from. */
    feed_scalar (node, 1.0, 10.0);
    m = pn_oscilloscope_read_afterglow (osc, PN_OSCILLOSCOPE_AFTERGLOW_MAX,
                                        ax, ay, al);
    PN_CHECK_CMPINT (m, ==, 0u);

    /* Moving the dot leaves the vacated position as one fresh trail point,
     * oldest→newest, its life near full (only microseconds have passed). */
    feed_scalar (node, 2.0, 20.0);
    feed_scalar (node, 3.0, 30.0);
    m = pn_oscilloscope_read_afterglow (osc, PN_OSCILLOSCOPE_AFTERGLOW_MAX,
                                        ax, ay, al);
    PN_CHECK_CMPINT (m, ==, 2u);
    PN_CHECK_NEAR (ax[0], 1.0,  1e-9);   /* oldest vacated position */
    PN_CHECK_NEAR (ay[0], 10.0, 1e-9);
    PN_CHECK_NEAR (ax[1], 2.0,  1e-9);   /* most recently vacated */
    PN_CHECK_NEAR (ay[1], 20.0, 1e-9);
    PN_CHECK (al[0] > 0.0 && al[0] <= 1.0);
    PN_CHECK (al[1] > 0.0 && al[1] <= 1.0);

    /* A repeated value (no motion) adds no trail point. */
    feed_scalar (node, 3.0, 30.0);
    m = pn_oscilloscope_read_afterglow (osc, PN_OSCILLOSCOPE_AFTERGLOW_MAX,
                                        ax, ay, al);
    PN_CHECK_CMPINT (m, ==, 2u);

    /* A vector snapshot keeps no trail — it clears the afterglow. */
    {
        gdouble wave[3] = { 1.0, 2.0, 3.0 };
        feed_vec_y (node, wave, 3);
    }
    m = pn_oscilloscope_read_afterglow (osc, PN_OSCILLOSCOPE_AFTERGLOW_MAX,
                                        ax, ay, al);
    PN_CHECK_CMPINT (m, ==, 0u);

    g_object_unref (node);
}

static void
test_vector_snapshot_supersedes_point (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         wave[4] = { 5.0, 7.0, 3.0, 9.0 };
    gdouble         tx[8], ty[8];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;

    /* Two scalars (only the last is kept), then a vector snapshot
     * supersedes the point entirely. */
    feed_scalar (node, 0.0, 1.0);
    feed_scalar (node, 1.0, 2.0);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 1u);

    feed_vec_y (node, wave, 4);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 0u);

    n = pn_oscilloscope_read_trace (osc, 8, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);

    /* The whole vector is the trace; X is the sample index 0..3. */
    PN_CHECK_CMPINT (n, ==, 4u);
    PN_CHECK_NEAR (tx[0], 0.0, 1e-9);
    PN_CHECK_NEAR (ty[0], 5.0, 1e-9);
    PN_CHECK_NEAR (tx[3], 3.0, 1e-9);
    PN_CHECK_NEAR (ty[3], 9.0, 1e-9);
    PN_CHECK_NEAR (xmin, 0.0, 1e-9);
    PN_CHECK_NEAR (xmax, 3.0, 1e-9);
    PN_CHECK_NEAR (ymin, 3.0, 1e-9);
    PN_CHECK_NEAR (ymax, 9.0, 1e-9);

    /* A scalar after a snapshot drops the snapshot, back to one point. */
    feed_scalar (node, 0.0, 0.0);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 1u);

    g_object_unref (node);
}

static void
test_vector_xy_pairs (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    gdouble         xs[3] = { 10.0, 20.0, 30.0 };
    gdouble         ys[3] = { -1.0,  0.0,  1.0 };
    gdouble         tx[8], ty[8];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;

    feed_vec_xy (node, xs, ys, 3);

    n = pn_oscilloscope_read_trace (osc, 8, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);
    PN_CHECK_CMPINT (n, ==, 3u);
    PN_CHECK_NEAR (tx[0], 10.0, 1e-9);
    PN_CHECK_NEAR (tx[2], 30.0, 1e-9);
    PN_CHECK_NEAR (ty[0], -1.0, 1e-9);
    PN_CHECK_NEAR (ty[2],  1.0, 1e-9);
    PN_CHECK_NEAR (xmin, 10.0, 1e-9);
    PN_CHECK_NEAR (xmax, 30.0, 1e-9);
    PN_CHECK_NEAR (ymin, -1.0, 1e-9);
    PN_CHECK_NEAR (ymax,  1.0, 1e-9);

    g_object_unref (node);
}

static void
test_decimation_bounded_and_exact_bounds (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    const gsize     N    = 1000;
    gdouble        *wave = g_new (gdouble, N);
    gdouble         tx[64], ty[64];
    gdouble         xmin, xmax, ymin, ymax;
    guint           n;
    gsize           i;

    for (i = 0; i < N; i++)
        wave[i] = (gdouble) i;          /* a monotonic ramp 0..999 */

    feed_vec_y (node, wave, N);

    n = pn_oscilloscope_read_trace (osc, 64, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);

    /* Never exceeds the cap, and the bounds reflect the FULL waveform
     * even though only a decimated subset is returned for drawing. */
    PN_CHECK (n > 0u && n <= 64u);
    PN_CHECK_NEAR (xmin, 0.0,   1e-9);
    PN_CHECK_NEAR (xmax, 999.0, 1e-9);
    PN_CHECK_NEAR (ymin, 0.0,   1e-9);
    PN_CHECK_NEAR (ymax, 999.0, 1e-9);

    g_free (wave);
    g_object_unref (node);
}

static void
test_missing_or_bad_value_is_noop (void)
{
    PnOscilloscope *osc  = pn_oscilloscope_new ();
    PnNode         *node = PN_NODE (osc);
    PnMessage      *m;

    /* Only X, no value → nothing to trace. */
    m = pn_message_new (NULL, "t");
    pn_message_set_double (m, "x", 5.0);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 0u);

    /* A non-numeric value string is dropped. */
    m = pn_message_new (NULL, "t");
    pn_message_set_string (m, "value", "not-a-number");
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 0u);

    /* Clearing the value key disables the node. */
    g_object_set (osc, "value-key", "", NULL);
    feed_scalar (node, 1.0, 1.0);
    PN_CHECK_CMPINT (pn_oscilloscope_get_point_count (osc), ==, 0u);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-oscilloscope");
    pn_test_add ("is_a_sink",                test_is_a_sink);
    pn_test_add ("key_defaults",             test_key_defaults);
    pn_test_add ("scalar_single_point",      test_scalar_single_point);
    pn_test_add ("scalar_trace_and_bounds",  test_scalar_trace_and_bounds);
    pn_test_add ("scalar_dot_moves",         test_scalar_dot_moves_in_envelope);
    pn_test_add ("scalar_no_x_is_zero",      test_scalar_no_x_is_zero);
    pn_test_add ("afterglow_trail",          test_afterglow_trail);
    pn_test_add ("vector_snapshot",          test_vector_snapshot_supersedes_point);
    pn_test_add ("vector_xy_pairs",          test_vector_xy_pairs);
    pn_test_add ("decimation_bounded",       test_decimation_bounded_and_exact_bounds);
    pn_test_add ("missing_value_noop",       test_missing_or_bad_value_is_noop);
    return pn_test_run ();
}
