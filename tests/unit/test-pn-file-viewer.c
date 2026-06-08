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

/* Unit tests for PnFileViewer.  The cairo preview only matters on screen,
 * so these headless tests cover the logic contract: it is a pure sink, an
 * incoming PnImageMessage's already-decoded GdkPixbuf is ref-shared into
 * the preview (no file read — the node never decodes, it only refs the
 * pixbuf the upstream FileDrop already loaded), the data.filename hint is
 * latched on every message type, a non-image message clears the preview,
 * the intrinsic size follows the image aspect ratio, and the two colour
 * properties round-trip.  The preview state is read through the GTK-free
 * seam (pn_file_viewer_get_paint_state).  A small in-memory GdkPixbuf
 * stands in for an image so nothing touches the filesystem. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-file-viewer.h"
#include "pn-image-message.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

/* A blank @w x @h RGB pixbuf, built entirely in memory (no file I/O). */
static GdkPixbuf *
make_pixbuf (int w, int h)
{
    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_fill (pb, 0x336699ffu);
    return pb;
}

/* Borrow the current preview state through the GTK-free seam. */
static void
peek (PnFileViewer *v, PnFileViewerPaintState *st)
{
    pn_file_viewer_get_paint_state (v, st);
}

/* A plain message carrying a data.filename string and no image. */
static void
feed_plain (PnNode *node, const gchar *filename)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    if (filename != NULL)
        pn_message_set_string (m, "filename", filename);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* An image message carrying @pixbuf plus an optional filename hint. */
static void
feed_image (PnNode *node, GdkPixbuf *pixbuf, const gchar *filename)
{
    PnImageMessage *m = pn_image_message_new (NULL, NULL, pixbuf);
    if (filename != NULL)
        pn_message_set_string (PN_MESSAGE (m), "filename", filename);
    pn_node_receive_message (node, PN_MESSAGE (m));
    g_object_unref (m);
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint      emits;
    PnNode    *node = PN_NODE (pn_file_viewer_new ());
    GdkPixbuf *pb   = make_pixbuf (4, 3);

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed_plain (node, "notes.txt");
    feed_image (node, pb, "pic.png");

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (pb);
    g_object_unref (node);
}

/* Before any message the preview is empty: no pixbuf, no filename hint. */
static void
test_initial_empty (void)
{
    PnFileViewer          *v = pn_file_viewer_new ();
    PnFileViewerPaintState st;

    peek (v, &st);
    PN_CHECK (st.pixbuf == NULL);
    PN_CHECK (st.last_filename == NULL);

    g_object_unref (v);
}

/* An image message ref-shares its pixbuf into the preview (the very same
 * pointer, not a copy) and records the filename hint. */
static void
test_image_latches_pixbuf (void)
{
    PnFileViewer          *v  = pn_file_viewer_new ();
    GdkPixbuf             *pb = make_pixbuf (8, 4);
    PnFileViewerPaintState st;

    feed_image (PN_NODE (v), pb, "shot.png");

    peek (v, &st);
    PN_CHECK (st.pixbuf == pb);                     /* ref-shared, same ptr */
    PN_CHECK_CMPSTR (st.last_filename, ==, "shot.png");

    g_object_unref (pb);
    g_object_unref (v);
}

/* The data.filename hint is latched regardless of message type, and a
 * message with no filename clears the hint back to NULL. */
static void
test_filename_hint (void)
{
    PnFileViewer          *v = pn_file_viewer_new ();
    PnFileViewerPaintState st;
    PnMessage             *m;

    feed_plain (PN_NODE (v), "report.pdf");
    peek (v, &st);
    PN_CHECK_CMPSTR (st.last_filename, ==, "report.pdf");

    /* A message with no filename member clears the hint. */
    feed_plain (PN_NODE (v), NULL);
    peek (v, &st);
    PN_CHECK (st.last_filename == NULL);

    /* A non-string filename is ignored (treated as absent). */
    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "filename", 3.14);
    pn_node_receive_message (PN_NODE (v), m);
    g_object_unref (m);
    peek (v, &st);
    PN_CHECK (st.last_filename == NULL);

    g_object_unref (v);
}

