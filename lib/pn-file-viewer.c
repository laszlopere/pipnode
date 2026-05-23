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

/* ------------------------------------------------------------------ */
/*  PnFileViewer — logic tier (headless core).                         */
/*                                                                     */
/*  This file holds the GTK-free half of the FileViewer sink node: the */
/*  GType, the two appearance colour properties, the intrinsic size    */
/*  (get_size / get_header_height, driven by the received image's       */
/*  aspect ratio) and the receive() that stores what to show — it refs  */
/*  the #GdkPixbuf carried by an incoming #PnImageMessage (gdk-pixbuf   */
/*  is an allowed core image-data dep) and reads the filename hint from */
/*  the data bag.  The cairo preview painter lives in the companion     */
/*  gui-tier file pn-file-viewer-gui.c, which installs that paint vfunc */
/*  slot onto this class at editor startup (see                         */
/*  pn_file_viewer_gui_install) and reads the preview state through the */
/*  GTK-free pn_file_viewer_get_paint_state() seam.  The headless       */
/*  runtime registers and runs this node without ever pulling GTK.     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-file-viewer.h"
#include "pn-image-message.h"
#include "pn-message.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Deliberately identical to #PnFileDrop: a standard header on top, a */
/*  square-ish view area below it, separated by the same small gap the */
/*  Graph node leaves between its body and its plot rectangle, so a    */
/*  FileDrop wired to a FileViewer shows the same picture at the same  */
/*  size in both nodes.                                                */
/* ------------------------------------------------------------------ */

#define PN_FILE_VIEWER_WIDTH         200.0
#define PN_FILE_VIEWER_HEADER_HEIGHT  40.0
#define PN_FILE_VIEWER_GAP             4.0

/* View-area height shown before any image has arrived (and after a
 * non-image message): a square-ish placeholder for the hint text. */
#define PN_FILE_VIEWER_EMPTY_AREA_HEIGHT 150.0

/* Bounds on the image-driven view-area height.  Once an image arrives
 * the area takes the image's aspect ratio at the fixed
 * PN_FILE_VIEWER_WIDTH, so the picture fills it edge-to-edge with no
 * letterbox; these clamps stop a panorama (very short) or a tall
 * portrait (very long) from producing an absurdly-shaped node. */
#define PN_FILE_VIEWER_AREA_MIN_HEIGHT    80.0
#define PN_FILE_VIEWER_AREA_MAX_HEIGHT   360.0

/* Inset used only by the empty-state hint (dashed box + label) so the
 * placeholder does not butt right up against the frame.  A shown image
 * deliberately uses no inset — it runs to the frame. */
#define PN_FILE_VIEWER_PADDING            6.0

struct _PnFileViewer
{
    PnNode parent_instance;

    /* Most recently received image, kept at full resolution and scaled
     * at paint time so it stays crisp when the node is zoomed.  Holds a
     * reference taken from the incoming #PnImageMessage.  %NULL whenever
     * the most recent message carried no image (or nothing has arrived
     * yet). */
    GdkPixbuf *pixbuf;

    /* Basename of the most recent message, shown as the hint text when
     * no image preview is up.  Read from the `filename` member of the
     * message data bag when present.  %NULL before the first message. */
    gchar     *last_filename;

    /* Appearance — view-area fill and frame.  Both serialise like any
     * other writable property and only affect painting. */
    PnColor    area_color;
    PnColor    border_color;
};

G_DEFINE_TYPE (PnFileViewer, pn_file_viewer, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_AREA_COLOR,
    PROP_BORDER_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

/** Height of the view-area rectangle for the current state.  With an
 *  image loaded the area takes the image's aspect ratio at the fixed
 *  area width so the picture fills it with no white margin (clamped to
 *  sane bounds); with no image it falls back to the placeholder
 *  height. */
static double
file_viewer_area_height (PnFileViewer *self)
{
    double iw, ih, h;

    if (self->pixbuf == NULL)
        return PN_FILE_VIEWER_EMPTY_AREA_HEIGHT;

    iw = (double) gdk_pixbuf_get_width  (self->pixbuf);
    ih = (double) gdk_pixbuf_get_height (self->pixbuf);
    if (iw <= 0.0 || ih <= 0.0)
        return PN_FILE_VIEWER_EMPTY_AREA_HEIGHT;

    h = PN_FILE_VIEWER_WIDTH * ih / iw;
    return CLAMP (h, PN_FILE_VIEWER_AREA_MIN_HEIGHT, PN_FILE_VIEWER_AREA_MAX_HEIGHT);
}

static void
pn_file_viewer_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    if (out_width  != NULL) *out_width  = PN_FILE_VIEWER_WIDTH;
    if (out_height != NULL)
        *out_height = PN_FILE_VIEWER_HEADER_HEIGHT
                    + PN_FILE_VIEWER_GAP
                    + file_viewer_area_height (PN_FILE_VIEWER (node));
}

