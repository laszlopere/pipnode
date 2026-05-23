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

#include "pn-filedrop.h"
#include "pn-image-message.h"
#include "pn-message.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Standard-style header on top, a square-ish drop area below it,     */
/*  separated by the same small gap the Graph node leaves between its  */
/*  body and its plot rectangle so the two read as distinct elements.  */
/* ------------------------------------------------------------------ */

#define PN_FILEDROP_WIDTH         200.0
#define PN_FILEDROP_HEADER_HEIGHT  40.0
#define PN_FILEDROP_GAP             4.0

/* Drop-area height shown before any image has been dropped (and after a
 * non-image drop): a square-ish placeholder for the "Drop a file here"
 * hint. */
#define PN_FILEDROP_EMPTY_AREA_HEIGHT 150.0

/* Bounds on the image-driven drop-area height.  Once an image is
 * dropped the area takes the image's aspect ratio at the fixed
 * PN_FILEDROP_WIDTH, so the picture fills it edge-to-edge with no
 * letterbox; these clamps stop a panorama (very short) or a tall
 * portrait (very long) from producing an absurdly-shaped node. */
#define PN_FILEDROP_AREA_MIN_HEIGHT    80.0
#define PN_FILEDROP_AREA_MAX_HEIGHT   360.0

/* Inset used only by the empty-state hint (dashed box + label) so the
 * placeholder does not butt right up against the frame.  A previewed
 * image deliberately uses no inset — it runs to the frame. */
#define PN_FILEDROP_PADDING            6.0

struct _PnFileDrop
{
    PnNode parent_instance;

    /* Last dropped image, kept at full resolution and scaled at paint
     * time so it stays crisp when the node is zoomed.  %NULL whenever
     * the most recent drop was not an image (or nothing was dropped
     * yet). */
    GdkPixbuf *pixbuf;

    /* Basename of the most recent drop, shown as the hint text when no
     * image preview is up.  %NULL before the first drop. */
    gchar     *last_filename;

    /* Appearance — drop-area fill and frame.  Both serialise like any
     * other writable property and only affect painting. */
    GdkRGBA    area_color;
    GdkRGBA    border_color;
};

G_DEFINE_TYPE (PnFileDrop, pn_filedrop, PN_TYPE_NODE)

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

/** Height of the drop-area rectangle for the current state.  With an
 *  image loaded the area takes the image's aspect ratio at the fixed
 *  area width so the picture fills it with no white margin (clamped to
 *  sane bounds); with no image it falls back to the placeholder
 *  height. */
static double
filedrop_area_height (PnFileDrop *self)
{
    double iw, ih, h;

    if (self->pixbuf == NULL)
        return PN_FILEDROP_EMPTY_AREA_HEIGHT;

    iw = (double) gdk_pixbuf_get_width  (self->pixbuf);
    ih = (double) gdk_pixbuf_get_height (self->pixbuf);
    if (iw <= 0.0 || ih <= 0.0)
        return PN_FILEDROP_EMPTY_AREA_HEIGHT;

    h = PN_FILEDROP_WIDTH * ih / iw;
    return CLAMP (h, PN_FILEDROP_AREA_MIN_HEIGHT, PN_FILEDROP_AREA_MAX_HEIGHT);
}

static void
pn_filedrop_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    if (out_width  != NULL) *out_width  = PN_FILEDROP_WIDTH;
    if (out_height != NULL)
        *out_height = PN_FILEDROP_HEADER_HEIGHT
                    + PN_FILEDROP_GAP
                    + filedrop_area_height (PN_FILEDROP (node));
}

