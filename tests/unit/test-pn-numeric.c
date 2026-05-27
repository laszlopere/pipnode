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

/* Unit tests for PnNumeric.  A pure sink: it reads data.value and
 * stores it for the GTK-free paint state seam to expose.  The cell-level
 * rounding lives in the painter, so these tests assert on the snapshot
 * the painter would read from. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-numeric.h"

static void
feed_double (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint   emits = 0;
    PnNode *node  = PN_NODE (pn_numeric_new ());

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed_double (node, 12.345);
    feed_double (node, -987.6);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* Before any message the display reads as a blank (has_value false), and
 * the stored value is zero. */
static void
test_initial_blank (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;

    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK (st.has_value == FALSE);
    PN_CHECK         (st.value == 0.0);

    g_object_unref (nm);
}

/* The first numeric message latches has_value and stores the value. */
static void
test_latches_value (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;

    feed_double (PN_NODE (nm), 42.5);
    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK (st.has_value == TRUE);
    PN_CHECK         (st.value == 42.5);

    g_object_unref (nm);
}

/* Negative readings survive into the snapshot as-is — the sign cell is
 * the painter's responsibility, the logic just stores the value. */
static void
test_negative_value (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;

    feed_double (PN_NODE (nm), -3.14);
    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK (st.has_value == TRUE);
    PN_CHECK         (st.value == -3.14);

    g_object_unref (nm);
}

/* An int64 value decodes the same as a double; a non-numeric value is
 * ignored and leaves the previous reading intact. */
static void
test_value_types (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;
    PnMessage           *m;

    m = pn_message_new (NULL, NULL);
    pn_message_set_int64 (m, "value", -7);
    pn_node_receive_message (PN_NODE (nm), m);
    g_object_unref (m);

    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK         (st.value == -7.0);

    /* A string value is ignored — the previous reading survives. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "soon");
    pn_node_receive_message (PN_NODE (nm), m);
    g_object_unref (m);

    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK         (st.value == -7.0);

    g_object_unref (nm);
}

/* Non-finite values are ignored. */
static void
test_non_finite_ignored (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;

    feed_double (PN_NODE (nm), 12.0);
    feed_double (PN_NODE (nm), INFINITY);
    feed_double (PN_NODE (nm), NAN);

    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK         (st.value == 12.0);

    g_object_unref (nm);
}

/* The layout/colour properties round-trip and surface in the snapshot. */
static void
test_properties_round_trip (void)
{
    PnNumeric           *nm = pn_numeric_new ();
    PnNumericPaintState  st;
    guint                digits = 0;
    guint                dp     = 99;
    PnColor              lit_in = { 0.10, 0.80, 0.30, 1.0 };
    PnColor             *lit_out = NULL;

    g_object_set (nm,
                  "digits",         8u,
                  "decimal-places", 3u,
                  "segment-color",  &lit_in,
                  NULL);
    g_object_get (nm,
                  "digits",         &digits,
                  "decimal-places", &dp,
                  "segment-color",  &lit_out,
                  NULL);

    PN_CHECK_CMPINT (digits, ==, 8);
    PN_CHECK_CMPINT (dp,     ==, 3);
    PN_CHECK        (pn_color_equal (lit_out, &lit_in));

    pn_numeric_get_paint_state (nm, &st);
    PN_CHECK_CMPINT (st.digits,         ==, 8);
    PN_CHECK_CMPINT (st.decimal_places, ==, 3);
    PN_CHECK        (pn_color_equal (&st.segment_color, &lit_in));

    pn_color_free (lit_out);
    g_object_unref (nm);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-numeric");
    pn_test_add ("is_a_sink",            test_is_a_sink);
    pn_test_add ("initial_blank",        test_initial_blank);
    pn_test_add ("latches_value",        test_latches_value);
    pn_test_add ("negative_value",       test_negative_value);
    pn_test_add ("value_types",          test_value_types);
    pn_test_add ("non_finite_ignored",   test_non_finite_ignored);
    pn_test_add ("properties_round_trip", test_properties_round_trip);
    return pn_test_run ();
}
