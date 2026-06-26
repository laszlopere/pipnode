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

/* The blooming phosphor beam — one recipe shared by the line-mode trace,
 * the scalar dot and the afterglow streak so they always match in stroke
 * weight and halo size: a wide soft pass and a medium ring (the bloom) at
 * the trace colour, then a bright near-white core.  Widths are multiples
 * of the base line width @lw; @a scales every pass for the afterglow fade
 * (a == 1.0 at full brightness). */
#define PN_OSC_GLOW_SOFT_MUL  4.5
#define PN_OSC_GLOW_SOFT_A    0.16
#define PN_OSC_GLOW_MED_MUL   2.2
#define PN_OSC_GLOW_MED_A     0.30

/** Source = the bright near-white beam core for trace colour @c. */
static void
set_beam_core (
        cairo_t       *cr,
        const PnColor *c,
        double         alpha)
{
    cairo_set_source_rgba (cr,
            MIN (1.0, c->red   + 0.45),
            MIN (1.0, c->green + 0.10),
            MIN (1.0, c->blue  + 0.45),
            alpha);
}

/** A dot with the full phosphor halo, centred at (@cx, @cy), radius @lw
 *  for the core; @a fades the whole stack (1.0 = the live dot). */
static void
draw_glow_dot (
        cairo_t       *cr,
        const PnColor *c,
        double         cx,
        double         cy,
        double         lw,
        double         a)
{
    set_rgba (cr, c, PN_OSC_GLOW_SOFT_A * a);
    cairo_arc (cr, cx, cy, lw * PN_OSC_GLOW_SOFT_MUL, 0.0, 2.0 * G_PI);
    cairo_fill (cr);

    set_rgba (cr, c, PN_OSC_GLOW_MED_A * a);
    cairo_arc (cr, cx, cy, lw * PN_OSC_GLOW_MED_MUL, 0.0, 2.0 * G_PI);
    cairo_fill (cr);

    set_beam_core (cr, c, a);
    cairo_arc (cr, cx, cy, lw, 0.0, 2.0 * G_PI);
    cairo_fill (cr);
}

/** One beam segment from (@x0,@y0) to (@x1,@y1) with the same halo a dot
 *  carries, so the afterglow connector matches both the dot and the line-
 *  mode beam in width and bloom; @a fades the whole stack.  The caller
 *  sets round caps/joins so segment ends round off like the beam. */
