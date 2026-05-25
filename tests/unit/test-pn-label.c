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

/* Unit tests for PnLabel.  The node is a pure sink that reads data.output
 * as a string and shows its last one or two lines.  The tail computation
 * is GTK-free (pn_label_get_paint_state), so these tests assert on it
 * directly without ever opening a display; the node never forwards a
 * message. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-label.h"

/* Feed one data.output string through the node's input. */
static void
feed (PnNode *node, const gchar *output)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "output", output);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* The text the label currently shows. */
static const gchar *
shown (PnLabel *label, PnLabelPaintState *st)
{
    pn_label_get_paint_state (label, st);
    return st->text;
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint   emits = 0;
    PnNode *node  = PN_NODE (pn_label_new ());

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, "hello");
    feed (node, "world");

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* With lines == 1 the label shows the last line of a multi-line output. */
static void
test_last_line (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    feed (PN_NODE (label), "alpha\nbeta\ngamma");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "gamma");

    g_object_unref (label);
}

/* With lines == 2 the label shows the last two lines, joined by '\n'. */
static void
test_last_two_lines (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    g_object_set (label, "lines", 2, NULL);

    feed (PN_NODE (label), "alpha\nbeta\ngamma");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "beta\ngamma");

    g_object_unref (label);
}

/* A trailing newline does not show as an empty last line. */
static void
test_trailing_newline_dropped (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    feed (PN_NODE (label), "one\ntwo\n");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "two");

    g_object_set (label, "lines", 2, NULL);
    PN_CHECK_CMPSTR (shown (label, &st), ==, "one\ntwo");

    g_object_unref (label);
}

/* Fewer lines than requested: a single-line output shows whole. */
static void
test_fewer_lines_than_requested (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    g_object_set (label, "lines", 2, NULL);

    feed (PN_NODE (label), "solo");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "solo");

    g_object_unref (label);
}

/* Carriage returns (CRLF output) are stripped so lines render clean. */
static void
test_crlf_stripped (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    g_object_set (label, "lines", 2, NULL);

    feed (PN_NODE (label), "a\r\nb\r\n");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "a\nb");

    g_object_unref (label);
}

/* An escaped newline (a literal backslash-n in the output) is treated as
 * a real line break, so the tail and the display split on it. */
static void
test_escaped_newline (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    /* The two characters backslash + 'n', not a real newline. */
    feed (PN_NODE (label), "first\\nsecond\\nthird");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "third");

    g_object_set (label, "lines", 2, NULL);
    PN_CHECK_CMPSTR (shown (label, &st), ==, "second\nthird");

    g_object_unref (label);
}

/* A message with no output member (or an empty one) blanks the label. */
static void
test_blank_on_no_output (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;
    PnMessage        *m;

    /* Show something first. */
    feed (PN_NODE (label), "visible");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "visible");

    /* A value-only message has no output → the label goes blank. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", 42.0);
    pn_node_receive_message (PN_NODE (label), m);
    g_object_unref (m);
    PN_CHECK_CMPSTR (shown (label, &st), ==, "");

    /* An explicitly empty output also blanks it. */
    feed (PN_NODE (label), "back");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "back");
    feed (PN_NODE (label), "");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "");

    g_object_unref (label);
}

/* Before any message the label is blank. */
static void
test_initial_is_blank (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    PN_CHECK_CMPSTR (shown (label, &st), ==, "");

    g_object_unref (label);
}

/* The locked font defaults: fill (100 %), Semi-Bold (Pango 600), upright,
 * and an empty family (so the painter falls back to the desktop font). */
static void
test_locked_defaults (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    pn_label_get_paint_state (label, &st);
    PN_CHECK_CMPINT (st.font_scale, ==, 100);
    PN_CHECK_CMPINT (st.weight,     ==, 600);
    PN_CHECK        (st.italic == FALSE);
    PN_CHECK_CMPSTR (st.font_family, ==, "");

    g_object_unref (label);
}

/* Changing the line count re-tails the already-received output without a
 * new message. */
static void
test_lines_change_retails (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;

    feed (PN_NODE (label), "x\ny\nz");
    PN_CHECK_CMPSTR (shown (label, &st), ==, "z");

    g_object_set (label, "lines", 2, NULL);
    PN_CHECK_CMPSTR (shown (label, &st), ==, "y\nz");

    g_object_set (label, "lines", 1, NULL);
    PN_CHECK_CMPSTR (shown (label, &st), ==, "z");

    g_object_unref (label);
}

/* The styling properties round-trip and surface in the snapshot. */
static void
test_properties_round_trip (void)
{
    PnLabel          *label = pn_label_new ();
    PnLabelPaintState st;
    PnColor           text_in = { 0.10, 0.20, 0.30, 1.0 };
    PnColor          *text_out = NULL;
    gchar            *family = NULL;
    gchar            *align = NULL;
    gint              scale = 0;
    gchar            *weight = NULL;

    g_object_set (label,
                  "font-family", "Monospace",
                  "font-scale",  60,
                  "weight",      "Semi-Bold",
                  "alignment",   "Right",
                  "text-color",  &text_in,
                  NULL);
    g_object_get (label,
                  "font-family", &family,
                  "font-scale",  &scale,
                  "weight",      &weight,
                  "alignment",   &align,
                  "text-color",  &text_out,
                  NULL);

    PN_CHECK_CMPSTR (family, ==, "Monospace");
    PN_CHECK_CMPINT (scale,  ==, 60);
    PN_CHECK_CMPSTR (weight, ==, "Semi-Bold");
    PN_CHECK_CMPSTR (align,  ==, "Right");
    PN_CHECK        (pn_color_equal (text_out, &text_in));

    pn_label_get_paint_state (label, &st);
    PN_CHECK_CMPSTR (st.font_family, ==, "Monospace");
    PN_CHECK_CMPINT (st.font_scale,  ==, 60);
    PN_CHECK_CMPINT (st.weight,      ==, 600);   /* Semi-Bold = Pango 600 */
    PN_CHECK        (st.align == PN_LABEL_ALIGN_RIGHT);
    PN_CHECK        (pn_color_equal (&st.text_color, &text_in));

    g_free (family);
    g_free (weight);
    g_free (align);
    pn_color_free (text_out);
    g_object_unref (label);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-label");
    pn_test_add ("is_a_sink",                 test_is_a_sink);
    pn_test_add ("last_line",                 test_last_line);
    pn_test_add ("last_two_lines",            test_last_two_lines);
    pn_test_add ("trailing_newline_dropped",  test_trailing_newline_dropped);
    pn_test_add ("fewer_lines_than_requested", test_fewer_lines_than_requested);
    pn_test_add ("crlf_stripped",             test_crlf_stripped);
    pn_test_add ("escaped_newline",           test_escaped_newline);
    pn_test_add ("blank_on_no_output",        test_blank_on_no_output);
    pn_test_add ("initial_is_blank",          test_initial_is_blank);
    pn_test_add ("locked_defaults",           test_locked_defaults);
    pn_test_add ("lines_change_retails",      test_lines_change_retails);
    pn_test_add ("properties_round_trip",     test_properties_round_trip);
    return pn_test_run ();
}
