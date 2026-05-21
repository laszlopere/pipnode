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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-file-viewer.h"
#include "pn-image-message.h"
#include "pn-message.h"

#include <gtk/gtk.h>

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
    GdkRGBA    area_color;
    GdkRGBA    border_color;
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
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Centre @text horizontally and vertically inside the rectangle
 *  (@x, @y, @w, @h), painted in a muted grey.  Used for the
 *  placeholder hint and the non-image filename label. */
static void
paint_centered_text (
        cairo_t     *cr,
        const gchar *text,
        double       x,
        double       y,
        double       w,
        double       h)
{
    cairo_text_extents_t ext;

    cairo_save (cr);
    cairo_select_font_face (cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, 13.0);
    cairo_set_source_rgba (cr, 0.45, 0.45, 0.48, 1.0);

    cairo_text_extents (cr, text, &ext);
    cairo_move_to (cr,
                   x + (w - ext.width)  / 2.0 - ext.x_bearing,
                   y + (h - ext.height) / 2.0 - ext.y_bearing);
    cairo_show_text (cr, text);
    cairo_restore (cr);
}

/** Paint the image so it *covers* the content rectangle
 *  (@cx, @cy, @cw, @ch): aspect ratio preserved, scaled so the image
 *  fills the whole box with no gap, centred so any overflow is cropped
 *  evenly.  Because the view area is sized to the image's own aspect
 *  ratio (see file_viewer_area_height), cover and fit coincide and
 *  nothing is cropped in the common case; cover only crops in the
 *  clamped extreme-aspect case, where it is still preferable to a white
 *  margin.  The caller is expected to have clipped to the same
 *  rectangle.  This matches #PnFileDrop's paint_pixbuf_cover so the same
 *  pixbuf renders identically in both nodes.  */
static void
paint_pixbuf_cover (
        cairo_t   *cr,
        GdkPixbuf *pixbuf,
        double     cx,
        double     cy,
        double     cw,
        double     ch)
{
    const double iw = (double) gdk_pixbuf_get_width  (pixbuf);
    const double ih = (double) gdk_pixbuf_get_height (pixbuf);
    double       scale, dw, dh, ox, oy;

    if (iw <= 0.0 || ih <= 0.0 || cw <= 0.0 || ch <= 0.0)
        return;

    /* Cover: the larger of the two ratios fills the box completely. */
    scale = MAX (cw / iw, ch / ih);
    dw    = iw * scale;
    dh    = ih * scale;
    ox    = cx + (cw - dw) / 2.0;
    oy    = cy + (ch - dh) / 2.0;

    cairo_save (cr);
    cairo_translate (cr, ox, oy);
    cairo_scale (cr, scale, scale);
    gdk_cairo_set_source_pixbuf (cr, pixbuf, 0.0, 0.0);
    /* GOOD is a sensible quality/speed trade-off for the down-scale
     * that the typical (larger-than-area) photo needs. */
    cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
    cairo_paint (cr);
    cairo_restore (cr);
}

/** PnNodeClass::paint_plot — draw the view-area rectangle and whatever
 *  preview / hint belongs in it, anchored at (@x, @y) with size
 *  @w × @h.  Mirrors #PnFileDrop's paint_plot so the two nodes look
 *  the same. */
static void
pn_file_viewer_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnFileViewer *self = PN_FILE_VIEWER (node);

    /* Base fill.  When an image is present it is painted over this
     * edge-to-edge, so the area colour only ever shows in the empty
     * state. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, &self->area_color);
    cairo_fill (cr);
    cairo_restore (cr);

    if (self->pixbuf != NULL)
    {
        /* Fill the whole rectangle with the image — no padding, no
         * white margin; the rectangle already carries the image's
         * aspect ratio, so the picture covers it exactly. */
        cairo_save (cr);
        cairo_rectangle (cr, x, y, w, h);
        cairo_clip (cr);
        paint_pixbuf_cover (cr, self->pixbuf, x, y, w, h);
        cairo_restore (cr);
    }
    else
    {
        /* Dashed inner outline + centred hint so an empty viewer reads
         * as a deliberate display waiting for an image. */
        const double cx = x + PN_FILE_VIEWER_PADDING;
        const double cy = y + PN_FILE_VIEWER_PADDING;
        const double cw = w - 2.0 * PN_FILE_VIEWER_PADDING;
        const double ch = h - 2.0 * PN_FILE_VIEWER_PADDING;

        cairo_save (cr);
        cairo_set_source_rgba (cr, 0.62, 0.62, 0.66, 1.0);
        cairo_set_line_width (cr, 1.0);
        {
            const double dashes[] = { 4.0, 3.0 };
            cairo_set_dash (cr, dashes, 2, 0.0);
        }
        cairo_rectangle (cr, cx, cy, cw, ch);
        cairo_stroke (cr);
        cairo_restore (cr);

        paint_centered_text (cr,
                             self->last_filename != NULL
                                 ? self->last_filename
                                 : "Nothing to show",
                             cx, cy, cw, ch);
    }

    /* Frame last, so it edges the image (or the empty area) cleanly on
     * top of whatever was drawn. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, &self->border_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);
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
            const GdkRGBA *c = g_value_get_boxed (value);
            if (c != NULL && !gdk_rgba_equal (c, &self->area_color))
            {
                self->area_color = *c;
                g_object_notify_by_pspec (object, props[PROP_AREA_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BORDER_COLOR:
        {
            const GdkRGBA *c = g_value_get_boxed (value);
            if (c != NULL && !gdk_rgba_equal (c, &self->border_color))
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
    node_class->paint_plot        = pn_file_viewer_paint_plot;
    /* A primary press on the view area lifts it into the centred zoom
     * overlay, the same gesture that enlarges a Graph's plot; click the
     * enlarged rectangle to drop it back.  Keep the preview's aspect
     * ratio when enlarged so the maximised image is not stretched. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    node_class->class_name = "FileViewer";
    node_class->icon       = "\xef\x80\xbe";  /* fa-image U+F03E */
    node_class->color      = (GdkRGBA){ 0.36, 0.60, 0.74, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_AREA_COLOR] = g_param_spec_boxed (
            "area-color", "Area colour",
            "Fill colour of the view-area rectangle drawn below the header",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BORDER_COLOR] = g_param_spec_boxed (
            "border-color", "Border colour",
            "Colour of the 1 px frame around the view area",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_file_viewer_init (PnFileViewer *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.36, 0.60, 0.74, 1.0 };

    self->pixbuf        = NULL;
    self->last_filename = NULL;
    self->area_color    = (GdkRGBA){ 1.0, 1.0, 1.0, 1.0 };   /* white */
    self->border_color  = (GdkRGBA){ 70.0/255.0, 70.0/255.0, 70.0/255.0, 1.0 };

    pn_node_set_class_name (node, "FileViewer");
    pn_node_set_icon       (node, "\xef\x80\xbe");  /* fa-image U+F03E */
    pn_node_set_color      (node, &color);
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