static void
draw_glow_segment (
        cairo_t       *cr,
        const PnColor *c,
        double         x0,
        double         y0,
        double         x1,
        double         y1,
        double         lw,
        double         a)
{
    set_rgba (cr, c, PN_OSC_GLOW_SOFT_A * a);
    cairo_set_line_width (cr, lw * PN_OSC_GLOW_SOFT_MUL);
    cairo_move_to (cr, x0, y0);
    cairo_line_to (cr, x1, y1);
    cairo_stroke (cr);

    set_rgba (cr, c, PN_OSC_GLOW_MED_A * a);
    cairo_set_line_width (cr, lw * PN_OSC_GLOW_MED_MUL);
    cairo_move_to (cr, x0, y0);
    cairo_line_to (cr, x1, y1);
    cairo_stroke (cr);

    set_beam_core (cr, c, a);
    cairo_set_line_width (cr, lw);
    cairo_move_to (cr, x0, y0);
    cairo_line_to (cr, x1, y1);
    cairo_stroke (cr);
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

/* Draw the CRT instrument (bezel, phosphor screen, graticule, trace) into
 * the rectangle (@x,@y,@w,@h).  At rest this is the whole node footprint;
 * when maximized it is just the top-left sub-rectangle, with the knob
 * panel painted around it by pn_oscilloscope_paint_plot. */
static void
paint_crt (
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

    /* The CRT-tube knobs reshape the beam: intensity scales every pass's
     * opacity (1.0 = the full-bright look the scope always had), focus
     * widens the stroke as it drops from 1 (a tighter beam stays sharp). */
    double  beam_bright = 0.10 + 0.90 * CLAMP (ps.intensity, 0.0, 1.0);
    double  beam_lw     = MAX (1.0, scale * (double) ps.trace_width)
                        * (1.0 + (1.0 - CLAMP (ps.focus, 0.0, 1.0)) * 1.8);

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
        set_rgba (cr, &ps.trace_color, 0.30 * beam_bright);
        cairo_set_line_width (cr, beam_lw);
        cairo_move_to (cr, px, py + ph * 0.5);
        cairo_line_to (cr, px + pw, py + ph * 0.5);
        cairo_stroke (cr);
        cairo_restore (cr);
    }
    else
    {
        double  lw = beam_lw;
        gdouble x0, x1, y0, y1;
        double  xspan, yspan;
        guint   i;

        /* The visible window: auto-fit (padded) per axis, or the manual
         * offset ± range/2 when a knob has been turned.  Single definition
         * lives in the core so the knobs and the mapping never disagree. */
        pn_oscilloscope_axis_window (self, TRUE,  xmin, xmax, &x0, &x1);
        pn_oscilloscope_axis_window (self, FALSE, ymin, ymax, &y0, &y1);
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

                    /* Connector B→next, drawn with the very same beam
                     * recipe the dot and the line-mode trace use, so the
                     * streak matches them in width and halo — only dimmed
                     * by the point's remaining life and the intensity knob. */
                    draw_glow_segment (cr, &ps.trace_color,
                                       bx, by, ex, ey, lw, a * beam_bright);
                }
            }
        }

        /* A single sample has no segment to stroke — mark it as a dot. */
        if (n == 1)
        {
            double dx = px + (tx[0] - x0) / xspan * pw;
            double dy = py + ph - (ty[0] - y0) / yspan * ph;

            /* The live dot: the full phosphor halo, scaled by the
             * intensity knob — the same recipe the afterglow streak and the
             * line-mode beam use, so dot and streak match. */
            draw_glow_dot (cr, &ps.trace_color, dx, dy, lw, beam_bright);
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
             * bright near-white core — the classic blooming beam.  Strokes
             * the whole polyline in one pass each (cleaner overlaps than
             * per-segment), but with the same widths/alphas the dot and the
             * afterglow streak use. */
            cairo_path_t *path = cairo_copy_path (cr);

            set_rgba (cr, &ps.trace_color, PN_OSC_GLOW_SOFT_A * beam_bright);
            cairo_set_line_width (cr, lw * PN_OSC_GLOW_SOFT_MUL);
            cairo_stroke_preserve (cr);

            set_rgba (cr, &ps.trace_color, PN_OSC_GLOW_MED_A * beam_bright);
            cairo_set_line_width (cr, lw * PN_OSC_GLOW_MED_MUL);
            cairo_stroke (cr);

            cairo_append_path (cr, path);
            set_beam_core (cr, &ps.trace_color, beam_bright);
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
/*  Maximized knob panel                                               */
/* ------------------------------------------------------------------ */

/* A short Pango label centred horizontally at (@cx, baseline @y_top),
 * drawn in the phosphor-green panel ink. */
static void
panel_text (
        cairo_t      *cr,
        double        cx,
        double        y_top,
        double        size,
        gboolean      bold,
        const PnColor *ink,
        const char   *text)
{
    PangoLayout          *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *desc   =
        pango_font_description_from_string (bold ? "Sans Bold" : "Sans");
    int pw, ph;

    pango_font_description_set_absolute_size (desc, size * PANGO_SCALE);
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    set_rgba (cr, ink, 1.0);
    cairo_move_to (cr, cx - pw * 0.5, y_top);
    pango_cairo_show_layout (cr, layout);
    g_object_unref (layout);
}

/* Format a knob value compactly (e.g. "2.0", "1.5k", "12m"). */
static void
format_value (
        char    *buf,
        gsize    len,
        gdouble  v)
{
    gdouble a = fabs (v);

    if (a != 0.0 && (a >= 1e5 || a < 1e-3))
        g_snprintf (buf, len, "%.2e", v);
    else
        g_snprintf (buf, len, "%.3g", v);
}

/* ------------------------------------------------------------------ */
/*  Kenwood-style palette                                              */
/*                                                                     */
/*  A warm light-grey instrument face with teal/green section groups,  */
/*  gold knob collars and dark rotary dials — the classic bench-scope  */
/*  bezel echoed from a Kenwood CS-series front panel.                 */
/* ------------------------------------------------------------------ */

/* Dark ink for labels printed on the light face. */
static const PnColor KEN_INK = { 0.14, 0.15, 0.14, 1.0 };

/* Format a level knob's 0..1 value as a percentage ("80%"). */
static void
format_percent (
        char    *buf,
        gsize    len,
        gdouble  v)
{
    g_snprintf (buf, len, "%d%%", (int) lround (CLAMP (v, 0.0, 1.0) * 100.0));
}

/* A faint tinted, rounded group panel behind a cluster of controls — the
 * coloured zones (teal VERTICAL, green HORIZONTAL …) of the reference. */
static void
draw_group_panel (
        cairo_t *cr,
        double   x, double y, double w, double h,
        double   tr, double tg, double tb)
{
    double rad = MIN (MIN (w, h) * 0.14, 14.0);

    if (w < 4.0 || h < 4.0)
        return;

    rounded_rect (cr, x, y, w, h, rad);
    cairo_set_source_rgba (cr, tr, tg, tb, 0.42);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.16);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
}

