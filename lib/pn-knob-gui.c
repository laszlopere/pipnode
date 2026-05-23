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
/*  PnKnob — gui tier.                                                  */
/*                                                                     */
/*  The cairo dial header decoration for the Knob node.  The node's     */
/*  GType, range + value properties, wheel-rotation emit and startup-   */
/*  announce logic live in the GTK-free core file pn-knob.c; this file  */
/*  installs the drawing vfunc slots onto that class at editor startup  */
/*  (pn_knob_gui_install), reading the pointer position through the      */
/*  core's GTK-free accessor pn_knob_get_value_fraction().  The         */
/*  headless runtime never loads this file's half, so the Knob logic    */
/*  runs without GTK.                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-knob-gui.h"
#include "pn-knob.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The dial sits on the right edge of the header.  The radius and      */
/*  right pad mirror the values the core file uses for its hit-test     */
/*  (pn_knob_hit_knob); the reserved label margin and the pointer       */
/*  sweep angles live here with the painter that defines them.          */
/* ------------------------------------------------------------------ */

#define PN_KNOB_RIGHT_PAD        12.0
#define PN_KNOB_RADIUS           13.0

#define PN_KNOB_RESERVED_RIGHT  (2.0 * PN_KNOB_RADIUS + PN_KNOB_RIGHT_PAD + 6.0)

/* The knob's pointer sweeps 270 degrees with the dead zone at the
 * bottom: minimum parks the indicator at the 7-o'clock position,
 * maximum at 5-o'clock, passing straight up (12-o'clock) at the
 * midpoint.  Expressed as cairo angles (0 = +x, growing clockwise on
 * screen because y points down): the sweep runs from 135 degrees
 * clockwise through the top to 135+270 degrees. */
#define PN_KNOB_SWEEP_START     (0.75 * G_PI)
#define PN_KNOB_SWEEP_EXTENT    (1.5  * G_PI)

/* ------------------------------------------------------------------ */
/*  Painting                                                            */
/* ------------------------------------------------------------------ */

/** Paint a rotary knob -- a recessed sweep track, a domed body lit
 *  from the upper-left, and a pointer line marking the current
 *  position.  Drawn concentric on (@cx, @cy) with radius @r, with the
 *  pointer at normalised position @t in [0, 1]. */
static void
paint_knob (
        cairo_t *cr,
        double   cx,
        double   cy,
        double   r,
        double   t)
{
    const double track_r  = r + 2.0;
    const double angle     = PN_KNOB_SWEEP_START + t * PN_KNOB_SWEEP_EXTENT;
    const double pointer_r = r - 3.0;

    /* Sweep track -- a thin dark arc behind the body marking the range
     * of travel, with the dead zone left open at the bottom so the
     * knob reads as a real rotary control with end stops. */
    cairo_set_line_width (cr, 2.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
    cairo_new_sub_path (cr);
    cairo_arc (cr, cx, cy, track_r,
               PN_KNOB_SWEEP_START,
               PN_KNOB_SWEEP_START + PN_KNOB_SWEEP_EXTENT);
    cairo_stroke (cr);

    /* Body -- a near-white disc with a radial gradient lit from the
     * upper-left so it reads as a physical domed knob, matching the
     * PnSwitch thumb's material. */
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial (
                cx - r * 0.30, cy - r * 0.30, r * 0.10,
                cx, cy, r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 1.00, 1.00, 1.00);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.78, 0.78, 0.80);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Rim hairline seals the body against the track. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, cx, cy, r - 0.4, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);

    /* Pointer -- a short stroke from near the centre out to the rim,
     * marking the current position.  Capped round so it reads as a
     * moulded indicator notch rather than a bare line. */
    cairo_set_source_rgb (cr, 0.16, 0.18, 0.20);
    cairo_set_line_width (cr, 2.4);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr,
                   cx + cos (angle) * (r * 0.30),
                   cy + sin (angle) * (r * 0.30));
    cairo_line_to (cr,
                   cx + cos (angle) * pointer_r,
                   cy + sin (angle) * pointer_r);
    cairo_stroke (cr);
}

static void
pn_knob_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnKnob      *self = PN_KNOB (node);
    const double cx   = x + w - PN_KNOB_RIGHT_PAD - PN_KNOB_RADIUS;
    const double cy   = y + h * 0.5;

    paint_knob (cr, cx, cy, PN_KNOB_RADIUS, pn_knob_get_value_fraction (self));
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_knob_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_KNOB));

    node_class->paint_header_overlay         = pn_knob_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_KNOB_RESERVED_RIGHT;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
