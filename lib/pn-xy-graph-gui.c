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
/*  PnXYGraph — gui tier.                                              */
/*                                                                     */
/*  The PLplot/cairo scatter-plot painter: a single 2D plot that       */
/*  overlays one coloured series per topic, auto-ranging both axes      */
/*  over the visible (x, y) samples.  The node's GType, properties,     */
/*  receive() logic, per-topic series fan-out, the sample rings and     */
/*  the throttle timer live in the GTK-free core file pn-xy-graph.c;    */
/*  this file installs the drawing vfunc slot onto that class at editor */
/*  startup (pn_xy_graph_gui_install), reading the rings through the    */
/*  core's GTK-free data accessors.  The per-instance PLplot stream is  */
/*  owned entirely here.  The headless runtime never loads this half,   */
/*  so the XY Graph logic runs without GTK or PLplot.                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-xy-graph-gui.h"
#include "pn-xy-graph.h"

#include <gtk/gtk.h>
#include <math.h>
#include <plplot/plplot.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_XY_GRAPH_WIDTH  280.0

/* PLplot plpoin glyph used for the Points draw-style: a filled circle. */
#define PN_XY_GRAPH_POINT_SYMBOL  17

/* GObject data key holding the per-instance PLplot stream id. */
#define PN_XY_GRAPH_PLSTREAM_KEY  "pn-xy-graph-plstream"

/* ------------------------------------------------------------------ */
/*  Per-instance PLplot stream                                          */
/*                                                                     */
/*  PLplot keeps a process-wide stream table; reusing one id across     */
/*  multiple PnXYGraph nodes would cross their axis state, so each node  */
/*  owns its own id, allocated lazily on first paint and torn down by a  */
/*  destroy-notify — keeping the whole PLplot lifecycle in the gui tier. */
/* ------------------------------------------------------------------ */

static void
pn_xy_graph_plstream_free (gpointer data)
{
    PLINT *id = data;

    if (id != NULL)
    {
        plsstrm (*id);
        plend1 ();
        g_free (id);
    }
}

