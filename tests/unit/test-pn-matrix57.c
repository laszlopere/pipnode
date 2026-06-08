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

/* Unit tests for PnMatrix57.  The 5x7 dot-matrix glyphs are GTK/cairo and
 * only matter on screen, so these headless tests cover the logic contract:
 * it is a pure sink (data.output never forwards), its cell/line/colour
 * properties carry sane defaults and round-trip, the visible-line count is
 * clamped to [1, 2], and the incoming string is normalised (backslash
 * escapes resolved, '\r' stripped) and tailed to the last @lines rows into
 * the GTK-free read seam (pn_matrix57_get_paint_state). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-matrix57.h"

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
shown (PnMatrix57 *m)
{
    static PnMatrix57PaintState st;
    pn_matrix57_get_paint_state (m, &st);
    return st.text;
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint   emits;
    PnNode *node = PN_NODE (pn_matrix57_new ());

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, "HELLO");
    feed (node, "WORLD");

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* The cell count and line count carry their documented defaults / bounds. */
static void
test_property_defaults (void)
{
    PnNode         *node = PN_NODE (pn_matrix57_new ());
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;
    GParamSpecInt  *ispec;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), "cells");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum,       ==, 1u);
    PN_CHECK_CMPINT (uspec->maximum,       ==, 40u);
    PN_CHECK_CMPINT (uspec->default_value, ==, 16u);

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), "lines");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_INT (pspec));
    ispec = G_PARAM_SPEC_INT (pspec);
    PN_CHECK_CMPINT (ispec->minimum,       ==, 1);
    PN_CHECK_CMPINT (ispec->maximum,       ==, 2);
    PN_CHECK_CMPINT (ispec->default_value, ==, 1);

    g_object_unref (node);
}

/* Cells round-trip through the property and surface in the snapshot. */
static void
test_cells_round_trip (void)
{
    PnMatrix57 *m     = pn_matrix57_new ();
    guint       cells = 0;
    PnMatrix57PaintState st;

    g_object_set (m, "cells", 24u, NULL);
    g_object_get (m, "cells", &cells, NULL);
    PN_CHECK_CMPINT (cells, ==, 24u);

    pn_matrix57_get_paint_state (m, &st);
    PN_CHECK_CMPINT (st.cells, ==, 24u);

    g_object_unref (m);
}

/* The two valid line counts round-trip through the property and surface in
 * the snapshot.  (The paramspec is bounded to [1, 2], so GObject itself
 * rejects an out-of-range value before the setter's defensive clamp runs.) */
static void
test_lines_round_trip (void)
{
    PnMatrix57          *m     = pn_matrix57_new ();
    gint                 lines = 0;
    PnMatrix57PaintState st;

    g_object_set (m, "lines", 2, NULL);
    g_object_get (m, "lines", &lines, NULL);
    PN_CHECK_CMPINT (lines, ==, 2);
    pn_matrix57_get_paint_state (m, &st);
    PN_CHECK_CMPINT (st.lines, ==, 2);

    g_object_set (m, "lines", 1, NULL);
    g_object_get (m, "lines", &lines, NULL);
    PN_CHECK_CMPINT (lines, ==, 1);

    g_object_unref (m);
}

/* The four colour properties round-trip through the GObject surface. */
static void
test_colors_round_trip (void)
{
    PnMatrix57 *m         = pn_matrix57_new ();
    PnColor     frame_in  = { 0.10, 0.20, 0.30, 1.0 };
    PnColor     bg_in     = { 0.40, 0.50, 0.60, 1.0 };
    PnColor     px_in     = { 0.01, 0.02, 0.03, 1.0 };
    PnColor     ghost_in  = { 0.70, 0.80, 0.90, 1.0 };
    PnColor    *frame_out = NULL, *bg_out = NULL, *px_out = NULL, *ghost_out = NULL;

    g_object_set (m,
                  "frame-color",       &frame_in,
                  "background-color",  &bg_in,
                  "pixel-color",       &px_in,
                  "unlit-pixel-color", &ghost_in,
                  NULL);
    g_object_get (m,
                  "frame-color",       &frame_out,
                  "background-color",  &bg_out,
                  "pixel-color",       &px_out,
                  "unlit-pixel-color", &ghost_out,
                  NULL);

    PN_CHECK (frame_out != NULL && pn_color_equal (frame_out, &frame_in));
    PN_CHECK (bg_out    != NULL && pn_color_equal (bg_out,    &bg_in));
    PN_CHECK (px_out    != NULL && pn_color_equal (px_out,    &px_in));
    PN_CHECK (ghost_out != NULL && pn_color_equal (ghost_out, &ghost_in));

    pn_color_free (frame_out);
    pn_color_free (bg_out);
    pn_color_free (px_out);
    pn_color_free (ghost_out);
    g_object_unref (m);
}

