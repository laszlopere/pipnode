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
/*  PnFileViewer — gui tier.                                            */
/*                                                                     */
/*  The cairo preview painter for the FileViewer sink node.  The node's */
/*  GType, properties, intrinsic size and receive() logic live in the   */
/*  GTK-free core file pn-file-viewer.c; this file installs the          */
/*  paint_plot vfunc slot (and its companion zoom-keep-aspect flag) onto */
/*  that class at editor startup (pn_file_viewer_gui_install), reading   */
/*  the node's preview state through the GTK-free                        */
/*  pn_file_viewer_get_paint_state() snapshot.  The headless runtime     */
/*  never loads this half, so the FileViewer logic runs without GTK.     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-file-viewer-gui.h"
#include "pn-file-viewer.h"

#include <gtk/gtk.h>

/* Inset used only by the empty-state hint (dashed box + label) so the
 * placeholder does not butt right up against the frame.  A shown image
 * deliberately uses no inset — it runs to the frame. */
#define PN_FILE_VIEWER_PADDING            6.0

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
 *  the same.  All preview state is read through the GTK-free
 *  pn_file_viewer_get_paint_state() snapshot; the PnColor fields cast to
 *  GdkRGBA* for gdk_cairo (layout-identical by design — see pn-color.h). */
static void
pn_file_viewer_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnFileViewer           *self = PN_FILE_VIEWER (node);
    PnFileViewerPaintState  st;

    pn_file_viewer_get_paint_state (self, &st);

    /* Base fill.  When an image is present it is painted over this
     * edge-to-edge, so the area colour only ever shows in the empty
     * state. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &st.area_color);
    cairo_fill (cr);
    cairo_restore (cr);

    if (st.pixbuf != NULL)
    {
        /* Fill the whole rectangle with the image — no padding, no
         * white margin; the rectangle already carries the image's
         * aspect ratio, so the picture covers it exactly. */
        cairo_save (cr);
        cairo_rectangle (cr, x, y, w, h);
        cairo_clip (cr);
        paint_pixbuf_cover (cr, st.pixbuf, x, y, w, h);
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
                             st.last_filename != NULL
                                 ? st.last_filename
                                 : "Nothing to show",
                             cx, cy, cw, ch);
    }

    /* Frame last, so it edges the image (or the empty area) cleanly on
     * top of whatever was drawn. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &st.border_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_file_viewer_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_FILE_VIEWER));

    node_class->paint_plot = pn_file_viewer_paint_plot;
    /* A primary press on the view area lifts it into the centred zoom
     * overlay, the same gesture that enlarges a Graph's plot; click the
     * enlarged rectangle to drop it back.  Keep the preview's aspect
     * ratio when enlarged so the maximised image is not stretched. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
