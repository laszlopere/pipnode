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
/*  PnOscilloscope — gui tier.                                         */
/*                                                                     */
/*  A pure-cairo painter that dresses the node's footprint as an old   */
/*  square-screen, green-phosphor CRT scope: a moulded bezel, a dark    */
/*  phosphor screen with a corner vignette, an etched 10×8 division     */
/*  graticule, and the auto-fitted trace drawn as a glowing beam.  All  */
/*  the data — the trace points and the auto-range bounds — comes from  */
/*  the GTK-free core (pn-oscilloscope.c) through its read seam, so the  */
/*  headless runtime never loads this half and the scope's logic runs   */
/*  without GTK or cairo.                                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-oscilloscope-gui.h"
#include "pn-oscilloscope.h"

#include <gtk/gtk.h>
#include <math.h>

/* Reference screen height the at-rest stroke widths are authored for;
 * the painter scales line widths by the actual short side so the beam
 * keeps its weight when the node is lifted into the zoom overlay. */
#define PN_OSC_REF_SHORT_SIDE  210.0

/* Graticule divisions, the classic scope ratio. */
#define PN_OSC_DIV_X  10
#define PN_OSC_DIV_Y   8

/* ------------------------------------------------------------------ */
/*  Small cairo helpers                                                */
/* ------------------------------------------------------------------ */

static void
set_rgba (
        cairo_t       *cr,
        const PnColor *c,
        double         alpha)
{
    cairo_set_source_rgba (cr, c->red, c->green, c->blue, alpha);
}

static void
rounded_rect (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   r)
{
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -G_PI / 2.0, 0.0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0.0,          G_PI / 2.0);
    cairo_arc (cr, x + r,     y + h - r, r, G_PI / 2.0,   G_PI);
    cairo_arc (cr, x + r,     y + r,     r, G_PI,         3.0 * G_PI / 2.0);
    cairo_close_path (cr);
}

/** Pad a [lo, hi] range so the data never touches the frame; widen a
 *  degenerate (flat) range to something drawable.  When @from_zero the
 *  range is first anchored at zero.  Mirrors pn-xy-graph-gui.c. */
static void
pad_range (
        gdouble  *lo,
        gdouble  *hi,
        gboolean  from_zero)
{
    if (from_zero)
    {
        if (*lo > 0.0) *lo = 0.0;
        if (*hi < 0.0) *hi = 0.0;
    }

    if (*hi - *lo < 1e-9)
    {
        gdouble pad = (fabs (*hi) > 1e-9) ? fabs (*hi) * 0.05 : 0.5;
        *lo -= pad;
        *hi += pad;
    }
    else
    {
        gdouble pad = (*hi - *lo) * 0.05;
        *lo -= pad;
        *hi += pad;
    }
}

/* ------------------------------------------------------------------ */
/*  Graticule                                                          */
/* ------------------------------------------------------------------ */

