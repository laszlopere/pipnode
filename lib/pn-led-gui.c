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
/*  PnLed — gui tier.                                                   */
/*                                                                     */
/*  The cairo header decoration and the settings-dialog customisation  */
/*  for the LED node.  The node's GType, properties and receive() logic */
/*  live in the GTK-free core file pn-led.c; this file installs the     */
/*  drawing + dialog vfunc slots onto that class at editor startup      */
/*  (pn_led_gui_install), reading the lit state through the core's      */
/*  GTK-free accessors.  The headless runtime never loads this file's   */
/*  half, so the LED logic runs without GTK.                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-led-gui.h"
#include "pn-led.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The LED disc is drawn by paint_header_overlay, which clips to the  */
/*  body's rounded rect so the LED respects the corner radius on the   */
/*  right edge.  The disc is circular (diameter PN_LED_DIAMETER),       */
/*  centred vertically in the header and sitting PN_LED_RIGHT_PAD       */
/*  pixels clear of the right edge.                                     */
/* ------------------------------------------------------------------ */

#define PN_LED_RIGHT_PAD          12.0     /* clear of right edge */
#define PN_LED_DIAMETER           24.0     /* outer bracket diameter */

/* The label painter is told to leave the rightmost slice of the     */
/* header alone -- LED diameter + right pad + a small breathing gap  */
/* so the centred label cannot bump into the LED bracket.            */
#define PN_LED_RESERVED_RIGHT    (PN_LED_DIAMETER + PN_LED_RIGHT_PAD + 6.0)

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Paint a circular through-hole LED sitting in a black panel-mount
 *  bracket -- the kind a real 5 mm indicator LED clips into on the
 *  front of a piece of equipment.  The whole assembly is concentric
 *  on (@cx, @cy) with overall radius @outer_r:
 *
 *    - When lit, a soft halo of the LED's colour spills out around
 *      the bracket first so the bracket overpaints (and softly trims)
 *      its outer edge.
 *    - The bracket is a thick black ring with a tiny lighter highlight
 *      on the upper-left lip so it reads as a chunky moulded bezel
 *      rather than a flat ring of paint.
 *    - The dome inside is a coloured circle with a radial gradient
 *      from a bright centre to a darker rim (user colour when lit,
 *      two greys when off).  A thin dark hairline just inside the
 *      bracket sells the "recessed into the bracket" look.
 *    - A small bright catch-light in the upper-left of the dome adds
 *      the obligatory glass highlight every real LED window picks up.
 */
