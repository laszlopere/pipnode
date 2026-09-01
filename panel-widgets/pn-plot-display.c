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
/*  PnPlotDisplay — a plot area blitted from a picture the engine sent  */
/*                                                                     */
/*  Unlike its siblings this widget restates nothing: it decodes a      */
/*  base64 PNG out of the widget state and paints it.  The reason is in */
/*  the header — a graph's look lives in PLplot / MathGL / Pango, which */
/*  this library may not link, so the one process that CAN draw it (the */
/*  engine, which owns the nodes) draws it and sends the pixels.        */
/*                                                                     */
/*  The picture is kept as a cairo image surface rather than a pixbuf   */
/*  so painting is a plain set_source_surface with no per-frame         */
/*  conversion, and it is re-decoded only when the encoded string       */
/*  actually changes — an engine that re-sends an unchanged plot costs  */
/*  one strcmp.                                                        */
/* ------------------------------------------------------------------ */

#include "pn-plot-display.h"

/* Fallback area before the layout says otherwise: the same default a
 * freshly-placed plot gets in the layout editor. */
#define PLOT_DEFAULT_WIDTH   260
#define PLOT_DEFAULT_HEIGHT  170

/* Nothing smaller is worth drawing; also stops a malformed state from
 * requesting a zero- or negative-sized widget. */
#define PLOT_MIN_SIDE  16

struct _PnPlotDisplay
{
    GtkDrawingArea parent_instance;

    gint width;
    gint height;

    /* The last picture, and the exact string it was decoded from (so an
     * unchanged re-send skips the decode).  Both NULL until one arrives. */
    cairo_surface_t *surface;
    gchar           *encoded;
};

G_DEFINE_TYPE (PnPlotDisplay, pn_plot_display, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Decoding                                                            */
/* ------------------------------------------------------------------ */

/* Decode @png_base64 into a fresh image surface, or %NULL when it is
 * empty or not a picture we can read.  Goes through GdkPixbuf (already a
 * hard GTK dependency) rather than cairo's PNG reader, which only reads
 * from a file or a stream callback. */
static cairo_surface_t *
decode_png_base64 (const gchar *png_base64)
{
    guchar          *raw;
    gsize            raw_len = 0;
    GdkPixbufLoader *loader;
    GdkPixbuf       *pixbuf;
    cairo_surface_t *surface = NULL;

    if (png_base64 == NULL || *png_base64 == '\0')
        return NULL;

    raw = g_base64_decode (png_base64, &raw_len);
    if (raw == NULL || raw_len == 0)
    {
        g_free (raw);
        return NULL;
    }

    loader = gdk_pixbuf_loader_new ();
    if (gdk_pixbuf_loader_write (loader, raw, raw_len, NULL)
        && gdk_pixbuf_loader_close (loader, NULL))
    {
        pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
        if (pixbuf != NULL)
            surface = gdk_cairo_surface_create_from_pixbuf (pixbuf, 1, NULL);
    }
    else
    {
        /* close() must still be called on a loader that failed mid-write,
         * or it warns on finalize. */
        gdk_pixbuf_loader_close (loader, NULL);
    }

    g_object_unref (loader);
    g_free (raw);
    return surface;
}

/* ------------------------------------------------------------------ */
/*  Painting                                                            */
/* ------------------------------------------------------------------ */

/* The empty state: a faint framed rectangle the size of the plot, so a
 * graph with no data yet reads as a reserved area rather than a hole. */
static void
paint_placeholder (cairo_t *cr, double w, double h)
{
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.10);
    cairo_rectangle (cr, 0.0, 0.0, w, h);
    cairo_fill (cr);

    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.55);
    cairo_rectangle (cr, 0.5, 0.5, w - 1.0, h - 1.0);
    cairo_stroke (cr);
}

static gboolean
pn_plot_display_draw (GtkWidget *widget, cairo_t *cr)
{
    PnPlotDisplay *self = PN_PLOT_DISPLAY (widget);
    double         w    = gtk_widget_get_allocated_width  (widget);
    double         h    = gtk_widget_get_allocated_height (widget);
    double         iw, ih;

    if (self->surface == NULL)
    {
        paint_placeholder (cr, w, h);
        return FALSE;
    }

    iw = cairo_image_surface_get_width  (self->surface);
    ih = cairo_image_surface_get_height (self->surface);
    if (iw <= 0.0 || ih <= 0.0)
    {
        paint_placeholder (cr, w, h);
        return FALSE;
    }

    /* The engine renders at the layout's size, so normally this is a 1:1
     * blit.  Scale anyway for the window in between a resize and the next
     * picture, where a stale image would otherwise sit in a corner. */
    cairo_save (cr);
    if (iw != w || ih != h)
        cairo_scale (cr, w / iw, h / ih);
    cairo_set_source_surface (cr, self->surface, 0.0, 0.0);
    cairo_paint (cr);
    cairo_restore (cr);

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Sizing                                                             */
/* ------------------------------------------------------------------ */

static void
pn_plot_display_get_preferred_width (GtkWidget *widget,
                                     gint      *minimum,
                                     gint      *natural)
{
    *minimum = *natural = PN_PLOT_DISPLAY (widget)->width;
}

static void
pn_plot_display_get_preferred_height (GtkWidget *widget,
                                      gint      *minimum,
                                      gint      *natural)
{
    *minimum = *natural = PN_PLOT_DISPLAY (widget)->height;
}

/* ------------------------------------------------------------------ */
/*  GObject / GTK boilerplate                                          */
/* ------------------------------------------------------------------ */

static void
pn_plot_display_finalize (GObject *object)
{
    PnPlotDisplay *self = PN_PLOT_DISPLAY (object);

    g_clear_pointer (&self->surface, cairo_surface_destroy);
    g_clear_pointer (&self->encoded, g_free);

    G_OBJECT_CLASS (pn_plot_display_parent_class)->finalize (object);
}

static void
pn_plot_display_class_init (PnPlotDisplayClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize = pn_plot_display_finalize;

    widget_class->draw                 = pn_plot_display_draw;
    widget_class->get_preferred_width  = pn_plot_display_get_preferred_width;
    widget_class->get_preferred_height = pn_plot_display_get_preferred_height;
}

static void
pn_plot_display_init (PnPlotDisplay *self)
{
    self->width  = PLOT_DEFAULT_WIDTH;
    self->height = PLOT_DEFAULT_HEIGHT;
    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_plot_display_new (void)
{
    return g_object_new (PN_TYPE_PLOT_DISPLAY, NULL);
}

void
pn_plot_display_set_size (PnPlotDisplay *self, gint width, gint height)
{
    g_return_if_fail (PN_IS_PLOT_DISPLAY (self));

    if (width <= 0 || height <= 0)
        return;

    width  = MAX (width,  PLOT_MIN_SIDE);
    height = MAX (height, PLOT_MIN_SIDE);

    if (width == self->width && height == self->height)
        return;

    self->width  = width;
    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
pn_plot_display_set_png_base64 (PnPlotDisplay *self, const gchar *png_base64)
{
    g_return_if_fail (PN_IS_PLOT_DISPLAY (self));

    if (png_base64 == NULL)
        png_base64 = "";

    /* Identical picture re-sent: nothing to decode and nothing to
     * repaint.  The engine coalesces, but a layout re-publish still
     * carries every widget's current state. */
    if (g_strcmp0 (png_base64, self->encoded != NULL ? self->encoded : "") == 0)
        return;

    g_free (self->encoded);
    self->encoded = g_strdup (png_base64);

    g_clear_pointer (&self->surface, cairo_surface_destroy);
    self->surface = decode_png_base64 (png_base64);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}