/* The dark rotary dial of radius @radius at (@cx,@cy): a top-lit metal disc
 * with a bright pointer at @angle (0 = up, +clockwise), dimmed by @ptr_a. */
static void
draw_dial (
        cairo_t *cr,
        double   cx,
        double   cy,
        double   radius,
        double   angle,
        double   ptr_a)
{
    cairo_pattern_t *grad;

    if (radius < 3.0)
        return;

    grad = cairo_pattern_create_radial (
            cx, cy - radius * 0.45, radius * 0.15,
            cx, cy,                 radius);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.30, 0.31, 0.32);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.06, 0.06, 0.07);
    cairo_arc (cr, cx, cy, radius, 0.0, 2.0 * G_PI);
    cairo_set_source (cr, grad);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    cairo_arc (cr, cx, cy, radius, 0.0, 2.0 * G_PI);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.65);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Pointer: a light notch from centre to the rim (the scope's white
     * skirt mark), dimmed when the knob is only mirroring an auto axis. */
    {
        double pxn = cx + sin (angle) * radius * 0.80;
        double pyn = cy - cos (angle) * radius * 0.80;

        cairo_set_source_rgba (cr, 0.92, 0.93, 0.88, ptr_a);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width (cr, MAX (2.0, radius * 0.18));
        cairo_move_to (cr, cx, cy);
        cairo_line_to (cr, pxn, pyn);
        cairo_stroke (cr);
    }
}

/* One panel knob inside @r.  A plain knob is a single dark dial; a
 * @concentric (range) knob adds a gold collar and a smaller inner FINE
 * dial with its own pointer (the coarse/fine stack of a VOLTS/DIV knob).
 * @is_level knobs (Focus / Intensity) print their value as a percentage.
 * @is_auto dims the pointer (the knob is mirroring a live auto axis). */
static void
draw_knob (
        cairo_t        *cr,
        const PnOscRect *r,
        const char     *name,
        gdouble         value,
        double          angle,
        gboolean        concentric,
        gboolean        is_level,
        gboolean        is_auto)
{
    double cx, dial_cy, radius;
    double label_h = MIN (r->h * 0.20, 16.0);
    double ptr_a   = is_auto ? 0.50 : 1.0;
    char   vbuf[32];

    pn_oscilloscope_knob_dial (r, &cx, &dial_cy, &radius);
    if (radius < 6.0)
        return;

    /* Name above the dial. */
    panel_text (cr, cx, r->y + 2.0, MIN (label_h, 13.0), FALSE,
                &KEN_INK, name);

    if (concentric)
    {
        /* Gold collar ring around the coarse skirt. */
        cairo_arc (cr, cx, dial_cy, radius, 0.0, 2.0 * G_PI);
        cairo_set_source_rgb (cr, 0.80, 0.64, 0.24);
        cairo_fill (cr);
        cairo_arc (cr, cx, dial_cy, radius, 0.0, 2.0 * G_PI);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);

        /* Coarse skirt: dark dial just inside the collar, coarse pointer. */
        draw_dial (cr, cx, dial_cy, radius * 0.82, angle, ptr_a);

        /* Fine inner dial: a vernier turning one decade per revolution, so
         * trimming it visibly spins the inner pointer. */
        {
            double fa = 0.0;

            if (value > 0.0 && isfinite (value))
            {
                fa = log10 (value) * (2.0 * G_PI);
                fa = fmod (fa, 2.0 * G_PI);
            }
            draw_dial (cr, cx, dial_cy, radius * PN_OSC_FINE_RADIUS_FRAC,
                       fa, ptr_a);
        }
    }
    else
    {
        draw_dial (cr, cx, dial_cy, radius, angle, ptr_a);
    }

    /* Value below the dial. */
    if (is_level)
        format_percent (vbuf, sizeof vbuf, value);
    else
        format_value (vbuf, sizeof vbuf, value);
    panel_text (cr, cx, dial_cy + radius + 2.0,
                MIN (label_h, 12.0), FALSE, &KEN_INK, vbuf);
}