static void
paint_led (
        cairo_t       *cr,
        double         cx,
        double         cy,
        double         outer_r,
        gboolean       lit,
        const PnColor *color)
{
    /* Bracket takes the outer ~22 % of the radius -- thick enough to
     * read as a moulded mounting collar at the small sizes we paint
     * at, thin enough that the coloured dome still dominates the
     * read. */
    const double     bracket_r = outer_r;
    const double     dome_r    = outer_r * 0.78;
    cairo_pattern_t *grad;

    /* Halo: only when lit.  A soft radial gradient centred on the LED
     * with the user's colour at the centre fading to transparent at
     * about 2 r, painted before the bracket so the bracket sits on
     * top of (and softly trims) the halo's outer edge.               */
    if (lit)
    {
        const double halo_r = outer_r * 2.1;

        grad = cairo_pattern_create_radial (cx, cy, outer_r * 0.7,
                                            cx, cy, halo_r);
        cairo_pattern_add_color_stop_rgba (grad, 0.0,
                                           color->red,
                                           color->green,
                                           color->blue,
                                           0.55);
        cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                           color->red,
                                           color->green,
                                           color->blue,
                                           0.0);
        cairo_set_source (cr, grad);
        cairo_rectangle (cr, cx - halo_r, cy - halo_r,
                         halo_r * 2.0, halo_r * 2.0);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Bracket body: a black disc filling the full outer radius.  The
     * dome painted on top of this leaves the outer ring of the disc
     * visible as the bracket's mounting collar.                      */
    cairo_set_source_rgb (cr, 0.05, 0.05, 0.06);
    cairo_arc (cr, cx, cy, bracket_r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Bracket highlight: a thin lighter arc on the upper-left rim of
     * the bracket so the bezel reads as a chamfered lip catching
     * overhead light.  A linear gradient from a mid-grey at the top
     * to near-black at the bottom, stroked along the outer edge,
     * gives the bracket its sense of thickness without an explicit
     * second concentric disc. */
    grad = cairo_pattern_create_linear (cx, cy - bracket_r,
                                        cx, cy + bracket_r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.40, 0.40, 0.42);
    cairo_pattern_add_color_stop_rgb (grad, 0.5, 0.12, 0.12, 0.13);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.22, 0.22, 0.24);
    cairo_set_source (cr, grad);
    cairo_set_line_width (cr, 1.2);
    cairo_arc (cr, cx, cy, bracket_r - 0.6, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);
    cairo_pattern_destroy (grad);

    /* Dome: a coloured radial gradient inside the bracket.  Painted
     * as an arc fill so the boundary against the black bracket is a
     * crisp circular edge (a radial-gradient pattern applied to a
     * rectangle would still leave the rectangle outside the dome
     * showing on top of the bracket). */
    {
        double cr_r, cg, cb;
        double or_r, og, ob;

        if (lit)
        {
            /* Lit: bright user colour at the centre, darker ring at
             * the rim -- the standard "shiny LED" read. */
            cr_r = color->red   * 0.40 + 0.60;
            cg   = color->green * 0.40 + 0.60;
            cb   = color->blue  * 0.40 + 0.60;
            or_r = color->red   * 0.85;
            og   = color->green * 0.85;
            ob   = color->blue  * 0.85;
        }
        else
        {
            /* Off: two greys, lighter centre to darker rim, reading
             * as a translucent dome with no light behind it. */
            cr_r = 0.72; cg = 0.72; cb = 0.74;
            or_r = 0.30; og = 0.30; ob = 0.32;
        }

        grad = cairo_pattern_create_radial (cx - dome_r * 0.20,
                                            cy - dome_r * 0.20,
                                            dome_r * 0.05,
                                            cx, cy, dome_r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, cr_r, cg, cb);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, or_r, og, ob);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Hairline shadow where the dome meets the bracket -- a thin
     * stroke just inside the dome's edge, in translucent black, sells
     * the "dome recessed into the bracket" read by suggesting the
     * tiny dark gap a real moulded part has at that seam. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, cx, cy, dome_r - 0.4, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);

    /* Small bright catch-light in the upper-left of the dome -- sells
     * the 3D read even when the LED is off, since real LED windows
     * always pick up an ambient highlight on the glass.  Clipped to
     * the dome so it does not bleed onto the bracket. */
    {
        const double hl_r = dome_r * 0.55;

        cairo_save (cr);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_clip (cr);

        grad = cairo_pattern_create_radial (cx - dome_r * 0.30,
                                            cy - dome_r * 0.30,
                                            0.0,
                                            cx - dome_r * 0.30,
                                            cy - dome_r * 0.30,
                                            hl_r);
        cairo_pattern_add_color_stop_rgba (grad, 0.0, 1.0, 1.0, 1.0,
                                           lit ? 0.65 : 0.45);
        cairo_pattern_add_color_stop_rgba (grad, 1.0, 1.0, 1.0, 1.0, 0.0);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);

        cairo_restore (cr);
    }
}

static void
pn_led_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnLed       *self = PN_LED (node);
    const double r    = PN_LED_DIAMETER * 0.5;
    const double cx   = x + w - PN_LED_RIGHT_PAD - r;
    const double cy   = y + h * 0.5;
    PnColor      color;

    pn_led_peek_color (self, &color);
    paint_led (cr, cx, cy, r, pn_led_get_lit (self), &color);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog                                                    */
/*                                                                     */
/*  The `hold-ms` row's "sensitive only when mode == Flash" rule is    */
/*  now a GTK-free enable-when in the core pn-led.c PnSettingsSchema    */
/*  (Phase 7.5); the dialog framework's renderer applies it.  This     */
/*  file is the LED's painter only.                                    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_led_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_LED));

    node_class->paint_header_overlay         = pn_led_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_LED_RESERVED_RIGHT;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
