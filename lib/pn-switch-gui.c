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
/*  PnSwitch — gui tier.                                                */
/*                                                                     */
/*  The cairo slider header decoration for the Switch node.  The node's */
/*  GType, property, receive() latch and startup-announce logic live in */
/*  the GTK-free core file pn-switch.c; this file installs the drawing  */
/*  vfunc slots onto that class at editor startup                       */
/*  (pn_switch_gui_install), reading the latch state through the core's */
/*  GTK-free accessor pn_switch_get_on().  The headless runtime never   */
/*  loads this file's half, so the Switch logic runs without GTK.       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-switch-gui.h"
#include "pn-switch.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The slider decoration sits on the right edge of the header.  The   */
/*  track width / height and right pad mirror the values the core file  */
/*  uses for its hit-test (pn_switch_hit_slider); the reserved label    */
/*  margin lives here with the painter that defines it.                 */
/* ------------------------------------------------------------------ */

#define PN_SWITCH_RIGHT_PAD        10.0
#define PN_SWITCH_TRACK_WIDTH      38.0
#define PN_SWITCH_TRACK_HEIGHT     20.0

#define PN_SWITCH_RESERVED_RIGHT  (PN_SWITCH_TRACK_WIDTH + PN_SWITCH_RIGHT_PAD + 6.0)

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Trace a horizontal pill (rounded-end rectangle) so the switch
 *  track and thumb share a consistent silhouette. */
static void
pill_path (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    const double r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r, r, -G_PI_2,  G_PI_2);
    cairo_arc (cr, x + r,     y + r, r,  G_PI_2,  3.0 * G_PI_2);
    cairo_close_path (cr);
}

/** Paint a slide switch -- a rounded "track" with a circular "thumb"
 *  parked at one end.  On state shows the thumb on the right with a
 *  green track; off state parks the thumb on the left with a dark
 *  grey track.  Painted concentric on (@cx, @cy) with the configured
 *  width / height. */
static void
paint_switch (
        cairo_t *cr,
        double   cx,
        double   cy,
        double   w,
        double   h,
        gboolean on)
{
    const double tx = cx - w * 0.5;
    const double ty = cy - h * 0.5;
    const double thumb_r = h * 0.5 - 2.0;
    const double thumb_cx = on ? (tx + w - h * 0.5)
                               : (tx + h * 0.5);

    /* Track fill.  Green when on (canonical "switch engaged"), neutral
     * dark grey when off so the on-state pops against the surrounding
     * teal body colour. */
    pill_path (cr, tx, ty, w, h);
    if (on)
        cairo_set_source_rgb (cr, 0.25, 0.72, 0.35);
    else
        cairo_set_source_rgb (cr, 0.28, 0.30, 0.32);
    cairo_fill_preserve (cr);

    /* Track outline -- a thin dark ring so the pill reads as a real
     * moulded part rather than a flat coloured patch. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Inset highlight on the upper rim sells the chamfered bevel the
     * surrounding node body uses, so the switch reads as built into
     * the panel rather than glued on top. */
    cairo_save (cr);
    pill_path (cr, tx, ty, w, h);
    cairo_clip (cr);
    pill_path (cr, tx + 1.0, ty + 1.0, w - 2.0, h - 2.0);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Thumb -- a near-white disc, lit from the upper-left with a
     * subtle radial gradient so it reads as a physical knob.  A thin
     * dark hairline just inside the rim seals it against the track. */
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial (
                thumb_cx - thumb_r * 0.30,
                cy        - thumb_r * 0.30,
                thumb_r * 0.10,
                thumb_cx, cy, thumb_r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 1.00, 1.00, 1.00);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.80, 0.80, 0.82);
        cairo_set_source (cr, grad);
        cairo_arc (cr, thumb_cx, cy, thumb_r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);

        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
        cairo_set_line_width (cr, 0.8);
        cairo_arc (cr, thumb_cx, cy, thumb_r - 0.4, 0.0, 2.0 * G_PI);
        cairo_stroke (cr);
    }
}

static void
pn_switch_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnSwitch    *self = PN_SWITCH (node);
    const double sw   = PN_SWITCH_TRACK_WIDTH;
    const double sh   = PN_SWITCH_TRACK_HEIGHT;
    const double cx   = x + w - PN_SWITCH_RIGHT_PAD - sw * 0.5;
    const double cy   = y + h * 0.5;

    paint_switch (cr, cx, cy, sw, sh, pn_switch_get_on (self));
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_switch_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_SWITCH));

    node_class->paint_header_overlay         = pn_switch_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_SWITCH_RESERVED_RIGHT;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