/* Before any message the readout is blank ("" never NULL). */
static void
test_initial_blank (void)
{
    PnMatrix57 *m = pn_matrix57_new ();
    PN_CHECK_CMPSTR (shown (m), ==, "");
    g_object_unref (m);
}

/* A single-line string latches verbatim into the snapshot. */
static void
test_latches_text (void)
{
    PnMatrix57 *m = pn_matrix57_new ();

    feed (PN_NODE (m), "HELLO");
    PN_CHECK_CMPSTR (shown (m), ==, "HELLO");

    /* The public seam matches what receive() does. */
    pn_matrix57_set_text (m, "PIPNODE");
    PN_CHECK_CMPSTR (shown (m), ==, "PIPNODE");

    g_object_unref (m);
}

/* NULL, "" and a non-string output all read as the blank row. */
static void
test_blank_and_non_string (void)
{
    PnMatrix57 *m = pn_matrix57_new ();
    PnMessage  *num;

    feed (PN_NODE (m), "TEXT");
    PN_CHECK_CMPSTR (shown (m), ==, "TEXT");

    feed (PN_NODE (m), "");              /* empty string blanks */
    PN_CHECK_CMPSTR (shown (m), ==, "");

    pn_matrix57_set_text (m, "AGAIN");
    PN_CHECK_CMPSTR (shown (m), ==, "AGAIN");

    /* A numeric data.output is not a string -> blanks the readout. */
    num = pn_message_new (NULL, NULL);
    pn_message_set_double (num, "output", 42.0);
    pn_node_receive_message (PN_NODE (m), num);
    g_object_unref (num);
    PN_CHECK_CMPSTR (shown (m), ==, "");

    g_object_unref (m);
}

/* With one visible line the latest newline-separated row wins; with two,
 * both survive.  A literal backslash-n and a real newline behave the same,
 * and a carriage return is dropped. */
static void
test_tail_and_normalise (void)
{
    PnMatrix57 *m = pn_matrix57_new ();

    /* Default lines == 1: the trailing row is what shows. */
    feed (PN_NODE (m), "ab\ncd");
    PN_CHECK_CMPSTR (shown (m), ==, "cd");

    /* Two lines: both rows survive, joined by a real newline. */
    g_object_set (m, "lines", 2, NULL);
    feed (PN_NODE (m), "ab\ncd");
    PN_CHECK_CMPSTR (shown (m), ==, "ab\ncd");

    /* A literal "\n" escape resolves to a newline -> same tailing. */
    g_object_set (m, "lines", 1, NULL);
    feed (PN_NODE (m), "ab\\ncd");
    PN_CHECK_CMPSTR (shown (m), ==, "cd");

    /* CRLF: the '\r' is stripped, the '\n' splits the row. */
    feed (PN_NODE (m), "ab\r\ncd");
    PN_CHECK_CMPSTR (shown (m), ==, "cd");

    /* A trailing newline does not eat the visible row. */
    feed (PN_NODE (m), "only\n");
    PN_CHECK_CMPSTR (shown (m), ==, "only");

    g_object_unref (m);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-matrix57");
    pn_test_add ("is_a_sink",            test_is_a_sink);
    pn_test_add ("property_defaults",    test_property_defaults);
    pn_test_add ("cells_round_trip",     test_cells_round_trip);
    pn_test_add ("lines_round_trip",     test_lines_round_trip);
    pn_test_add ("colors_round_trip",    test_colors_round_trip);
    pn_test_add ("initial_blank",        test_initial_blank);
    pn_test_add ("latches_text",         test_latches_text);
    pn_test_add ("blank_and_non_string", test_blank_and_non_string);
    pn_test_add ("tail_and_normalise",   test_tail_and_normalise);
    return pn_test_run ();
}