static PLINT
pn_xy_graph_plstream (PnXYGraph *self)
{
    PLINT *id = g_object_get_data (G_OBJECT (self), PN_XY_GRAPH_PLSTREAM_KEY);

    if (id == NULL)
    {
        id = g_new0 (PLINT, 1);
        plmkstrm (id);
        g_object_set_data_full (G_OBJECT (self), PN_XY_GRAPH_PLSTREAM_KEY,
                                id, pn_xy_graph_plstream_free);
    }

    return *id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Deterministic colour for series @idx.  Index 0 uses the user-picked
 *  line colour exactly.  Higher indices walk the hue wheel in golden-
 *  angle steps (≈137.5°) starting from the base hue. */
static void
color_for_series (
        const PnXYGraphPaintState *ps,
        guint                      idx,
        PnColor                   *out)
{
    if (idx == 0)
    {
        *out = ps->line_color;
        return;
    }

    {
        PLFLT  h_base, l_base, s_base;
        PLFLT  h, l, s;
        PLFLT  r, g, b;

        plrgbhls ((PLFLT) ps->line_color.red,
                  (PLFLT) ps->line_color.green,
                  (PLFLT) ps->line_color.blue,
                  &h_base, &l_base, &s_base);

        h = fmod (h_base + 137.508 * (gdouble) idx, 360.0);
        if (h < 0.0) h += 360.0;
        l = 0.50;
        s = 0.65;

        plhlsrgb (h, l, s, &r, &g, &b);

        out->red   = r;
        out->green = g;
        out->blue  = b;
        out->alpha = ps->line_color.alpha;
    }
}

/** Format @value into @label as inline scientific notation when its
 *  magnitude is far enough from 1 that "%g" would otherwise emit
 *  "1e+17" or trigger PLplot's multiplier extraction. */
static void
format_inline_scientific (
        char    *label,
        PLINT    length,
        gdouble  value)
{
    gdouble abs_v = fabs (value);
    gint    exp10;
    gdouble mantissa;
    gchar   mant_buf[32];

    if (abs_v == 0.0)
    {
        g_snprintf (label, length, "0");
        return;
    }

    if (abs_v >= 1e-3 && abs_v < 1e6)
    {
        g_snprintf (label, length, "%g", value);
        return;
    }

    exp10    = (gint) floor (log10 (abs_v));
    mantissa = value / pow (10.0, exp10);

    g_snprintf (mant_buf, sizeof (mant_buf), "%g", mantissa);

    g_snprintf (label, length, "%s×10#u%d#d", mant_buf, exp10);
}

/* Tick-label formatter — both axes carry plain numeric values. */
static void
xy_label_format (
        PLINT     axis,
        PLFLT     value,
        char     *label,
        PLINT     length,
        PLPointer user)
{
    (void) axis;
    (void) user;
    format_inline_scientific (label, length, (gdouble) value);
}

/** Common PLplot setup shared by the painter. */
static void
paint_setup_page (
        PLINT                      plstream,
        const PnXYGraphPaintState *ps,
        cairo_t                   *cr,
        double                     w,
        double                     h)
{
    plsstrm (plstream);
    plsdev  ("extcairo");
    plspage (0.0, 0.0,
             (PLINT) w,
             (PLINT) h,
             0, 0);
    plinit  ();
    pl_cmd  (PLESC_DEVINIT, cr);

    pladv (0);

    plscol0 (1,
             (PLINT) (ps->line_color.red   * 255.0 + 0.5),
             (PLINT) (ps->line_color.green * 255.0 + 0.5),
             (PLINT) (ps->line_color.blue  * 255.0 + 0.5));
    plscol0 (2,
             (PLINT) (ps->axis_color.red   * 255.0 + 0.5),
             (PLINT) (ps->axis_color.green * 255.0 + 0.5),
             (PLINT) (ps->axis_color.blue  * 255.0 + 0.5));

    {
        const gboolean enlarged = (w > PN_XY_GRAPH_WIDTH * 1.2);
        plschr (0, enlarged ? 0.9 : 1.8);
    }

    plsxax (20, 0);
    plsyax (20, 0);
}

/** Pin colour @rgba into cmap0 slot @slot. */
static void
paint_set_cmap0 (
        PLINT          slot,
        const PnColor *rgba)
{
    plscol0 (slot,
             (PLINT) (rgba->red   * 255.0 + 0.5),
             (PLINT) (rgba->green * 255.0 + 0.5),
             (PLINT) (rgba->blue  * 255.0 + 0.5));
}

/** Pad a [lo, hi] range so the data does not touch the frame; widen a
 *  degenerate (flat) range to something drawable.  When @from_zero, the
 *  range is first anchored at zero. */
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
/*  Painter                                                            */
/* ------------------------------------------------------------------ */

static void
pn_xy_graph_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnXYGraph           *self    = PN_XY_GRAPH (node);
    PnXYGraphSeriesView *views   = NULL;
    guint                nseries = 0;
    PnXYGraphPaintState  ps;
    PLINT                plstream;
    guint                cap;
    gdouble             *all_x;
    gdouble             *all_y;
    guint               *counts;
    gdouble              xmin = 0.0, xmax = 1.0;
    gdouble              ymin = 0.0, ymax = 1.0;
    gboolean             have_range = FALSE;
    guint                i;

    pn_xy_graph_get_paint_state (self, &ps);

    /* Plot background and frame are user-tunable.  Even with no data the
     * rectangle is painted so the empty plot reads as a deliberate area. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.axis_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    views = pn_xy_graph_collect_series_sorted (self, &nseries);
    if (nseries == 0)
        return;

    cap = ps.max_points;
    if (cap > PN_XY_GRAPH_SAMPLES) cap = PN_XY_GRAPH_SAMPLES;
    if (cap == 0)
    {
        g_free (views);
        return;
    }

    all_x  = g_new (gdouble, nseries * cap);
    all_y  = g_new (gdouble, nseries * cap);
    counts = g_new0 (guint, nseries);

    for (i = 0; i < nseries; i++)
    {
        gdouble *gx = all_x + i * cap;
        gdouble *gy = all_y + i * cap;
        guint    n  = pn_xy_graph_series_chrono (views[i].series, cap, gx, gy);
        guint    j;

        counts[i] = n;
        for (j = 0; j < n; j++)
        {
            if (!have_range)
            {
                xmin = xmax = gx[j];
                ymin = ymax = gy[j];
                have_range = TRUE;
            }
            else
            {
                if (gx[j] < xmin) xmin = gx[j];
                if (gx[j] > xmax) xmax = gx[j];
                if (gy[j] < ymin) ymin = gy[j];
                if (gy[j] > ymax) ymax = gy[j];
            }
        }
    }

    if (!have_range)
    {
        g_free (all_x); g_free (all_y); g_free (counts); g_free (views);
        return;
    }

    pad_range (&xmin, &xmax, ps.x_from_zero);
    pad_range (&ymin, &ymax, ps.y_from_zero);

    plstream = pn_xy_graph_plstream (self);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    paint_setup_page (plstream, &ps, cr, w, h);

    plvpor (0.12, 0.97, 0.18, 0.93);
    plwind ((PLFLT) xmin, (PLFLT) xmax, (PLFLT) ymin, (PLFLT) ymax);

    plslabelfunc (xy_label_format, NULL);
    plcol0  (2);
    plwidth (1);
    {
        const char *xopts = ps.show_grid ? "bcgnsto" : "bcnsto";
        const char *yopts = ps.show_grid ? "bcgnsto" : "bcnsto";
        plbox (xopts, 0.0, 0,
               yopts, 0.0, 0);
    }
    plslabelfunc (NULL, NULL);

    {
        PLFLT *px = g_new (PLFLT, cap);
        PLFLT *py = g_new (PLFLT, cap);

        for (i = 0; i < nseries; i++)
        {
            guint    n  = counts[i];
            gdouble *gx = all_x + i * cap;
            gdouble *gy = all_y + i * cap;
            PnColor  c;
            guint    j;

            if (n == 0)
                continue;

            for (j = 0; j < n; j++)
            {
                px[j] = (PLFLT) gx[j];
                py[j] = (PLFLT) gy[j];
            }

            color_for_series (&ps, i, &c);
            paint_set_cmap0 (8 + (PLINT) i, &c);
            plcol0 (8 + (PLINT) i);

            if (ps.style == PN_XY_GRAPH_STYLE_LINES && n >= 2)
            {
                plwidth ((PLINT) ps.line_width);
                plline ((PLINT) n, px, py);
            }
            else
            {
                /* Points style, or a single-sample series that has no
                 * polyline to draw — render discrete markers. */
                plssym (0.0, 1.5 + 0.5 * (gdouble) (ps.line_width - 1));
                plpoin ((PLINT) n, px, py, PN_XY_GRAPH_POINT_SYMBOL);
            }
        }

        g_free (px);
        g_free (py);
    }

    pleop  ();
    plend1 ();

    cairo_restore (cr);

    g_free (all_x); g_free (all_y); g_free (counts); g_free (views);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_xy_graph_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_XY_GRAPH));

    node_class->paint_plot = pn_xy_graph_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid. */
}