static void
paint_graticule (
        cairo_t       *cr,
        const PnColor *grid,
        double         px,
        double         py,
        double         pw,
        double         ph,
        double         hairline)
{
    int i;

    cairo_save (cr);
    cairo_set_line_width (cr, hairline);

    /* Minor division lines. */
    for (i = 1; i < PN_OSC_DIV_X; i++)
    {
        double gx = px + pw * (double) i / (double) PN_OSC_DIV_X;
        set_rgba (cr, grid, (i == PN_OSC_DIV_X / 2) ? 0.55 : 0.28);
        cairo_move_to (cr, gx, py);
        cairo_line_to (cr, gx, py + ph);
        cairo_stroke (cr);
    }
    for (i = 1; i < PN_OSC_DIV_Y; i++)
    {
        double gy = py + ph * (double) i / (double) PN_OSC_DIV_Y;
        set_rgba (cr, grid, (i == PN_OSC_DIV_Y / 2) ? 0.55 : 0.28);
        cairo_move_to (cr, px, gy);
        cairo_line_to (cr, px + pw, gy);
        cairo_stroke (cr);
    }

    /* Fine tick marks along the two centre axes, like a real graticule. */
    {
        double cx = px + pw * 0.5;
        double cy = py + ph * 0.5;
        double t  = MIN (pw, ph) * 0.012;
        int    k;

        set_rgba (cr, grid, 0.55);
        for (k = 1; k < PN_OSC_DIV_X * 5; k++)
        {
            double gx = px + pw * (double) k / (double) (PN_OSC_DIV_X * 5);
            cairo_move_to (cr, gx, cy - t);
            cairo_line_to (cr, gx, cy + t);
        }
        for (k = 1; k < PN_OSC_DIV_Y * 5; k++)
        {
            double gy = py + ph * (double) k / (double) (PN_OSC_DIV_Y * 5);
            cairo_move_to (cr, cx - t, gy);
            cairo_line_to (cr, cx + t, gy);
        }
        cairo_stroke (cr);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Painter                                                            */
/* ------------------------------------------------------------------ */

static void
pn_oscilloscope_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnOscilloscope           *self = PN_OSCILLOSCOPE (node);
    PnOscilloscopePaintState  ps;
    double  smin   = MIN (w, h);
    double  bezelr = smin * 0.075;            /* bezel corner radius      */
    double  border = smin * 0.030;            /* bezel thickness          */
    double  scale  = smin / PN_OSC_REF_SHORT_SIDE;
    double  sx, sy, sw, sh;                   /* phosphor screen rect     */
    double  px, py, pw, ph;                   /* graticule / plot rect    */
    guint   cap;
    gdouble *tx, *ty;
    gdouble  xmin, xmax, ymin, ymax;
    guint    n;

    pn_oscilloscope_get_paint_state (self, &ps);

    /* --- Bezel: a dark moulded surround.  The worksheet has already
     *     dropped a rounded shadow under this rectangle. --- */
    cairo_save (cr);
    rounded_rect (cr, x, y, w, h, bezelr);
    {
        cairo_pattern_t *grad =
            cairo_pattern_create_linear (x, y, x, y + h);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.20, 0.21, 0.20);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.09, 0.10, 0.09);
        cairo_set_source (cr, grad);
        cairo_fill_preserve (cr);
        cairo_pattern_destroy (grad);
    }
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* --- Phosphor screen: inset into the bezel, slightly rounded. --- */
    sx = x + border;
    sy = y + border;
    sw = w - 2.0 * border;
    sh = h - 2.0 * border;
    if (sw < 4.0 || sh < 4.0)
        return;

    cairo_save (cr);
    rounded_rect (cr, sx, sy, sw, sh, bezelr * 0.5);
    cairo_clip (cr);

    /* Base fill + a corner vignette for a touch of CRT curvature. */
    set_rgba (cr, &ps.screen_color, 1.0);
    cairo_paint (cr);
    {
        cairo_pattern_t *vig = cairo_pattern_create_radial (
                sx + sw * 0.5, sy + sh * 0.5, smin * 0.10,
                sx + sw * 0.5, sy + sh * 0.5, MAX (sw, sh) * 0.70);
        cairo_pattern_add_color_stop_rgba (vig, 0.0, 0.0, 0.0, 0.0, 0.0);
        cairo_pattern_add_color_stop_rgba (vig, 1.0, 0.0, 0.0, 0.0, 0.45);
        cairo_set_source (cr, vig);
        cairo_paint (cr);
        cairo_pattern_destroy (vig);
    }

    /* Plot rectangle = screen interior, leaving a small inner margin. */
    {
        double inset = smin * 0.045;
        px = sx + inset;
        py = sy + inset;
        pw = sw - 2.0 * inset;
        ph = sh - 2.0 * inset;
    }
    if (pw < 2.0 || ph < 2.0)
    {
        cairo_restore (cr);
        return;
    }

    if (ps.show_graticule)
        paint_graticule (cr, &ps.grid_color, px, py, pw, ph,
                         MAX (0.6, scale * 0.8));

    /* --- Trace --- */
    cap = (guint) CLAMP (pw * 2.0, 64.0, 4096.0);
    tx  = g_new (gdouble, cap);
    ty  = g_new (gdouble, cap);

    n = pn_oscilloscope_read_trace (self, cap, tx, ty,
                                    &xmin, &xmax, &ymin, &ymax);

    if (n == 0)
    {
        /* No signal yet — a dim flat baseline across the centre. */
        cairo_save (cr);
        set_rgba (cr, &ps.trace_color, 0.30);
        cairo_set_line_width (cr, MAX (1.0, scale * (double) ps.trace_width));
        cairo_move_to (cr, px, py + ph * 0.5);
        cairo_line_to (cr, px + pw, py + ph * 0.5);
        cairo_stroke (cr);
        cairo_restore (cr);
    }
    else
    {
        double  lw = MAX (1.0, scale * (double) ps.trace_width);
        gdouble x0 = xmin, x1 = xmax, y0 = ymin, y1 = ymax;
        double  xspan, yspan;
        guint   i;

        pad_range (&x0, &x1, ps.x_from_zero);
        pad_range (&y0, &y1, ps.y_from_zero);
        xspan = x1 - x0;
        yspan = y1 - y0;

        cairo_save (cr);
        cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);

        /* --- Afterglow: the fading streak a moving dot leaves behind.
         *     Drawn first so the live dot/beam blooms over the top.  Only
         *     scalar (dot) mode keeps a trail; a vector snapshot returns
         *     none, so this is a no-op there. --- */
        {
            gdouble ax[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
            gdouble ay[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
            gdouble al[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
            guint   m = pn_oscilloscope_read_afterglow (
                            self, PN_OSCILLOSCOPE_AFTERGLOW_MAX, ax, ay, al);

            if (m > 0)
            {
                /* The live dot is the head the newest trail point trails a
                 * connector back to. */
                double curx = px + (tx[0] - x0) / xspan * pw;
                double cury = py + ph - (ty[0] - y0) / yspan * ph;
                guint  j;

                for (j = 0; j < m; j++)
                {
                    double bx = px + (ax[j] - x0) / xspan * pw;
                    double by = py + ph - (ay[j] - y0) / yspan * ph;
                    double ex, ey;
                    /* Square the linear life for a phosphor-like fast-then-
                     * slow falloff. */
                    double a  = al[j] * al[j];

                    if (j + 1 < m)
                    {
                        ex = px + (ax[j + 1] - x0) / xspan * pw;
                        ey = py + ph - (ay[j + 1] - y0) / yspan * ph;
                    }
                    else
                    {
                        ex = curx;
                        ey = cury;
                    }

                    /* Connector A→B: a soft wide glow under a thin core,
                     * both scaled by the point's remaining life. */
                    set_rgba (cr, &ps.trace_color, 0.16 * a);
                    cairo_set_line_width (cr, lw * 3.0);
                    cairo_move_to (cr, bx, by);
                    cairo_line_to (cr, ex, ey);
                    cairo_stroke (cr);

                    set_rgba (cr, &ps.trace_color, 0.65 * a);
                    cairo_set_line_width (cr, lw);
                    cairo_move_to (cr, bx, by);
                    cairo_line_to (cr, ex, ey);
                    cairo_stroke (cr);

                    /* A fading dot sitting at the vacated position. */
                    set_rgba (cr, &ps.trace_color, 0.55 * a);
                    cairo_arc (cr, bx, by, lw * 1.3, 0.0, 2.0 * G_PI);
                    cairo_fill (cr);
                }
            }
        }

        /* A single sample has no segment to stroke — mark it as a dot. */
        if (n == 1)
        {
            double dx = px + (tx[0] - x0) / xspan * pw;
            double dy = py + ph - (ty[0] - y0) / yspan * ph;

            /* Phosphor halo around the dot: a wide soft bloom, a medium
             * ring, then a bright near-white core — the dot-mode analogue
             * of the beam glow below. */
            set_rgba (cr, &ps.trace_color, 0.16);
            cairo_arc (cr, dx, dy, lw * 4.5, 0.0, 2.0 * G_PI);
            cairo_fill (cr);

            set_rgba (cr, &ps.trace_color, 0.30);
            cairo_arc (cr, dx, dy, lw * 2.2, 0.0, 2.0 * G_PI);
            cairo_fill (cr);

            cairo_set_source_rgba (cr,
                    MIN (1.0, ps.trace_color.red   + 0.45),
                    MIN (1.0, ps.trace_color.green + 0.10),
                    MIN (1.0, ps.trace_color.blue  + 0.45),
                    1.0);
            cairo_arc (cr, dx, dy, lw, 0.0, 2.0 * G_PI);
            cairo_fill (cr);
        }
        else
        {
            for (i = 0; i < n; i++)
            {
                double dx = px + (tx[i] - x0) / xspan * pw;
                double dy = py + ph - (ty[i] - y0) / yspan * ph;
                if (i == 0) cairo_move_to (cr, dx, dy);
                else        cairo_line_to (cr, dx, dy);
            }

            /* Phosphor glow: a wide soft pass, a medium pass, then a
             * bright near-white core — the classic blooming beam. */
            cairo_path_t *path = cairo_copy_path (cr);

            set_rgba (cr, &ps.trace_color, 0.16);
            cairo_set_line_width (cr, lw * 4.5);
            cairo_stroke_preserve (cr);

            set_rgba (cr, &ps.trace_color, 0.30);
            cairo_set_line_width (cr, lw * 2.2);
            cairo_stroke (cr);

            cairo_append_path (cr, path);
            cairo_set_source_rgba (cr,
                    MIN (1.0, ps.trace_color.red   + 0.45),
                    MIN (1.0, ps.trace_color.green + 0.10),
                    MIN (1.0, ps.trace_color.blue  + 0.45),
                    1.0);
            cairo_set_line_width (cr, lw);
            cairo_stroke (cr);

            cairo_path_destroy (path);
        }

        cairo_restore (cr);
    }

    g_free (tx);
    g_free (ty);

    /* --- Glass: a soft specular highlight across the top. --- */
    {
        cairo_pattern_t *gloss =
            cairo_pattern_create_linear (sx, sy, sx, sy + sh * 0.5);
        cairo_pattern_add_color_stop_rgba (gloss, 0.0, 1.0, 1.0, 1.0, 0.06);
        cairo_pattern_add_color_stop_rgba (gloss, 1.0, 1.0, 1.0, 1.0, 0.0);
        cairo_set_source (cr, gloss);
        cairo_paint (cr);
        cairo_pattern_destroy (gloss);
    }

    cairo_restore (cr);  /* screen clip */
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_oscilloscope_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_OSCILLOSCOPE));

    node_class->paint_plot              = pn_oscilloscope_paint_plot;
    /* The bezel is a rounded rectangle, so let the worksheet's drop
     * shadow follow that silhouette instead of leaking square corners. */
    node_class->paint_plot_corner_radius = 16.0;
    /* The CRT has a fixed screen shape: keep its aspect when the node is
     * lifted into the zoom overlay rather than stretching it. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    /* The class ref is intentionally held for the process lifetime — the
     * same lifetime the factory keeps it alive for — so the slots we just
     * wrote stay valid. */
}