/* A square pushbutton (the reference's grey momentary keys).  Bevelled,
 * with a green tally LED that lights when @active (the axis is auto-
 * fitting); pressing it returns the axis to auto. */
static void
draw_auto_button (
        cairo_t        *cr,
        const PnOscRect *r,
        const char     *label,
        gboolean        active)
{
    double rad = MIN (5.0, r->h * 0.25);
    double led_r;

    /* Bevelled grey key — slightly pressed-in when active. */
    rounded_rect (cr, r->x, r->y, r->w, r->h, rad);
    {
        cairo_pattern_t *grad =
            cairo_pattern_create_linear (r->x, r->y, r->x, r->y + r->h);
        if (active)
        {
            cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.66, 0.66, 0.62);
            cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.80, 0.80, 0.76);
        }
        else
        {
            cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.86, 0.86, 0.82);
            cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.70, 0.70, 0.66);
        }
        cairo_set_source (cr, grad);
        cairo_fill_preserve (cr);
        cairo_pattern_destroy (grad);
    }
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.40);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Tally LED, top-left, lit green when active. */
    led_r = MIN (r->h, r->w) * 0.10;
    led_r = CLAMP (led_r, 2.5, 5.0);
    {
        double lx = r->x + r->w * 0.16;
        double ly = r->y + r->h * 0.30;

        if (active)
        {
            cairo_set_source_rgba (cr, 0.30, 0.92, 0.36, 0.40);
            cairo_arc (cr, lx, ly, led_r * 2.0, 0.0, 2.0 * G_PI);
            cairo_fill (cr);
            cairo_set_source_rgb (cr, 0.40, 1.0, 0.46);
        }
        else
        {
            cairo_set_source_rgb (cr, 0.30, 0.34, 0.30);
        }
        cairo_arc (cr, lx, ly, led_r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
    }

    {
        double th = MIN (r->h * 0.42, 13.0);
        panel_text (cr, r->x + r->w * 0.5, r->y + (r->h - th) * 0.5 + th * 0.18,
                    th, TRUE, &KEN_INK, label);
    }
}

/* Pointer angle for a knob, in radians (0 = up, +clockwise).  Range knobs
 * sweep with log(range) so a decade is a fixed turn; offset knobs sweep
 * with offset/range, clamped, so centre reads straight up. */
static double
knob_angle (
        gboolean is_range,
        gdouble  range,
        gdouble  offset)
{
    if (is_range)
    {
        if (!(range > 0.0) || !isfinite (range))
            return 0.0;
        /* 90° per decade, wrapped into [-PI, PI). */
        double a = log10 (range) * (G_PI * 0.5);
        a = fmod (a + G_PI, 2.0 * G_PI);
        if (a < 0.0) a += 2.0 * G_PI;
        return a - G_PI;
    }
    else
    {
        double frac = (range > 0.0 && isfinite (range)) ? offset / range : 0.0;
        frac = CLAMP (frac, -1.5, 1.5);
        return frac * (G_PI * 0.66);     /* ±~120° over ±1.5 screenfuls */
    }
}

/* Pointer angle for a level knob: 0..1 swept across a 270° arc, value 0.5
 * reading straight up (−135° at 0, +135° at 1). */
static double
level_angle (gdouble v)
{
    return (CLAMP (v, 0.0, 1.0) * 2.0 - 1.0) * (G_PI * 0.75);
}

/* The light moulded instrument face behind the whole maximized rect. */
static void
paint_panel_bg (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    double r = MIN (w, h) * 0.04;

    rounded_rect (cr, x, y, w, h, r);
    {
        cairo_pattern_t *grad = cairo_pattern_create_linear (x, y, x, y + h);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.87, 0.86, 0.80);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.72, 0.71, 0.65);
        cairo_set_source (cr, grad);
        cairo_fill_preserve (cr);
        cairo_pattern_destroy (grad);
    }
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.40);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
}

