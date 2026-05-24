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
/*  PnLedLamp — a panel-sized circular indicator lamp                   */
/*                                                                     */
/*  A miniature of the LED node's header indicator: a through-hole LED  */
/*  in a black panel-mount bracket, a coloured glass dome with a        */
/*  catch-light, and (while lit) a soft colour halo around the bracket. */
/*  The painter is a port of pn-led-gui.c's paint_led, restated with    */
/*  plain RGB doubles instead of a node #PnColor so the applet never    */
/*  links a pipnode library.                                           */
/* ------------------------------------------------------------------ */

#include "pn-led-lamp.h"

#include <math.h>

/* The bracket+dome occupy this fraction of the widget's half-size; the
 * remaining margin gives the lit halo room to glow before the widget edge
 * clips it (the halo fades to transparent, so the clip is soft). */
#define LAMP_BRACKET_RATIO 0.72

/* A hair of inset so the halo and collar never sit flush against the edge. */
#define LAMP_EDGE_PAD 2.0

struct _PnLedLamp
{
    GtkDrawingArea parent_instance;

    gboolean lit;                 /* whether the dome is glowing          */
    gdouble  red, green, blue;    /* lit colour, 0..1                     */
    gint     size;                /* requested overall pixel size (square) */
};

G_DEFINE_TYPE (PnLedLamp, pn_led_lamp, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Painting (ported from pn-led-gui.c's paint_led)                     */
/* ------------------------------------------------------------------ */

/* Paint a circular through-hole LED in a black panel-mount bracket,
 * concentric on (@cx,@cy) with overall bracket radius @outer_r.  See
 * pn-led-gui.c for the anatomy; this is the same routine with the node's
 * #PnColor replaced by plain @r/@g/@b doubles. */
static void
paint_lamp (cairo_t *cr,
            double   cx,
            double   cy,
            double   outer_r,
            gboolean lit,
            double   r,
            double   g,
            double   b)
{
    const double     bracket_r = outer_r;
    const double     dome_r    = outer_r * 0.78;
    cairo_pattern_t *grad;

    /* Halo: only when lit.  A soft radial gradient in the lamp's colour,
     * painted before the bracket so the bracket trims its outer edge. */
    if (lit)
    {
        const double halo_r = outer_r * 2.1;

        grad = cairo_pattern_create_radial (cx, cy, outer_r * 0.7,
                                            cx, cy, halo_r);
        cairo_pattern_add_color_stop_rgba (grad, 0.0, r, g, b, 0.55);
        cairo_pattern_add_color_stop_rgba (grad, 1.0, r, g, b, 0.0);
        cairo_set_source (cr, grad);
        cairo_rectangle (cr, cx - halo_r, cy - halo_r,
                         halo_r * 2.0, halo_r * 2.0);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Bracket body: a black disc; the dome on top leaves its outer ring
     * showing as the mounting collar. */
    cairo_set_source_rgb (cr, 0.05, 0.05, 0.06);
    cairo_arc (cr, cx, cy, bracket_r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Bracket highlight: a thin lighter arc on the upper-left rim so the
     * bezel reads as a chamfered lip catching overhead light. */
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

    /* Dome: a coloured radial gradient inside the bracket, filled as an
     * arc so the boundary against the black bracket is a crisp edge. */
    {
        double cr_r, cg, cb;
        double or_r, og, ob;

        if (lit)
        {
            cr_r = r * 0.40 + 0.60;
            cg   = g * 0.40 + 0.60;
            cb   = b * 0.40 + 0.60;
            or_r = r * 0.85;
            og   = g * 0.85;
            ob   = b * 0.85;
        }
        else
        {
            /* Off: two greys, lighter centre to darker rim. */
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

    /* Hairline shadow where the dome meets the bracket. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, cx, cy, dome_r - 0.4, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);

    /* Small bright catch-light in the upper-left of the dome, clipped to
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

static gboolean
pn_led_lamp_draw (GtkWidget *widget, cairo_t *cr)
{
    PnLedLamp    *self = PN_LED_LAMP (widget);
    GtkAllocation alloc;
    double        cx, cy, outer_r;

    gtk_widget_get_allocation (widget, &alloc);

    /* No frame or background: the lamp is drawn straight onto the
     * transparent allocation, so the panel shows through behind it. */
    cx      = alloc.width  * 0.5;
    cy      = alloc.height * 0.5;
    /* Keep a hair of margin so the lit halo and collar never touch the edge. */
    outer_r = (MIN (alloc.width, alloc.height) - 2.0 * LAMP_EDGE_PAD)
              * 0.5 * LAMP_BRACKET_RATIO;
    if (outer_r < 2.0)
        outer_r = 2.0;

    paint_lamp (cr, cx, cy, outer_r, self->lit,
                self->red, self->green, self->blue);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_led_lamp_get_preferred_width (GtkWidget *widget,
                                 gint      *minimum,
                                 gint      *natural)
{
    *minimum = *natural = PN_LED_LAMP (widget)->size;
}

static void
pn_led_lamp_get_preferred_height (GtkWidget *widget,
                                  gint      *minimum,
                                  gint      *natural)
{
    *minimum = *natural = PN_LED_LAMP (widget)->size;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_led_lamp_class_init (PnLedLampClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->draw                 = pn_led_lamp_draw;
    widget_class->get_preferred_width  = pn_led_lamp_get_preferred_width;
    widget_class->get_preferred_height = pn_led_lamp_get_preferred_height;
}

static void
pn_led_lamp_init (PnLedLamp *self)
{
    self->lit   = FALSE;
    /* A bright green by default, the canonical "OK / activity" indicator
     * and the LED node's own default lit colour. */
    self->red   = 0.20;
    self->green = 0.85;
    self->blue  = 0.30;
    self->size  = 24;
    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_led_lamp_new (void)
{
    return g_object_new (PN_TYPE_LED_LAMP, NULL);
}

void
pn_led_lamp_set_lit (PnLedLamp *self, gboolean lit)
{
    g_return_if_fail (PN_IS_LED_LAMP (self));

    lit = !!lit;
    if (lit == self->lit)
        return;

    self->lit = lit;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_led_lamp_set_color (PnLedLamp *self,
                       gdouble    red,
                       gdouble    green,
                       gdouble    blue)
{
    g_return_if_fail (PN_IS_LED_LAMP (self));

    red   = CLAMP (red,   0.0, 1.0);
    green = CLAMP (green, 0.0, 1.0);
    blue  = CLAMP (blue,  0.0, 1.0);

    if (red == self->red && green == self->green && blue == self->blue)
        return;

    self->red   = red;
    self->green = green;
    self->blue  = blue;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_led_lamp_set_size (PnLedLamp *self, gint size)
{
    g_return_if_fail (PN_IS_LED_LAMP (self));

    if (size < 8)
        size = 8;
    if (size == self->size)
        return;

    self->size = size;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}
