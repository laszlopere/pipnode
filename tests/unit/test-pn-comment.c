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

/* Unit tests for PnComment: a port-less free-text annotation box.  It has
 * no message behaviour, so the tests cover its geometry (the footprint is
 * the stored width/height, the whole box is the header), the min-size
 * clamp on resize, and a full document round-trip through
 * pn_flow_to_string -> pn_flow_load_from_data (the same path undo/redo
 * uses), asserting text, size, position and colours survive.  Headless:
 * no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-comment.h"
#include "pn-color.h"
#include "pn-flow.h"
#include "pn-node-store.h"

/* A comment is a pure annotation: no input, no output port, and its
 * footprint is exactly the stored width/height with the whole box acting
 * as the header (so it is hit-tested / selectable end to end and has no
 * passive plot extension). */
static void
test_defaults_and_geometry (void)
{
    PnComment *comment = pn_comment_new ();
    PnNode    *node    = PN_NODE (comment);
    double     w, h, hh;

    PN_CHECK_FALSE (pn_node_get_has_input  (node));
    PN_CHECK_FALSE (pn_node_get_has_output (node));

    pn_node_get_size (node, &w, &h);
    PN_CHECK_NEAR (w, PN_COMMENT_DEFAULT_WIDTH,  1e-9);
    PN_CHECK_NEAR (h, PN_COMMENT_DEFAULT_HEIGHT, 1e-9);

    hh = pn_node_get_header_height (node);
    PN_CHECK_NEAR (hh, h, 1e-9);

    /* No paint_plot client area — the whole box is solid body. */
    PN_CHECK_FALSE (pn_node_get_client_area (node, NULL, NULL, NULL, NULL));

    g_object_unref (comment);
}

/* pn_comment_set_size resizes the box and clamps each dimension to its
 * minimum so a drag can never collapse the frame to nothing. */
static void
test_set_size_clamps (void)
{
    PnComment *comment = pn_comment_new ();
    double     w, h;

    pn_comment_set_size (comment, 300.0, 250.0);
    pn_node_get_size (PN_NODE (comment), &w, &h);
    PN_CHECK_NEAR (w, 300.0, 1e-9);
    PN_CHECK_NEAR (h, 250.0, 1e-9);

    /* Below the minimum: clamped, not honoured. */
    pn_comment_set_size (comment, 1.0, 1.0);
    pn_node_get_size (PN_NODE (comment), &w, &h);
    PN_CHECK_NEAR (w, PN_COMMENT_MIN_WIDTH,  1e-9);
    PN_CHECK_NEAR (h, PN_COMMENT_MIN_HEIGHT, 1e-9);

    g_object_unref (comment);
}

/* The text / size / colour properties read back what was written, and the
 * GTK-free paint-state seam mirrors them. */
static void
test_properties_and_paint_state (void)
{
    PnComment           *comment = pn_comment_new ();
    PnColor              frame   = { 0.20, 0.40, 0.60, 1.0 };
    PnCommentPaintState  st;

    g_object_set (comment,
                  "text",      "Hello\nworld",
                  "text-size", 18.0,
                  "frame-color", &frame,
                  NULL);

    pn_comment_get_paint_state (comment, &st);
    PN_CHECK_CMPSTR (st.text, ==, "Hello\nworld");
    PN_CHECK_NEAR   (st.text_size, 18.0, 1e-9);
    PN_CHECK_NEAR   (st.frame_color.red,   0.20, 1e-9);
    PN_CHECK_NEAR   (st.frame_color.green, 0.40, 1e-9);
    PN_CHECK_NEAR   (st.frame_color.blue,  0.60, 1e-9);

    g_object_unref (comment);
}

/* A comment serialises as a normal node and reloads intact through the
 * shared save/load path — so it round-trips through files AND undo/redo
 * for free.  Text, size, position and colours all survive. */
static void
test_document_round_trip (void)
{
    PnFlow    *flow = pn_flow_new ();
    PnComment *comment = pn_comment_new ();
    PnColor    bg    = { 0.90, 0.95, 1.00, 1.0 };
    PnPoint    pos   = { 123.0, 456.0 };
    gchar     *json;
    PnFlow    *flow2;
    PnNode    *loaded;
    PnComment *lc;
    PnCommentPaintState st;
    const PnPoint *lpos;
    GError    *error = NULL;

    g_object_set (comment,
                  "text",       "round trip",
                  "text-size",  15.0,
                  "background-color", &bg,
                  NULL);
    pn_comment_set_size (comment, 240.0, 160.0);
    pn_node_set_position (PN_NODE (comment), &pos);
    pn_flow_add_node (flow, PN_NODE (comment));
    g_object_unref (comment);

    json = pn_flow_to_string (flow);
    PN_CHECK (json != NULL);

    flow2 = pn_flow_new ();
    PN_CHECK (pn_flow_load_from_data (flow2, json, &error));
    PN_CHECK (error == NULL);
    PN_CHECK_CMPINT (pn_node_store_get_length (pn_flow_get_nodes (flow2)),
                     ==, 1);

    loaded = pn_node_store_get_node (pn_flow_get_nodes (flow2), 0);
    PN_CHECK (PN_IS_COMMENT (loaded));
    lc = PN_COMMENT (loaded);

    pn_comment_get_paint_state (lc, &st);
    PN_CHECK_CMPSTR (st.text, ==, "round trip");
    PN_CHECK_NEAR   (st.text_size, 15.0, 1e-9);
    PN_CHECK_NEAR   (st.width,  240.0, 1e-9);
    PN_CHECK_NEAR   (st.height, 160.0, 1e-9);
    /* Colour serialises as 8-bit hex, so allow one channel step (1/255). */
    PN_CHECK_NEAR   (st.background_color.red,   0.90, 1.0 / 255.0);
    PN_CHECK_NEAR   (st.background_color.green, 0.95, 1.0 / 255.0);
    PN_CHECK_NEAR   (st.background_color.blue,  1.00, 1.0 / 255.0);

    lpos = pn_node_get_position (loaded);
    PN_CHECK_NEAR (lpos->x, 123.0, 1e-9);
    PN_CHECK_NEAR (lpos->y, 456.0, 1e-9);

    g_free (json);
    g_object_unref (flow2);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-comment");
    pn_test_add ("defaults_geometry",   test_defaults_and_geometry);
    pn_test_add ("set_size_clamps",     test_set_size_clamps);
    pn_test_add ("properties",          test_properties_and_paint_state);
    pn_test_add ("document_round_trip",  test_document_round_trip);
    return pn_test_run ();
}
