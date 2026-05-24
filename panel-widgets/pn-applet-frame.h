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
/*  Shared panel-applet widget frame                                   */
/*                                                                     */
/*  Every panel-applet face — the seven-segment readout, the indicator */
/*  lamp, and whatever comes next — sits on the same dark-brown panel   */
/*  with a raised bezel rim, so a row of mixed applets reads as one     */
/*  instrument cluster rather than a jumble of loose widgets.  The look */
/*  is identical whether the widget is shown on the panel-editor canvas */
/*  or embedded in the real xfce4-panel, because each widget paints     */
/*  this frame itself in its draw handler.                              */
/*                                                                     */
/*  Cairo-only and header-inline so the convenience library carries no  */
/*  new object and the applet still links no pipnode library.           */
/* ------------------------------------------------------------------ */

#ifndef PN_APPLET_FRAME_H
#define PN_APPLET_FRAME_H

#include <cairo.h>
#include <glib.h>
#include <math.h>

G_BEGIN_DECLS

/* The bezel rim thickness, in pixels, and the content inset a widget
 * should keep clear of the outer edge so its face never touches the rim. */
#define PN_APPLET_FRAME_BORDER 1.5
#define PN_APPLET_FRAME_PAD    2.0
#define PN_APPLET_FRAME_RADIUS 4.0

/* The dark-brown face the applet content is drawn on. */
#define PN_APPLET_FRAME_BG_R 0.16
#define PN_APPLET_FRAME_BG_G 0.10
#define PN_APPLET_FRAME_BG_B 0.06

/* The lighter warm-brown rim, read as a chamfered bezel around the face. */
#define PN_APPLET_FRAME_BORDER_R 0.38
#define PN_APPLET_FRAME_BORDER_G 0.26
#define PN_APPLET_FRAME_BORDER_B 0.15

/* Trace a rounded-rectangle path with corner radius @r, clamped so it
 * never exceeds half of either side. */
static inline void
pn_applet_frame_rounded_rect (cairo_t *cr,
                              double   x,
                              double   y,
                              double   w,
                              double   h,
                              double   r)
{
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -M_PI_2, 0.0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0.0,     M_PI_2);
    cairo_arc (cr, x + r,     y + h - r, r, M_PI_2,  M_PI);
    cairo_arc (cr, x + r,     y + r,     r, M_PI,    1.5 * M_PI);
    cairo_close_path (cr);
}

/* Paint the shared applet frame over the whole @width x @height
 * allocation: the bezel rim first, then the dark-brown face inset by the
 * rim width.  The caller then draws its content on top, keeping it within
 * PN_APPLET_FRAME_PAD of the edge. */
static inline void
pn_applet_frame_draw (cairo_t *cr, double width, double height)
{
    pn_applet_frame_rounded_rect (cr, 0.0, 0.0, width, height,
                                  PN_APPLET_FRAME_RADIUS);
    cairo_set_source_rgb (cr, PN_APPLET_FRAME_BORDER_R,
                          PN_APPLET_FRAME_BORDER_G,
                          PN_APPLET_FRAME_BORDER_B);
    cairo_fill (cr);

    pn_applet_frame_rounded_rect (cr,
                                  PN_APPLET_FRAME_BORDER,
                                  PN_APPLET_FRAME_BORDER,
                                  width  - 2.0 * PN_APPLET_FRAME_BORDER,
                                  height - 2.0 * PN_APPLET_FRAME_BORDER,
                                  PN_APPLET_FRAME_RADIUS - PN_APPLET_FRAME_BORDER);
    cairo_set_source_rgb (cr, PN_APPLET_FRAME_BG_R,
                          PN_APPLET_FRAME_BG_G,
                          PN_APPLET_FRAME_BG_B);
    cairo_fill (cr);
}

G_END_DECLS

#endif /* PN_APPLET_FRAME_H */