/* A non-image message after an image clears the preview pixbuf but the
 * node still reads the filename hint off the same message. */
static void
test_non_image_clears_preview (void)
{
    PnFileViewer          *v  = pn_file_viewer_new ();
    GdkPixbuf             *pb = make_pixbuf (6, 6);
    PnFileViewerPaintState st;

    feed_image (PN_NODE (v), pb, "kept.png");
    peek (v, &st);
    PN_CHECK (st.pixbuf == pb);

    /* A plain message clears the image preview... */
    feed_plain (PN_NODE (v), "doc.txt");
    peek (v, &st);
    PN_CHECK (st.pixbuf == NULL);
    /* ...and updates the hint from the same message. */
    PN_CHECK_CMPSTR (st.last_filename, ==, "doc.txt");

    g_object_unref (pb);
    g_object_unref (v);
}

/* The intrinsic size tracks the loaded image's aspect ratio: a wide image
 * makes a shorter view area than a tall one, both at the fixed node width. */
static void
test_size_follows_aspect (void)
{
    PnFileViewer *v = pn_file_viewer_new ();
    GdkPixbuf    *wide = make_pixbuf (200, 50);
    GdkPixbuf    *tall = make_pixbuf (50, 200);
    double        h_empty = 0, h_wide = 0, h_tall = 0, w = 0;

    pn_node_get_size (PN_NODE (v), &w, &h_empty);

    feed_image (PN_NODE (v), wide, NULL);
    pn_node_get_size (PN_NODE (v), &w, &h_wide);

    feed_image (PN_NODE (v), tall, NULL);
    pn_node_get_size (PN_NODE (v), &w, &h_tall);

    /* Width is fixed; a tall image yields a taller node than a wide one. */
    PN_CHECK (w > 0.0);
    PN_CHECK (h_tall > h_wide);
    /* The empty placeholder sits between the two extremes' clamps; at
     * minimum it differs from the tall image's height. */
    PN_CHECK (h_empty != h_tall);

    g_object_unref (wide);
    g_object_unref (tall);
    g_object_unref (v);
}

/* The two appearance colours round-trip and surface in the snapshot. */
static void
test_properties_round_trip (void)
{
    PnFileViewer          *v        = pn_file_viewer_new ();
    PnColor                area_in  = { 0.10, 0.20, 0.30, 1.0 };
    PnColor                bdr_in   = { 0.40, 0.50, 0.60, 1.0 };
    PnColor               *area_out = NULL, *bdr_out = NULL;
    PnFileViewerPaintState st;

    g_object_set (v, "area-color", &area_in, "border-color", &bdr_in, NULL);
    g_object_get (v, "area-color", &area_out, "border-color", &bdr_out, NULL);

    PN_CHECK (area_out != NULL && pn_color_equal (area_out, &area_in));
    PN_CHECK (bdr_out  != NULL && pn_color_equal (bdr_out,  &bdr_in));

    peek (v, &st);
    PN_CHECK (pn_color_equal (&st.area_color,   &area_in));
    PN_CHECK (pn_color_equal (&st.border_color, &bdr_in));

    pn_color_free (area_out);
    pn_color_free (bdr_out);
    g_object_unref (v);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-file-viewer");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("initial_empty",          test_initial_empty);
    pn_test_add ("image_latches_pixbuf",   test_image_latches_pixbuf);
    pn_test_add ("filename_hint",          test_filename_hint);
    pn_test_add ("non_image_clears",       test_non_image_clears_preview);
    pn_test_add ("size_follows_aspect",    test_size_follows_aspect);
    pn_test_add ("properties_round_trip",  test_properties_round_trip);
    return pn_test_run ();
}
