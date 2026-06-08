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

/* Unit tests for PnSegment16.  The 16-segment starburst glyphs are
 * GTK/cairo and only matter on screen, so these headless tests cover the
 * logic contract: it is a pure sink (data.output never forwards), its
 * cell-count and colour properties carry sane defaults and round-trip, and
 * the incoming string latches verbatim into the GTK-free read seam
 * (pn_segment16_get_paint_state), with NULL / "" / a non-string output all
 * reading as the blank row. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-segment16.h"

/* Feed one data.output string through the node's input. */
static void
feed (PnNode *node, const gchar *output)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    if (output != NULL)
        pn_message_set_string (m, "output", output);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* Borrow the current display text from the GTK-free paint-state seam. */
static const gchar *
shown (PnSegment16 *s)
{
    static PnSegment16PaintState st;
    pn_segment16_get_paint_state (s, &st);
    return st.text;
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint   emits;
    PnNode *node = PN_NODE (pn_segment16_new ());

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, "12:00");
    feed (node, "ABCDE");

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* The cell count carries its documented default and bounds. */
static void
test_cells_default (void)
{
    PnNode         *node = PN_NODE (pn_segment16_new ());
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), "cells");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum,       ==, 1u);
    PN_CHECK_CMPINT (uspec->maximum,       ==, 32u);
    PN_CHECK_CMPINT (uspec->default_value, ==, 8u);

    g_object_unref (node);
}

/* Cells round-trip through the property and surface in the snapshot. */
static void
test_cells_round_trip (void)
{
    PnSegment16          *s     = pn_segment16_new ();
    guint                 cells = 0;
    PnSegment16PaintState st;

    g_object_set (s, "cells", 12u, NULL);
    g_object_get (s, "cells", &cells, NULL);
    PN_CHECK_CMPINT (cells, ==, 12u);

    pn_segment16_get_paint_state (s, &st);
    PN_CHECK_CMPINT (st.cells, ==, 12u);

    g_object_unref (s);
}

/* The three colour properties round-trip through the GObject surface and
 * surface in the snapshot. */
static void
test_colors_round_trip (void)
{
    PnSegment16          *s        = pn_segment16_new ();
    PnColor               bg_in    = { 0.10, 0.20, 0.30, 1.0 };
    PnColor               seg_in   = { 0.90, 0.10, 0.05, 1.0 };
    PnColor               ghost_in = { 0.20, 0.02, 0.01, 1.0 };
    PnColor              *bg_out = NULL, *seg_out = NULL, *ghost_out = NULL;
    PnSegment16PaintState st;

    g_object_set (s,
                  "background-color",    &bg_in,
                  "segment-color",       &seg_in,
                  "unlit-segment-color", &ghost_in,
                  NULL);
    g_object_get (s,
                  "background-color",    &bg_out,
                  "segment-color",       &seg_out,
                  "unlit-segment-color", &ghost_out,
                  NULL);

    PN_CHECK (bg_out    != NULL && pn_color_equal (bg_out,    &bg_in));
    PN_CHECK (seg_out   != NULL && pn_color_equal (seg_out,   &seg_in));
    PN_CHECK (ghost_out != NULL && pn_color_equal (ghost_out, &ghost_in));

    pn_segment16_get_paint_state (s, &st);
    PN_CHECK (pn_color_equal (&st.background_color,    &bg_in));
    PN_CHECK (pn_color_equal (&st.segment_color,       &seg_in));
    PN_CHECK (pn_color_equal (&st.unlit_segment_color, &ghost_in));

    pn_color_free (bg_out);
    pn_color_free (seg_out);
    pn_color_free (ghost_out);
    g_object_unref (s);
}

/* Before any message the readout is blank ("" never NULL). */
static void
test_initial_blank (void)
{
    PnSegment16 *s = pn_segment16_new ();
    PN_CHECK_CMPSTR (shown (s), ==, "");
    g_object_unref (s);
}

/* A string latches verbatim into the snapshot, via receive() and via the
 * public set_text seam alike. */
static void
test_latches_text (void)
{
    PnSegment16 *s = pn_segment16_new ();

    feed (PN_NODE (s), "HELLO");
    PN_CHECK_CMPSTR (shown (s), ==, "HELLO");

    pn_segment16_set_text (s, "42.0V");
    PN_CHECK_CMPSTR (shown (s), ==, "42.0V");

    g_object_unref (s);
}

/* NULL, "" and a non-string output all read as the blank row. */
static void
test_blank_and_non_string (void)
{
    PnSegment16 *s = pn_segment16_new ();
    PnMessage   *num;

    feed (PN_NODE (s), "ON");
    PN_CHECK_CMPSTR (shown (s), ==, "ON");

    feed (PN_NODE (s), "");              /* empty string blanks */
    PN_CHECK_CMPSTR (shown (s), ==, "");

    pn_segment16_set_text (s, "BACK");
    PN_CHECK_CMPSTR (shown (s), ==, "BACK");

    /* A numeric data.output is not a string -> blanks the readout. */
    num = pn_message_new (NULL, NULL);
    pn_message_set_double (num, "output", 7.0);
    pn_node_receive_message (PN_NODE (s), num);
    g_object_unref (num);
    PN_CHECK_CMPSTR (shown (s), ==, "");

    g_object_unref (s);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-segment16");
    pn_test_add ("is_a_sink",            test_is_a_sink);
    pn_test_add ("cells_default",        test_cells_default);
    pn_test_add ("cells_round_trip",     test_cells_round_trip);
    pn_test_add ("colors_round_trip",    test_colors_round_trip);
    pn_test_add ("initial_blank",        test_initial_blank);
    pn_test_add ("latches_text",         test_latches_text);
    pn_test_add ("blank_and_non_string", test_blank_and_non_string);
    return pn_test_run ();
}