/* ------------------------------------------------------------------ */
/*  Painter entry point                                                */
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
    PnOscilloscope *self = PN_OSCILLOSCOPE (node);
    PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
    gdouble         xmin, xmax, ymin, ymax;
    gdouble         raw_x_lo, raw_x_hi, raw_y_lo, raw_y_hi;
    int             i;

    /* At rest (and during a zoom of any OTHER node) the scope is the bare
     * CRT, exactly as before. */
    if (!pn_oscilloscope_get_maximized (self))
    {
        paint_crt (node, cr, x, y, w, h);
        return;
    }

    /* Maximized: CRT retreats to the top-left, knob panel fills the rest. */
    pn_oscilloscope_layout (self, x, y, w, h, &crt, knobs, autobtn);

    paint_panel_bg (cr, x, y, w, h);

    /* Coloured section groups behind the control clusters — teal over the
     * scale/position knobs, green over the Focus/Intensity tube knobs and
     * the Auto pushbuttons, echoing the reference's labelled zones. */
    {
        double pad = 4.0;
        PnOscRect a = knobs[PN_OSC_KNOB_X_RANGE];
        PnOscRect b = knobs[PN_OSC_KNOB_Y_OFFSET];
        PnOscRect f = knobs[PN_OSC_KNOB_FOCUS];
        PnOscRect g = knobs[PN_OSC_KNOB_INTENSITY];

        draw_group_panel (cr, a.x + pad, a.y + pad,
                          (b.x + b.w) - a.x - 2.0 * pad,
                          (b.y + b.h) - a.y - 2.0 * pad,
                          0.50, 0.74, 0.70);                 /* teal  */
        draw_group_panel (cr, f.x + pad, f.y + pad,
                          (g.x + g.w) - f.x - 2.0 * pad,
                          (g.y + g.h) - f.y - 2.0 * pad,
                          0.64, 0.78, 0.50);                 /* green */
        draw_group_panel (cr, autobtn[0].x - pad, autobtn[0].y - pad,
                          (autobtn[1].x + autobtn[1].w) - autobtn[0].x
                              + 2.0 * pad,
                          autobtn[0].h + 2.0 * pad,
                          0.64, 0.78, 0.50);                 /* green */
    }

    paint_crt (node, cr, crt.x, crt.y, crt.w, crt.h);

    /* Raw data extent → the live (effective) range/offset shown on each
     * knob, even while an axis is still auto-fitting.  We only need the
     * bounds, but read_trace requires scratch output arrays. */
    {
        gdouble scratchx[2], scratchy[2];

        pn_oscilloscope_read_trace (self, 2, scratchx, scratchy,
                                    &xmin, &xmax, &ymin, &ymax);
    }
    raw_x_lo = xmin; raw_x_hi = xmax;
    raw_y_lo = ymin; raw_y_hi = ymax;

    for (i = 0; i < PN_OSC_KNOB_N; i++)
    {
        gboolean concentric = pn_oscilloscope_knob_is_concentric (i);
        gboolean is_level    = pn_oscilloscope_knob_is_level (i);

        if (is_level)
        {
            gdouble     v    = pn_oscilloscope_knob_level (self, i);
            const char *name = (i == PN_OSC_KNOB_FOCUS) ? "Focus"
                                                        : "Intensity";

            draw_knob (cr, &knobs[i], name, v, level_angle (v),
                       FALSE, TRUE, FALSE);
        }
        else
        {
            gboolean    x_axis  = (i == PN_OSC_KNOB_X_RANGE ||
                                   i == PN_OSC_KNOB_X_OFFSET);
            gboolean    is_auto = pn_oscilloscope_axis_is_auto (self, x_axis);
            gdouble     raw_lo  = x_axis ? raw_x_lo : raw_y_lo;
            gdouble     raw_hi  = x_axis ? raw_x_hi : raw_y_hi;
            gdouble     range, offset;
            const char *name;

            pn_oscilloscope_axis_knob_values (self, x_axis, raw_lo, raw_hi,
                                              &range, &offset);

            switch (i)
            {
            case PN_OSC_KNOB_X_RANGE:  name = "X range";  break;
            case PN_OSC_KNOB_X_OFFSET: name = "X offset"; break;
            case PN_OSC_KNOB_Y_RANGE:  name = "Y range";  break;
            default:                   name = "Y offset"; break;
            }

            draw_knob (cr, &knobs[i], name,
                       concentric ? range : offset,
                       knob_angle (concentric, range, offset),
                       concentric, FALSE, is_auto);
        }
    }

    draw_auto_button (cr, &autobtn[0], "Auto X",
                      pn_oscilloscope_axis_is_auto (self, TRUE));
    draw_auto_button (cr, &autobtn[1], "Auto Y",
                      pn_oscilloscope_axis_is_auto (self, FALSE));
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