static double
pn_filedrop_get_header_height (PnNode *node)
{
    (void) node;
    return PN_FILEDROP_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Centre @text horizontally and vertically inside the rectangle
 *  (@x, @y, @w, @h), painted in a muted grey.  Used for the
 *  "Drop a file here" placeholder and the non-image filename label. */
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

/** Paint the dropped image so it *covers* the content rectangle
 *  (@cx, @cy, @cw, @ch): aspect ratio preserved, scaled so the image
 *  fills the whole box with no gap, centred so any overflow is cropped
 *  evenly.  Because the drop area is sized to the image's own aspect
 *  ratio (see filedrop_area_height), cover and fit coincide and nothing
 *  is cropped in the common case; cover only crops in the clamped
 *  extreme-aspect case, where it is still preferable to a white margin.
 *  The caller is expected to have clipped to the same rectangle.  */
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

/** PnNodeClass::paint_plot — draw the drop-area rectangle and whatever
 *  preview / hint belongs in it, anchored at (@x, @y) with size
 *  @w × @h.  Mirrors the Graph node's "fill the rectangle even when
 *  empty so it reads as a deliberate area" approach. */
static void
pn_filedrop_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnFileDrop *self = PN_FILEDROP (node);

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
        /* Dashed inner outline + centred hint so an empty target
         * advertises itself as a place to drop something. */
        const double cx = x + PN_FILEDROP_PADDING;
        const double cy = y + PN_FILEDROP_PADDING;
        const double cw = w - 2.0 * PN_FILEDROP_PADDING;
        const double ch = h - 2.0 * PN_FILEDROP_PADDING;

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
                                 : "Drop a file here",
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
/*  Drop handling                                                      */
/* ------------------------------------------------------------------ */

/** Query @path's byte size, or -1 if it cannot be determined. */
static gint64
query_file_size (const gchar *path)
{
    GFile     *file = g_file_new_for_path (path);
    GFileInfo *info;
    gint64     size = -1;

    info = g_file_query_info (file,
                              G_FILE_ATTRIBUTE_STANDARD_SIZE,
                              G_FILE_QUERY_INFO_NONE,
                              NULL, NULL);
    if (info != NULL)
    {
        size = (gint64) g_file_info_get_size (info);
        g_object_unref (info);
    }

    g_object_unref (file);
    return size;
}

void
pn_filedrop_drop_file (
        PnFileDrop  *self,
        const gchar *path)
{
    PnNode    *node = PN_NODE (self);
    GdkPixbuf *pixbuf;
    gchar     *basename;
    gchar     *content_type;
    gint64     size;

    g_return_if_fail (PN_IS_FILEDROP (self));
    g_return_if_fail (path != NULL);

    basename     = g_path_get_basename (path);
    content_type = g_content_type_guess (path, NULL, 0, NULL);
    size         = query_file_size (path);

    /* The cheapest reliable "is this an image?" test is to try to
     * decode it: gdk-pixbuf understands every format we could preview
     * anyway, and a successful load hands us exactly the pixbuf we want
     * to both show and ship.  A NULL return (unsupported / corrupt /
     * not-an-image) falls through to the plain-file path. */
    pixbuf = gdk_pixbuf_new_from_file (path, NULL);

    /* Remember the basename for the empty-area hint regardless of type. */
    g_free (self->last_filename);
    self->last_filename = g_strdup (basename);

    if (pixbuf != NULL)
    {
        PnImageMessage *msg;
        gchar          *summary;
        const gint      iw = gdk_pixbuf_get_width  (pixbuf);
        const gint      ih = gdk_pixbuf_get_height (pixbuf);

        /* Show it: take over the load reference for the on-canvas
         * preview; the message takes its own ref below. */
        g_clear_object (&self->pixbuf);
        self->pixbuf = pixbuf;   /* transfer ownership of the load ref */

        msg = pn_image_message_new (node, NULL, pixbuf);

        /* Metadata only — the pixels ride the pixbuf pointer, never the
         * data bag, so they never reach a JSON serialisation. */
        pn_message_set_string  (PN_MESSAGE (msg), "filename", basename);
        pn_message_set_string  (PN_MESSAGE (msg), "path",     path);
        pn_message_set_string  (PN_MESSAGE (msg), "mimetype",
                                content_type ? content_type : "image");
        pn_message_set_boolean (PN_MESSAGE (msg), "image",    TRUE);
        pn_message_set_int     (PN_MESSAGE (msg), "width",    iw);
        pn_message_set_int     (PN_MESSAGE (msg), "height",   ih);
        if (size >= 0)
            pn_message_set_int64 (PN_MESSAGE (msg), "size", size);
        pn_message_set_boolean (PN_MESSAGE (msg), "success", TRUE);

        summary = g_strdup_printf ("%s (%d×%d)", basename, iw, ih);
        pn_message_set_string  (PN_MESSAGE (msg), "output", summary);
        g_free (summary);

        pn_node_emit_message (node, PN_MESSAGE (msg));
        g_object_unref (msg);
    }
    else
    {
        PnMessage *msg = pn_message_new (node, NULL);

        /* No preview for a non-image: clear any image left from a
         * previous drop so the area falls back to the filename hint. */
        g_clear_object (&self->pixbuf);

        pn_message_set_string  (msg, "filename", basename);
        pn_message_set_string  (msg, "path",     path);
        pn_message_set_string  (msg, "mimetype",
                                content_type ? content_type
                                             : "application/octet-stream");
        pn_message_set_boolean (msg, "image", FALSE);
        if (size >= 0)
            pn_message_set_int64 (msg, "size", size);
        pn_message_set_boolean (msg, "success", TRUE);
        pn_message_set_string  (msg, "output", basename);

        pn_node_emit_message (node, msg);
        g_object_unref (msg);
    }

    g_free (basename);
    g_free (content_type);

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_filedrop_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFileDrop *self = PN_FILEDROP (object);

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
pn_filedrop_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFileDrop *self = PN_FILEDROP (object);

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
pn_filedrop_finalize (GObject *object)
{
    PnFileDrop *self = PN_FILEDROP (object);

    g_clear_object  (&self->pixbuf);
    g_clear_pointer (&self->last_filename, g_free);

    G_OBJECT_CLASS (pn_filedrop_parent_class)->finalize (object);
}

static void
pn_filedrop_class_init (PnFileDropClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_filedrop_get_property;
    object_class->set_property = pn_filedrop_set_property;
    object_class->finalize     = pn_filedrop_finalize;

    node_class->get_size          = pn_filedrop_get_size;
    node_class->get_header_height = pn_filedrop_get_header_height;
    node_class->paint_plot        = pn_filedrop_paint_plot;
    /* A primary press on the drop area lifts it into the centred zoom
     * overlay, the same gesture that enlarges a Graph's plot; click the
     * enlarged rectangle to drop it back.  (File drops are routed by the
     * worksheet's drag-and-drop handler, not by a click, so this does not
     * interfere with dropping files onto the node.)  Keep the preview's
     * aspect ratio when enlarged so the maximised image is not stretched. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    node_class->class_name = "FileDrop";
    node_class->icon       = "\xef\x80\x9c";  /* fa-inbox U+F01C */
    node_class->color      = (PnColor){ 0.36, 0.60, 0.74, 1.0 };
    node_class->category   = "Sources";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;

    props[PROP_AREA_COLOR] = g_param_spec_boxed (
            "area-color", "Area colour",
            "Fill colour of the drop-area rectangle drawn below the header",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BORDER_COLOR] = g_param_spec_boxed (
            "border-color", "Border colour",
            "Colour of the 1 px frame around the drop area",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_filedrop_init (PnFileDrop *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.36, 0.60, 0.74, 1.0 };

    self->pixbuf        = NULL;
    self->last_filename = NULL;
    self->area_color    = (GdkRGBA){ 1.0, 1.0, 1.0, 1.0 };   /* white */
    self->border_color  = (GdkRGBA){ 70.0/255.0, 70.0/255.0, 70.0/255.0, 1.0 };

    pn_node_set_class_name (node, "FileDrop");
    pn_node_set_icon       (node, "\xef\x80\x9c");  /* fa-inbox U+F01C */
    pn_node_set_color (node, (const PnColor *)&color);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFileDrop *
pn_filedrop_new (void)
{
    return g_object_new (PN_TYPE_FILEDROP, NULL);
}