static double
pn_file_viewer_get_header_height (PnNode *node)
{
    (void) node;
    return PN_FILE_VIEWER_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GUI paint-state seam (GTK-free)                                    */
/*                                                                     */
/*  The cairo preview painter lives in the gui tier                    */
/*  (pn-file-viewer-gui.c) and cannot see this file's private instance */
/*  struct.  It reads everything it needs through this one snapshot     */
/*  accessor: the borrowed preview pixbuf (or %NULL), the borrowed      */
/*  filename hint (or %NULL), and the two appearance colours by value.  */
/*  gdk-pixbuf is an allowed core dep, so the pixbuf can ride the       */
/*  snapshot as a plain (borrowed) pointer with no GTK involvement.     */
/* ------------------------------------------------------------------ */

void
pn_file_viewer_get_paint_state (
        PnFileViewer            *self,
        PnFileViewerPaintState  *out)
{
    g_return_if_fail (PN_IS_FILE_VIEWER (self));
    g_return_if_fail (out != NULL);

    out->pixbuf        = self->pixbuf;        /* borrowed */
    out->last_filename = self->last_filename; /* borrowed */
    out->area_color    = self->area_color;
    out->border_color  = self->border_color;
}

/* ------------------------------------------------------------------ */
/*  Message handling                                                   */
/* ------------------------------------------------------------------ */

/** Borrowed basename from the message's data bag (`filename` member),
 *  or %NULL when absent / not a string. */
static const gchar *
message_filename (PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *node;

    if (data == NULL || !json_object_has_member (data, "filename"))
        return NULL;

    node = json_object_get_member (data, "filename");
    if (node == NULL
        || !JSON_NODE_HOLDS_VALUE (node)
        || json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

/** PnNodeClass::receive — take whatever the message carries and show
 *  it.  An image message hands us the same #GdkPixbuf the upstream
 *  FileDrop is displaying, so we ref it and paint it identically; any
 *  other message clears the preview and falls back to the hint. */
static void
pn_file_viewer_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnFileViewer *self = PN_FILE_VIEWER (node);
    const gchar  *filename;
    GdkPixbuf    *pixbuf = NULL;

    if (PN_IS_IMAGE_MESSAGE (message))
        pixbuf = pn_image_message_get_pixbuf (PN_IMAGE_MESSAGE (message));

    /* Remember the basename for the hint regardless of message type. */
    filename = message_filename (message);
    g_free (self->last_filename);
    self->last_filename = filename != NULL ? g_strdup (filename) : NULL;

    /* Swap in the new image (ref-shared with the upstream node and any
     * other branch) or clear the preview for a non-image message. */
    g_clear_object (&self->pixbuf);
    if (pixbuf != NULL)
        self->pixbuf = g_object_ref (pixbuf);

    /* The view area resizes to the new image's aspect ratio, so a
     * repaint request both redraws and re-queries get_size. */
    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_file_viewer_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFileViewer *self = PN_FILE_VIEWER (object);

    switch (prop_id)
    {
    case PROP_AREA_COLOR:
        g_value_set_boxed (value, &self->area_color);
        break;
    case PROP_BORDER_COLOR:
        g_value_set_boxed (value, &self->border_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_file_viewer_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFileViewer *self = PN_FILE_VIEWER (object);

    switch (prop_id)
    {
    case PROP_AREA_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->area_color))
            {
                self->area_color = *c;
                g_object_notify_by_pspec (object, props[PROP_AREA_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BORDER_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->border_color))
            {
                self->border_color = *c;
                g_object_notify_by_pspec (object, props[PROP_BORDER_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_file_viewer_finalize (GObject *object)
{
    PnFileViewer *self = PN_FILE_VIEWER (object);

    g_clear_object  (&self->pixbuf);
    g_clear_pointer (&self->last_filename, g_free);

    G_OBJECT_CLASS (pn_file_viewer_parent_class)->finalize (object);
}

static void
pn_file_viewer_class_init (PnFileViewerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_file_viewer_get_property;
    object_class->set_property = pn_file_viewer_set_property;
    object_class->finalize     = pn_file_viewer_finalize;

    node_class->receive           = pn_file_viewer_receive;
    node_class->get_size          = pn_file_viewer_get_size;
    node_class->get_header_height = pn_file_viewer_get_header_height;
    /* The cairo paint_plot painter and its companion
     * paint_plot_zoom_keep_aspect flag are installed by the gui tier
     * (pn_file_viewer_gui_install); the headless core leaves them NULL. */

    node_class->class_name = "FileViewer";
    node_class->icon       = "\xef\x80\xbe";  /* fa-image U+F03E */
    node_class->color      = (PnColor){ 0.36, 0.60, 0.74, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_AREA_COLOR] = g_param_spec_boxed (
            "area-color", "Area colour",
            "Fill colour of the view-area rectangle drawn below the header",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BORDER_COLOR] = g_param_spec_boxed (
            "border-color", "Border colour",
            "Colour of the 1 px frame around the view area",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_file_viewer_init (PnFileViewer *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.36, 0.60, 0.74, 1.0 };

    self->pixbuf        = NULL;
    self->last_filename = NULL;
    self->area_color    = (PnColor){ 1.0, 1.0, 1.0, 1.0 };   /* white */
    self->border_color  = (PnColor){ 70.0/255.0, 70.0/255.0, 70.0/255.0, 1.0 };

    pn_node_set_class_name (node, "FileViewer");
    pn_node_set_icon       (node, "\xef\x80\xbe");  /* fa-image U+F03E */
    pn_node_set_color (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFileViewer *
pn_file_viewer_new (void)
{
    return g_object_new (PN_TYPE_FILE_VIEWER, NULL);
}
