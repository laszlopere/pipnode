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

/* Unit tests for PnFileDrop.  The GTK drag-and-drop and the cairo preview
 * are GUI-only, but the drop-emit logic is the GTK-free headless seam
 * (pn_filedrop_drop_file), so these tests drive it directly: it is a pure
 * source (no input, has output), a non-image drop emits a plain PnMessage
 * carrying the filename / path / mimetype / image=FALSE metadata, an image
 * drop emits a PnImageMessage carrying the same metadata plus pixel
 * dimensions and a ref-shared pixbuf preview, and the two colour properties
 * round-trip.
 *
 * The non-image path is exercised with a non-existent path so it stays
 * deterministic and never reads the filesystem (gdk-pixbuf fails fast, the
 * mime type is guessed from the name, the size query returns -1).  The
 * image path is the one place the node genuinely loads a file — its whole
 * job — so it is fed a tiny PNG the test writes to a private temp dir and
 * deletes again; this is the only filesystem touch and it is self-created. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-filedrop.h"
#include "pn-image-message.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

/* Capture the most recent emitted message (ref-held) and a running count. */
typedef struct
{
    guint      count;
    PnMessage *last;
} Capture;

static void
on_message (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *c = user_data;
    (void) node;
    c->count++;
    g_clear_object (&c->last);
    c->last = g_object_ref (message);
}

/* FileDrop is a source: no input, an output, in the Sources category. */
static void
test_is_a_source (void)
{
    PnNode *node = PN_NODE (pn_filedrop_new ());

    PN_CHECK (pn_node_get_has_output (node) == TRUE);
    PN_CHECK (pn_node_get_has_input  (node) == FALSE);

    g_object_unref (node);
}

/* Before any drop the preview is empty: no pixbuf, no filename hint. */
static void
test_initial_empty (void)
{
    PnFileDrop          *fd = pn_filedrop_new ();
    PnFileDropPaintState st;

    pn_filedrop_get_paint_state (fd, &st);
    PN_CHECK (st.pixbuf == NULL);
    PN_CHECK (st.last_filename == NULL);

    g_object_unref (fd);
}

/* Dropping a non-image (here a path that does not resolve to a decodable
 * image) emits one plain PnMessage — never an image message — carrying the
 * filename / path / image-flag metadata, and leaves the preview pixbuf
 * clear while still recording the basename hint. */
static void
test_non_image_drop_emits_plain (void)
{
    PnFileDrop          *fd = pn_filedrop_new ();
    Capture              cap = { 0, NULL };
    PnFileDropPaintState st;

    g_signal_connect (fd, "message", G_CALLBACK (on_message), &cap);

    pn_filedrop_drop_file (fd, "/nonexistent/dir/notes.txt");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK (cap.last != NULL);
    /* A plain message, not the image subclass. */
    PN_CHECK (!PN_IS_IMAGE_MESSAGE (cap.last));

    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "filename"), ==, "notes.txt");
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "path"),     ==,
                     "/nonexistent/dir/notes.txt");
    PN_CHECK        (pn_test_bool (cap.last, "image")     == FALSE);
    PN_CHECK        (pn_test_bool (cap.last, "success")   == TRUE);
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "output"),   ==, "notes.txt");

    /* No image preview for a non-image drop, but the hint is set. */
    pn_filedrop_get_paint_state (fd, &st);
    PN_CHECK (st.pixbuf == NULL);
    PN_CHECK_CMPSTR (st.last_filename, ==, "notes.txt");

    g_clear_object (&cap.last);
    g_object_unref (fd);
}

/* Dropping a real (tiny, self-written) image emits a PnImageMessage that
 * carries the pixel dimensions and image=TRUE, and the node keeps the
 * decoded pixbuf as its on-canvas preview. */
static void
test_image_drop_emits_image_msg (void)
{
    PnFileDrop          *fd  = pn_filedrop_new ();
    Capture              cap = { 0, NULL };
    PnFileDropPaintState st;
    GdkPixbuf           *pb;
    gchar               *dir;
    gchar               *path;
    gboolean             saved;

    /* Write a 4x2 PNG into a private temp dir — the only filesystem touch,
     * and it is one we create and clean up ourselves. */
    dir = g_dir_make_tmp ("pn-filedrop-XXXXXX", NULL);
    g_assert (dir != NULL);
    path = g_build_filename (dir, "img.png", NULL);

    pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 4, 2);
    gdk_pixbuf_fill (pb, 0xff0000ffu);
    saved = gdk_pixbuf_save (pb, path, "png", NULL, NULL);
    g_object_unref (pb);
    PN_CHECK (saved == TRUE);

    g_signal_connect (fd, "message", G_CALLBACK (on_message), &cap);
    pn_filedrop_drop_file (fd, path);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK (cap.last != NULL);
    PN_CHECK (PN_IS_IMAGE_MESSAGE (cap.last));

    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "filename"), ==, "img.png");
    PN_CHECK        (pn_test_bool (cap.last, "image")     == TRUE);
    PN_CHECK        (pn_test_bool (cap.last, "success")   == TRUE);
    PN_CHECK_CMPINT ((int) pn_test_num (cap.last, "width"),  ==, 4);
    PN_CHECK_CMPINT ((int) pn_test_num (cap.last, "height"), ==, 2);

    /* The image message carries the decoded pixels out of band. */
    if (PN_IS_IMAGE_MESSAGE (cap.last))
        PN_CHECK (pn_image_message_get_pixbuf (
                      PN_IMAGE_MESSAGE (cap.last)) != NULL);

    /* The node keeps the pixbuf as its live preview. */
    pn_filedrop_get_paint_state (fd, &st);
    PN_CHECK (st.pixbuf != NULL);
    PN_CHECK_CMPSTR (st.last_filename, ==, "img.png");

    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
    g_clear_object (&cap.last);
    g_object_unref (fd);
}

/* The two appearance colours round-trip and surface in the snapshot. */
static void
test_properties_round_trip (void)
{
    PnFileDrop          *fd       = pn_filedrop_new ();
    PnColor              area_in  = { 0.10, 0.20, 0.30, 1.0 };
    PnColor              bdr_in   = { 0.40, 0.50, 0.60, 1.0 };
    PnColor             *area_out = NULL, *bdr_out = NULL;
    PnFileDropPaintState st;

    g_object_set (fd, "area-color", &area_in, "border-color", &bdr_in, NULL);
    g_object_get (fd, "area-color", &area_out, "border-color", &bdr_out, NULL);

    PN_CHECK (area_out != NULL && pn_color_equal (area_out, &area_in));
    PN_CHECK (bdr_out  != NULL && pn_color_equal (bdr_out,  &bdr_in));

    pn_filedrop_get_paint_state (fd, &st);
    PN_CHECK (pn_color_equal (&st.area_color,   &area_in));
    PN_CHECK (pn_color_equal (&st.border_color, &bdr_in));

    pn_color_free (area_out);
    pn_color_free (bdr_out);
    g_object_unref (fd);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-filedrop");
    pn_test_add ("is_a_source",            test_is_a_source);
    pn_test_add ("initial_empty",          test_initial_empty);
    pn_test_add ("non_image_drop_plain",   test_non_image_drop_emits_plain);
    pn_test_add ("image_drop_image_msg",   test_image_drop_emits_image_msg);
    pn_test_add ("properties_round_trip",  test_properties_round_trip);
    return pn_test_run ();
}
