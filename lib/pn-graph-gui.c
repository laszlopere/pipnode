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
/*  PnGraph — gui tier.                                                 */
/*                                                                     */
/*  The whole PLplot/cairo plot painter (2D + 3D time-series and        */
/*  distribution renderers, the inline-scientific tick formatter, the   */
/*  per-series colour wheel, the 3D bar projection + painter's-         */
/*  algorithm depth sort) and the two-tab settings dialog.  The node's  */
/*  GType, properties, receive() logic, the per-topic series fan-out,   */
/*  the time-bucket rings and the throttle/refresh timers live in the   */
/*  GTK-free core file pn-graph.c; this file installs the drawing +      */
/*  dialog vfunc slots onto that class at editor startup                 */
/*  (pn_graph_gui_install), reading the rings through the core's         */
/*  GTK-free data accessors.  The per-instance PLplot stream is owned    */
/*  entirely here: allocated lazily on first paint and torn down via a   */
/*  GObject data destroy-notify, so the core never references PLplot.    */
/*  The headless runtime never loads this file's half, so the Graph      */
/*  logic runs without GTK or PLplot.                                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-graph-gui.h"
#include "pn-graph.h"

#include <gtk/gtk.h>
#include <math.h>
#include <plplot/plplot.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_GRAPH_WIDTH         280.0
#define PN_GRAPH_HEADER_HEIGHT  40.0

/* Number of value bins drawn in the 2D distribution view. */
#define PN_GRAPH_HIST_BINS  40

/* Number of value bins drawn in 3D histogram mode. */
#define PN_GRAPH_3D_HIST_BINS  12

/* Fraction of a bin's X-width occupied by the bar itself. */
#define PN_GRAPH_3D_BAR_FILL  0.8

/* Half-width of each topic ribbon in world Y-units. */
#define PN_GRAPH_3D_RIBBON_HALF  0.20

/* Viewing angles for the 3D projection. */
#define PN_GRAPH_3D_ALT  45.0
#define PN_GRAPH_3D_AZ   300.0

/* GObject data key holding the per-instance PLplot stream id (boxed in a
 * heap PLINT so a destroy-notify can run plend1 at finalize). */
#define PN_GRAPH_PLSTREAM_KEY  "pn-graph-plstream"

/* ------------------------------------------------------------------ */
/*  Per-instance PLplot stream                                          */
/*                                                                     */
/*  PLplot keeps a process-wide stream table; reusing one id across     */
/*  multiple PnGraph nodes would cross their axis state, so each node    */
/*  owns its own id.  The id is allocated lazily on the first paint and  */
/*  stashed on the GObject; its destroy-notify selects the stream and    */
/*  runs plend1 when the node is finalised, which keeps the whole PLplot */
/*  lifecycle inside the gui tier (the core never calls plmkstrm /       */
/*  plend1).                                                             */
/* ------------------------------------------------------------------ */

static void
pn_graph_plstream_free (gpointer data)
{
    PLINT *id = data;

    if (id != NULL)
    {
        plsstrm (*id);
        plend1 ();
        g_free (id);
    }
}

/** Return this node's PLplot stream id, allocating one on first use. */
static PLINT
pn_graph_plstream (PnGraph *self)
{
    PLINT *id = g_object_get_data (G_OBJECT (self), PN_GRAPH_PLSTREAM_KEY);

    if (id == NULL)
    {
        id = g_new0 (PLINT, 1);
        plmkstrm (id);
        g_object_set_data_full (G_OBJECT (self), PN_GRAPH_PLSTREAM_KEY,
                                id, pn_graph_plstream_free);
    }

    return *id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Deterministic colour for series @idx.  Index 0 uses the user-
 *  picked line colour exactly.  Higher indices walk the hue wheel in
 *  golden-angle steps (≈137.5°) starting from the base hue. */
static void
color_for_series (
        const PnGraphPaintState *ps,
        guint                    idx,
        PnColor                 *out)
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

/* The PLplot tick-label formatter is handed a PnGraphPaintState* (the
 * per-paint snapshot) as its user pointer so it can branch on the active
 * view + resolution without reaching back into the node. */
static void
graph_label_format (
        PLINT     axis,
        PLFLT     value,
        char     *label,
        PLINT     length,
        PLPointer user)
{
    const PnGraphPaintState *ps = user;

    if (axis == PL_X_AXIS && ps->view == PN_GRAPH_VIEW_TIME_SERIES)
    {
        gdouble secs = fabs ((gdouble) value);

        switch (ps->resolution)
        {
        case PN_GRAPH_RES_MINUTE:
            g_snprintf (label, length, "%gs", secs);
            break;
        case PN_GRAPH_RES_15_MINUTES:
        case PN_GRAPH_RES_HOUR:
            g_snprintf (label, length, "%gm", secs / 60.0);
            break;
        case PN_GRAPH_RES_DAY:
            g_snprintf (label, length, "%gh", secs / 3600.0);
            break;
        case PN_GRAPH_RES_WEEK:
            g_snprintf (label, length, "%gd", secs / 86400.0);
            break;
        }
        return;
    }

    format_inline_scientific (label, length, (gdouble) value);
}

/* ------------------------------------------------------------------ */
/*  Data extraction (reads the core's per-series rings)                 */
/* ------------------------------------------------------------------ */

/** Walk @s's bin ring and emit (seconds_ago, mean_value) pairs in
 *  left-to-right time order. */
static guint
collect_line_points (
        const PnGraphSeries *s,
        guint                n_bins,
        gint64               width,
        gint64               cur_epoch,
        gint64               oldest_valid_epoch,
        PLFLT               *xs,
        PLFLT               *ys,
        gdouble             *inout_ymin,
        gdouble             *inout_ymax,
        gboolean            *have_range_io)
{
    guint  n = 0;
    guint  i;

    for (i = 0; i < n_bins; i++)
    {
        gint64 epoch = oldest_valid_epoch + (gint64) i;
        gint64 slot  = epoch % (gint64) n_bins;
        if (slot < 0) slot += (gint64) n_bins;
        {
            const PnGraphBin *b = &s->ring[slot];
            gdouble           mean;
            gdouble           seconds_ago;

            if (b->epoch != epoch || b->count == 0)
                continue;

            mean = b->sum / (gdouble) b->count;
            seconds_ago = (gdouble) (cur_epoch - epoch)
                        * (gdouble) width
                        / (gdouble) G_TIME_SPAN_SECOND;

            xs[n] = (PLFLT) (-seconds_ago);
            ys[n] = (PLFLT) mean;
            n += 1;

            if (!*have_range_io)
            {
                *inout_ymin = *inout_ymax = mean;
                *have_range_io = TRUE;
            }
            else
            {
                if (b->min < *inout_ymin) *inout_ymin = b->min;
                if (b->max > *inout_ymax) *inout_ymax = b->max;
            }
        }
    }

    return n;
}

/** Like collect_line_points, but for the Error-bars draw-style. */
static guint
collect_error_bars (
        const PnGraphSeries *s,
        guint                n_bins,
        gint64               width,
        gint64               cur_epoch,
        gint64               oldest_valid_epoch,
        PLFLT               *xs,
        PLFLT               *mean,
        PLFLT               *sd_lo,
        PLFLT               *sd_hi,
        PLFLT               *w_lo,
        PLFLT               *w_hi,
        gdouble             *inout_ymin,
        gdouble             *inout_ymax,
        gboolean            *have_range_io)
{
    guint  n = 0;
    guint  i;

    for (i = 0; i < n_bins; i++)
    {
        gint64 epoch = oldest_valid_epoch + (gint64) i;
        gint64 slot  = epoch % (gint64) n_bins;
        if (slot < 0) slot += (gint64) n_bins;
        {
            const PnGraphBin *b = &s->ring[slot];
            gdouble           m, var, sd, seconds_ago;

            if (b->epoch != epoch || b->count == 0)
                continue;

            m   = b->sum / (gdouble) b->count;
            var = b->sum_sq / (gdouble) b->count - m * m;
            if (var < 0.0) var = 0.0;       /* float noise on flat bins */
            sd  = sqrt (var);

            seconds_ago = (gdouble) (cur_epoch - epoch)
                        * (gdouble) width
                        / (gdouble) G_TIME_SPAN_SECOND;

            xs[n]    = (PLFLT) (-seconds_ago);
            mean[n]  = (PLFLT) m;
            sd_lo[n] = (PLFLT) (m - sd);
            sd_hi[n] = (PLFLT) (m + sd);
            w_lo[n]  = (PLFLT) b->min;
            w_hi[n]  = (PLFLT) b->max;
            n += 1;

            if (!*have_range_io)
            {
                *inout_ymin = b->min;
                *inout_ymax = b->max;
                *have_range_io = TRUE;
            }
            else
            {
                if (b->min < *inout_ymin) *inout_ymin = b->min;
                if (b->max > *inout_ymax) *inout_ymax = b->max;
            }
        }
    }

    /* A bin holding a single sample has min == max, so its whisker would
     * collapse to a zero-height glyph — a row of disconnected dashes for a
     * slow, one-sample-per-bin series.  Extend each whisker halfway toward
     * the neighbouring buckets' means so adjacent whiskers meet at the
     * midpoints, tracing the movement as a continuous vertical band while
     * still leaving the sample's own value at the centre.  Any genuine
     * intra-bin spread (w_lo..w_hi) is kept via the union.  Midpoints of
     * in-range means stay in range, so the y-range needs no adjustment. */
    for (i = 0; i < n; i++)
    {
        gdouble lo = mean[i], hi = mean[i];

        if ((gdouble) w_lo[i] < lo) lo = w_lo[i];
        if ((gdouble) w_hi[i] > hi) hi = w_hi[i];

        if (i > 0)
        {
            gdouble mid = 0.5 * ((gdouble) mean[i] + (gdouble) mean[i - 1]);
            if (mid < lo) lo = mid;
            if (mid > hi) hi = mid;
        }
        if (i + 1 < n)
        {
            gdouble mid = 0.5 * ((gdouble) mean[i] + (gdouble) mean[i + 1]);
            if (mid < lo) lo = mid;
            if (mid > hi) hi = mid;
        }

        w_lo[i] = (PLFLT) lo;
        w_hi[i] = (PLFLT) hi;
    }

    return n;
}

/** Count in-window samples for series @s and write the kept values to
 *  @out_values, tracking the value range. */
static guint
collect_window_values (
        const PnGraphSeries *s,
        gint64               cutoff,
        PLFLT               *out_values,
        guint                cap,
        gdouble             *inout_vmin,
        gdouble             *inout_vmax,
        gboolean            *have_range_io)
{
    guint n = 0;
    guint i;

    for (i = 0; i < s->sample_count && n < cap; i++)
    {
        const PnGraphSample *p = &s->samples[i];
        if (p->time_us < cutoff)
            continue;

        out_values[n++] = (PLFLT) p->value;

        if (!*have_range_io)
        {
            *inout_vmin = *inout_vmax = p->value;
            *have_range_io = TRUE;
        }
        else
        {
            if (p->value < *inout_vmin) *inout_vmin = p->value;
            if (p->value > *inout_vmax) *inout_vmax = p->value;
        }
    }

    return n;
}

/* ------------------------------------------------------------------ */
/*  PLplot page setup + colour helpers                                  */
/* ------------------------------------------------------------------ */

/** Common PLplot setup shared by every painter. */
static void
paint_setup_page (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t                 *cr,
        double                   w,
        double                   h)
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
        const gboolean enlarged = (w > PN_GRAPH_WIDTH * 1.2);
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

/** Re-shade an RGB triple in HLS space: keep hue and saturation, set
 *  lightness to @target (clamped). */
static PnColor
shade_l (
        PnColor c,
        gdouble target_l)
{
    PLFLT h, l, s;
    PLFLT r, g, b;
    PnColor out;

    plrgbhls ((PLFLT) c.red, (PLFLT) c.green, (PLFLT) c.blue, &h, &l, &s);

    if (target_l < 0.0) target_l = 0.0;
    if (target_l > 1.0) target_l = 1.0;

    if (s < 0.55) s = 0.55;

    plhlsrgb (h, (PLFLT) target_l, s, &r, &g, &b);

    out.red   = r;
    out.green = g;
    out.blue  = b;
    out.alpha = c.alpha;
    return out;
}

/* ------------------------------------------------------------------ */
/*  3D projection helper                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    PLFLT basex, basey, height;
    PLFLT xmin, xmax;
    PLFLT ymin, ymax;
    PLFLT zmin, zmax;
    PLFLT caz, saz, calt, salt;
} Pn3DProj;

static void
proj_init (
        Pn3DProj *p,
        PLFLT     basex,
        PLFLT     basey,
        PLFLT     height,
        PLFLT     xmin,
        PLFLT     xmax,
        PLFLT     ymin,
        PLFLT     ymax,
        PLFLT     zmin,
        PLFLT     zmax,
        PLFLT     alt_deg,
        PLFLT     az_deg)
{
    p->basex  = basex;  p->basey  = basey;  p->height = height;
    p->xmin   = xmin;   p->xmax   = xmax;
    p->ymin   = ymin;   p->ymax   = ymax;
    p->zmin   = zmin;   p->zmax   = zmax;
    p->caz    = (PLFLT) cos (M_PI * az_deg  / 180.0);
    p->saz    = (PLFLT) sin (M_PI * az_deg  / 180.0);
    p->calt   = (PLFLT) cos (M_PI * alt_deg / 180.0);
    p->salt   = (PLFLT) sin (M_PI * alt_deg / 180.0);
}

static inline void
proj_xy (
        const Pn3DProj *p,
        PLFLT           x,
        PLFLT           y,
        PLFLT           z,
        PLFLT          *sx,
        PLFLT          *sy)
{
    PLFLT xn = (x - p->xmin) / (p->xmax - p->xmin) * p->basex - p->basex * 0.5f;
    PLFLT yn = (y - p->ymin) / (p->ymax - p->ymin) * p->basey - p->basey * 0.5f;
    PLFLT zn = (z - p->zmin) / (p->zmax - p->zmin) * p->height;

    *sx = xn * p->caz + yn * p->saz;
    *sy = -xn * p->saz * p->salt + yn * p->caz * p->salt + zn * p->calt;
}

/** Depth of a 3D point under the projection: higher means further from
 *  the viewer (drawn first by the painter's algorithm). */
static inline PLFLT
proj_depth (
        const Pn3DProj *p,
        PLFLT           x,
        PLFLT           y,
        PLFLT           z)
{
    PLFLT xn = (x - p->xmin) / (p->xmax - p->xmin) * p->basex - p->basex * 0.5f;
    PLFLT yn = (y - p->ymin) / (p->ymax - p->ymin) * p->basey - p->basey * 0.5f;
    PLFLT zn = (z - p->zmin) / (p->zmax - p->zmin) * p->height;

    return xn * p->saz * p->calt - yn * p->caz * p->calt - zn * p->salt;
}

/** Project four 3D corners and fill the resulting 2D quadrilateral with
 *  @fill, then stroke its outline with @stroke. */
static void
fill_3d_quad (
        const Pn3DProj *p,
        const PLFLT     x[4],
        const PLFLT     y[4],
        const PLFLT     z[4],
        const PnColor  *fill,
        const PnColor  *stroke)
{
    PLFLT xs[4], ys[4];
    PLFLT ox[5], oy[5];
    guint i;

    for (i = 0; i < 4; i++)
        proj_xy (p, x[i], y[i], z[i], &xs[i], &ys[i]);

    paint_set_cmap0 (3, fill);
    plcol0 (3);
    plfill (4, xs, ys);

    for (i = 0; i < 4; i++)
    {
        ox[i] = xs[i];
        oy[i] = ys[i];
    }
    ox[4] = xs[0];
    oy[4] = ys[0];

    paint_set_cmap0 (4, stroke);
    plcol0 (4);
    plline (5, ox, oy);
}

/* ------------------------------------------------------------------ */
/*  Bar sorting helper                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    PLFLT    xl, xr;        /* X (value) bin edges */
    PLFLT    y0, y1;        /* Y (series) ribbon edges */
    PLFLT    zb, zt;        /* Z (count) base and top */
    guint    series_idx;    /* index into views[] for colour lookup */
    PLFLT    depth;         /* center-depth for back-to-front sort */
} PnGraph3DBar;

static gint
bar_depth_cmp (
        gconstpointer a,
        gconstpointer b)
{
    const PnGraph3DBar *pa = a;
    const PnGraph3DBar *pb = b;
    if (pa->depth < pb->depth) return  1;
    if (pa->depth > pb->depth) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Line-mode painters                                                 */
/* ------------------------------------------------------------------ */

/* PLplot plpoin glyph used for the Points draw-style: a filled circle. */
#define PN_GRAPH_POINT_SYMBOL  17

/** Draw @n (@xs, @ys) points using the active draw-style. */
static void
draw_series_2d (
        const PnGraphPaintState *ps,
        guint    n,
        PLFLT   *xs,
        PLFLT   *ys,
        PLFLT    baseline,
        PLFLT    bar_hw)
{
    guint i;

    switch (ps->style)
    {
    case PN_GRAPH_STYLE_POINTS:
        plcol0 (1);
        plssym (0.0, 1.5 + 0.5 * (gdouble) (ps->line_width - 1));
        plpoin ((PLINT) n, xs, ys, PN_GRAPH_POINT_SYMBOL);
        break;

    case PN_GRAPH_STYLE_BARS:
        for (i = 0; i < n; i++)
        {
            PLFLT fx[4] = { xs[i] - bar_hw, xs[i] + bar_hw,
                            xs[i] + bar_hw, xs[i] - bar_hw };
            PLFLT fy[4] = { baseline, baseline, ys[i], ys[i] };

            plcol0 (1);
            plfill (4, fx, fy);

            plcol0  (2);
            plwidth (1);
            {
                PLFLT ox[5] = { fx[0], fx[1], fx[2], fx[3], fx[0] };
                PLFLT oy[5] = { fy[0], fy[1], fy[2], fy[3], fy[0] };
                plline (5, ox, oy);
            }
        }
        break;

    case PN_GRAPH_STYLE_LINES:
    default:
        plcol0  (1);
        plwidth ((PLINT) ps->line_width);
        plline  ((PLINT) n, xs, ys);
        break;
    }
}

/* Trend colours for the Error-bars draw-style. */
static const PnColor pn_graph_trend_up   = { 0.18, 0.70, 0.34, 1.0 };
static const PnColor pn_graph_trend_down = { 0.85, 0.27, 0.27, 1.0 };
static const PnColor pn_graph_trend_flat = { 0.55, 0.55, 0.55, 1.0 };

/** Draw @n buckets as box-and-whisker glyphs for the Error-bars
 *  draw-style. */
static void
draw_error_bars_2d (
        const PnGraphPaintState *ps,
        guint        n,
        const PLFLT *xs,
        const PLFLT *mean,
        const PLFLT *sd_lo,
        const PLFLT *sd_hi,
        const PLFLT *w_lo,
        const PLFLT *w_hi,
        PLFLT        box_hw)
{
    const PLFLT cap_hw = box_hw * 0.55;
    guint       i;

    paint_set_cmap0 (5, &pn_graph_trend_up);
    paint_set_cmap0 (6, &pn_graph_trend_down);
    paint_set_cmap0 (7, &pn_graph_trend_flat);

    for (i = 0; i < n; i++)
    {
        const PLFLT x = xs[i];
        PLINT       col;

        if (i == 0 || mean[i] == mean[i - 1])
            col = 7;
        else
            col = (mean[i] > mean[i - 1]) ? 5 : 6;

        plcol0  (col);
        plwidth ((PLINT) ps->line_width);
        {
            PLFLT wx[2]   = { x, x };
            PLFLT wy[2]   = { w_lo[i], w_hi[i] };
            PLFLT capx[2] = { x - cap_hw, x + cap_hw };
            PLFLT lo[2]   = { w_lo[i], w_lo[i] };
            PLFLT hi[2]   = { w_hi[i], w_hi[i] };

            plline (2, wx, wy);
            plline (2, capx, lo);
            plline (2, capx, hi);
        }

        {
            PLFLT bx[4] = { x - box_hw, x + box_hw, x + box_hw, x - box_hw };
            PLFLT by[4] = { sd_lo[i], sd_lo[i], sd_hi[i], sd_hi[i] };
            PLFLT ox[5] = { bx[0], bx[1], bx[2], bx[3], bx[0] };
            PLFLT oy[5] = { by[0], by[1], by[2], by[3], by[0] };

            plcol0  (col);
            plfill  (4, bx, by);

            plcol0  (2);
            plwidth (1);
            plline  (5, ox, oy);
        }

        {
            PLFLT mx[2] = { x - box_hw, x + box_hw };
            PLFLT my[2] = { mean[i], mean[i] };

            plcol0  (2);
            plwidth ((PLINT) ps->line_width);
            plline  (2, mx, my);
        }
    }
}

/** 2D time-series painter — single-series fast path. */
static void
paint_timeseries_2d (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t             *cr,
        double               w,
        double               h,
        gint64               width,
        const PnGraphSeries *s)
{
    PLFLT     xs[PN_GRAPH_MAX_BINS];
    PLFLT     ys[PN_GRAPH_MAX_BINS];
    PLFLT     e_sdlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_sdhi[PN_GRAPH_MAX_BINS];
    PLFLT     e_wlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_whi[PN_GRAPH_MAX_BINS];
    gboolean  error_bars = (ps->style == PN_GRAPH_STYLE_ERROR_BARS);
    guint     n_bins            = ps->n_bins;
    guint     n;
    gint64    cur_epoch         = g_get_monotonic_time () / width;
    gint64    oldest_valid      = cur_epoch - (gint64) n_bins + 1;
    gdouble   ymin = 0.0, ymax = 1.0;
    gboolean  have_range = FALSE;

    if (error_bars)
        n = collect_error_bars (s, n_bins, width, cur_epoch, oldest_valid,
                                xs, ys, e_sdlo, e_sdhi, e_wlo, e_whi,
                                &ymin, &ymax, &have_range);
    else
        n = collect_line_points (s, n_bins, width, cur_epoch, oldest_valid,
                                 xs, ys, &ymin, &ymax, &have_range);

    if (n < 1 || !have_range)
        return;

    if (ymax - ymin < 1e-9)
    {
        gdouble pad = (fabs (ymax) > 1e-9) ? fabs (ymax) * 0.05 : 0.5;
        ymin -= pad;
        ymax += pad;
    }
    else
    {
        gdouble pad = (ymax - ymin) * 0.05;
        ymin -= pad;
        ymax += pad;
    }

    if (ps->y_from_zero)
    {
        if (ymin > 0.0) ymin = 0.0;
        if (ymax < 0.0) ymax = 0.0;
    }

    paint_setup_page (plstream, ps, cr, w, h);

    plvpor (0.10, 0.97, 0.18, 0.93);
    plwind ((PLFLT) -(gdouble) pn_graph_resolution_seconds (ps->resolution),
            0.0f,
            (PLFLT) ymin, (PLFLT) ymax);

    plslabelfunc (graph_label_format, (PLPointer) ps);
    plcol0  (2);
    plwidth (1);
    {
        const char *xopts = ps->show_grid ? "bcgnsto" : "bcnsto";
        const char *yopts = ps->show_grid ? "bcgnsto" : "bcnsto";
        plbox (xopts, 0.0, 0,
               yopts, 0.0, 0);
    }
    plslabelfunc (NULL, NULL);

    {
        const PLFLT baseline = (ymin <= 0.0 && 0.0 <= ymax)
                             ? 0.0f : (PLFLT) ymin;
        const PLFLT bar_hw   = (PLFLT) ((gdouble) width
                                        / (gdouble) G_TIME_SPAN_SECOND
                                        * 0.40);
        if (error_bars)
            draw_error_bars_2d (ps, n, xs, ys, e_sdlo, e_sdhi,
                                e_wlo, e_whi, bar_hw);
        else
            draw_series_2d (ps, n, xs, ys, baseline, bar_hw);
    }

    pleop  ();
    plend1 ();
}

/** 3D line painter — one polyline per series, stacked along Y. */
static void
paint_line_3d (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t                 *cr,
        double                   w,
        double                   h,
        gint64                   width,
        const PnGraphSeriesView *views,
        guint                    nseries)
{
    guint     n_bins            = ps->n_bins;
    gint64    cur_epoch         = g_get_monotonic_time () / width;
    gint64    oldest_valid      = cur_epoch - (gint64) n_bins + 1;
    gdouble   zmin = 0.0, zmax = 1.0;
    gboolean  have_range = FALSE;
    guint     i;

    PLFLT  *all_xs = g_new (PLFLT, nseries * n_bins);
    PLFLT  *all_ys = g_new (PLFLT, nseries * n_bins);
    PLFLT  *all_zs = g_new (PLFLT, nseries * n_bins);
    guint  *counts = g_new0 (guint, nseries);

    for (i = 0; i < nseries; i++)
    {
        PLFLT *xs = all_xs + i * n_bins;
        PLFLT *zs = all_zs + i * n_bins;
        PLFLT *ys = all_ys + i * n_bins;
        guint  n;
        guint  j;

        n = collect_line_points (views[i].series, n_bins, width, cur_epoch,
                                 oldest_valid, xs, zs,
                                 &zmin, &zmax, &have_range);
        counts[i] = n;
        for (j = 0; j < n; j++)
            ys[j] = (PLFLT) i;
    }

    if (!have_range)
    {
        g_free (all_xs); g_free (all_ys); g_free (all_zs); g_free (counts);
        return;
    }

    if (zmax - zmin < 1e-9)
    {
        gdouble pad = (fabs (zmax) > 1e-9) ? fabs (zmax) * 0.05 : 0.5;
        zmin -= pad;
        zmax += pad;
    }
    else
    {
        gdouble pad = (zmax - zmin) * 0.05;
        zmin -= pad;
        zmax += pad;
    }

    paint_setup_page (plstream, ps, cr, w, h);

    plvpor (0.05, 0.97, 0.10, 0.95);

    plwind (-1.0f, 1.0f, -0.9f, 1.1f);

    plw3d (1.0, 0.8, 1.0,
           (PLFLT) -(gdouble) pn_graph_resolution_seconds (ps->resolution),
           0.0f,
           -0.5f, (PLFLT) nseries - 0.5f,
           (PLFLT) zmin, (PLFLT) zmax,
           PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

    plslabelfunc (graph_label_format, (PLPointer) ps);
    plcol0  (2);
    plwidth (1);
    {
        const char *xopts = ps->show_grid ? "bnstugo" : "bnstuo";
        const char *yopts = "bnstu";
        const char *zopts = ps->show_grid ? "bcdmnstuvo" : "bcdmnstuvo";
        plbox3 (xopts, "", 0.0, 0,
                yopts, "", 1.0, 0,
                zopts, "", 0.0, 0);
    }
    plslabelfunc (NULL, NULL);

    plwidth ((PLINT) ps->line_width);
    for (i = 0; i < nseries; i++)
    {
        PnColor c;
        if (counts[i] < 2)
            continue;

        color_for_series (ps, i, &c);
        paint_set_cmap0 (8 + (PLINT) i, &c);
        plcol0 (8 + (PLINT) i);

        plline3 ((PLINT) counts[i],
                 all_xs + i * n_bins,
                 all_ys + i * n_bins,
                 all_zs + i * n_bins);
    }

    g_free (all_xs); g_free (all_ys); g_free (all_zs); g_free (counts);

    pleop  ();
    plend1 ();
}

/* ------------------------------------------------------------------ */
/*  Histogram-mode painters                                            */
/* ------------------------------------------------------------------ */

/** 2D distribution painter — single-series fast path. */
static void
paint_distribution_2d (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t             *cr,
        double               w,
        double               h,
        const PnGraphSeries *s)
{
    PLFLT    data[PN_GRAPH_SAMPLES];
    guint    n;
    guint    i;
    gint64   now_us  = g_get_monotonic_time ();
    gint64   cutoff  = now_us
                    - (gint64) pn_graph_resolution_seconds (ps->resolution)
                    * G_TIME_SPAN_SECOND;
    gdouble  vmin = 0.0, vmax = 0.0;
    gboolean have_range = FALSE;
    gdouble  bin_w;
    guint    counts[PN_GRAPH_HIST_BINS] = { 0 };
    guint    max_count = 0;

    n = collect_window_values (s, cutoff, data, PN_GRAPH_SAMPLES,
                               &vmin, &vmax, &have_range);

    if (n < 2 || !have_range)
        return;

    if (vmax - vmin < 1e-9)
    {
        gdouble pad = (fabs (vmax) > 1e-9) ? fabs (vmax) * 0.05 : 0.5;
        vmin -= pad;
        vmax += pad;
    }

    bin_w = (vmax - vmin) / (gdouble) PN_GRAPH_HIST_BINS;

    for (i = 0; i < n; i++)
    {
        gdouble  rel = (data[i] - vmin) / bin_w;
        gint     b   = (gint) floor (rel);
        if (b < 0)                          b = 0;
        if (b >= (gint) PN_GRAPH_HIST_BINS) b = PN_GRAPH_HIST_BINS - 1;
        counts[b] += 1;
        if (counts[b] > max_count)
            max_count = counts[b];
    }

    paint_setup_page (plstream, ps, cr, w, h);

    {
        const PLFLT ybot = ps->log_y
                         ? (PLFLT) log10 (0.5)
                         : 0.0f;
        const PLFLT ytop = ps->log_y
                         ? (PLFLT) log10 ((gdouble) max_count * 1.1 + 1.0)
                         : (PLFLT) ((gdouble) max_count * 1.1 + 1.0);

        plvpor (0.10, 0.97, 0.18, 0.93);
        plwind ((PLFLT) vmin, (PLFLT) vmax, ybot, ytop);

        plslabelfunc (graph_label_format, (PLPointer) ps);
        plcol0  (2);
        plwidth (1);
        {
            const char *xopts = ps->show_grid ? "bcgnsto" : "bcnsto";
            const char *yopts;
            if (ps->log_y)
                yopts = ps->show_grid ? "bcgnstl"  : "bcnstl";
            else
                yopts = ps->show_grid ? "bcgnsto" : "bcnsto";
            plbox (xopts, 0.0, 0,
                   yopts, 0.0, 0);
        }
        plslabelfunc (NULL, NULL);

        if (ps->style == PN_GRAPH_STYLE_BARS
            || ps->style == PN_GRAPH_STYLE_ERROR_BARS)
        {
            for (i = 0; i < PN_GRAPH_HIST_BINS; i++)
            {
                PLFLT xl, xr, yt;
                PLFLT xs[4], ys[4];

                if (counts[i] == 0)
                    continue;

                xl = (PLFLT) (vmin + (gdouble) i * bin_w);
                xr = (PLFLT) (vmin + (gdouble) (i + 1) * bin_w);
                yt = ps->log_y
                   ? (PLFLT) log10 ((gdouble) counts[i])
                   : (PLFLT) counts[i];

                xs[0] = xl; ys[0] = ybot;
                xs[1] = xr; ys[1] = ybot;
                xs[2] = xr; ys[2] = yt;
                xs[3] = xl; ys[3] = yt;

                plcol0  (1);
                plfill  (4, xs, ys);

                plcol0  (2);
                plwidth (1);
                {
                    PLFLT ox[5] = { xl, xr, xr, xl, xl };
                    PLFLT oy[5] = { ybot, ybot, yt, yt, ybot };
                    plline (5, ox, oy);
                }
            }
        }
        else
        {
            PLFLT  cx[PN_GRAPH_HIST_BINS];
            PLFLT  cy[PN_GRAPH_HIST_BINS];
            guint  m = 0;

            for (i = 0; i < PN_GRAPH_HIST_BINS; i++)
            {
                if (ps->style == PN_GRAPH_STYLE_POINTS && counts[i] == 0)
                    continue;

                cx[m] = (PLFLT) (vmin + ((gdouble) i + 0.5) * bin_w);
                cy[m] = (counts[i] > 0)
                      ? (ps->log_y ? (PLFLT) log10 ((gdouble) counts[i])
                                   : (PLFLT) counts[i])
                      : ybot;
                m += 1;
            }

            if (ps->style == PN_GRAPH_STYLE_POINTS)
            {
                plcol0 (1);
                plssym (0.0, 1.5 + 0.5 * (gdouble) (ps->line_width - 1));
                plpoin ((PLINT) m, cx, cy, PN_GRAPH_POINT_SYMBOL);
            }
            else
            {
                plcol0  (1);
                plwidth ((PLINT) ps->line_width);
                plline  ((PLINT) m, cx, cy);
            }
        }
    }

    pleop  ();
    plend1 ();
}

/** 3D histogram painter — one ribbon of bars per series. */
static void
paint_histogram_3d (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t                 *cr,
        double                   w,
        double                   h,
        const PnGraphSeriesView *views,
        guint                    nseries)
{
    gint64   now_us = g_get_monotonic_time ();
    gint64   cutoff = now_us
                  - (gint64) pn_graph_resolution_seconds (ps->resolution)
                  * G_TIME_SPAN_SECOND;
    gdouble  vmin = 0.0, vmax = 0.0;
    gboolean have_range = FALSE;
    guint    i, j;
    PLFLT   *per_series_data = g_new (PLFLT, nseries * PN_GRAPH_SAMPLES);
    guint   *per_series_n    = g_new0 (guint, nseries);
    guint   *all_counts      = g_new0 (guint, nseries * PN_GRAPH_3D_HIST_BINS);
    guint    max_count       = 0;
    gdouble  bin_w;

    for (i = 0; i < nseries; i++)
    {
        per_series_n[i] = collect_window_values (views[i].series, cutoff,
                                                 per_series_data + i * PN_GRAPH_SAMPLES,
                                                 PN_GRAPH_SAMPLES,
                                                 &vmin, &vmax, &have_range);
    }

    if (!have_range)
    {
        g_free (per_series_data); g_free (per_series_n); g_free (all_counts);
        return;
    }

    if (vmax - vmin < 1e-9)
    {
        gdouble pad = (fabs (vmax) > 1e-9) ? fabs (vmax) * 0.05 : 0.5;
        vmin -= pad;
        vmax += pad;
    }

    bin_w = (vmax - vmin) / (gdouble) PN_GRAPH_3D_HIST_BINS;

    for (i = 0; i < nseries; i++)
    {
        guint  n  = per_series_n[i];
        PLFLT *vs = per_series_data + i * PN_GRAPH_SAMPLES;
        guint *cs = all_counts      + i * PN_GRAPH_3D_HIST_BINS;

        for (j = 0; j < n; j++)
        {
            gdouble rel = ((gdouble) vs[j] - vmin) / bin_w;
            gint    b   = (gint) floor (rel);
            if (b < 0)                             b = 0;
            if (b >= (gint) PN_GRAPH_3D_HIST_BINS) b = PN_GRAPH_3D_HIST_BINS - 1;
            cs[b] += 1;
            if (cs[b] > max_count)
                max_count = cs[b];
        }
    }

    if (max_count == 0)
    {
        g_free (per_series_data); g_free (per_series_n); g_free (all_counts);
        return;
    }

    paint_setup_page (plstream, ps, cr, w, h);

    {
        const PLFLT  zbot = ps->log_y
                          ? (PLFLT) log10 (0.5)
                          : 0.0f;
        const PLFLT  ztop = ps->log_y
                          ? (PLFLT) log10 ((gdouble) max_count * 1.1 + 1.0)
                          : (PLFLT) ((gdouble) max_count * 1.1 + 1.0);
        const PLFLT  ymin = -0.5f;
        const PLFLT  ymax = (PLFLT) nseries - 0.5f;
        Pn3DProj     proj;
        GArray      *bars;
        guint        b;

        plvpor (0.05, 0.97, 0.10, 0.95);
        plwind (-1.0f, 1.0f, -0.9f, 1.1f);
        plw3d (1.0, 0.6, 1.0,
               (PLFLT) vmin, (PLFLT) vmax,
               ymin, ymax,
               zbot, ztop,
               PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

        proj_init (&proj, 1.0, 0.6, 1.0,
                   (PLFLT) vmin, (PLFLT) vmax,
                   ymin, ymax,
                   zbot, ztop,
                   PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

        {
            const PnColor floor = { 0.92, 0.92, 0.92, 1.0 };
            PLFLT fx[4] = { (PLFLT) vmin, (PLFLT) vmax,
                            (PLFLT) vmax, (PLFLT) vmin };
            PLFLT fy[4] = { ymin, ymin, ymax, ymax };
            PLFLT fz[4] = { zbot, zbot, zbot, zbot };
            PLFLT sx[4], sy[4];
            guint k;

            for (k = 0; k < 4; k++)
                proj_xy (&proj, fx[k], fy[k], fz[k], &sx[k], &sy[k]);
            paint_set_cmap0 (5, &floor);
            plcol0 (5);
            plfill (4, sx, sy);
        }

        plslabelfunc (graph_label_format, (PLPointer) ps);
        plcol0  (2);
        plwidth (1);
        {
            const char *xopts = ps->show_grid ? "bcnstugo" : "bcnstuo";
            const char *yopts = ps->show_grid ? "bcnstug"  : "bcnstu";
            const char *zopts;
            if (ps->log_y)
                zopts = ps->show_grid ? "bcdnstuvl" : "bcdnstuvl";
            else
                zopts = ps->show_grid ? "bcdnstuvo" : "bcdnstuvo";
            plbox3 (xopts, "", 0.0, 0,
                    yopts, "", 1.0, 0,
                    zopts, "", 0.0, 0);
        }
        plslabelfunc (NULL, NULL);

        bars = g_array_new (FALSE, FALSE, sizeof (PnGraph3DBar));
        for (i = 0; i < nseries; i++)
        {
            const PLFLT y0 = (PLFLT) i - (PLFLT) PN_GRAPH_3D_RIBBON_HALF;
            const PLFLT y1 = (PLFLT) i + (PLFLT) PN_GRAPH_3D_RIBBON_HALF;
            const gdouble gap_half = (1.0 - PN_GRAPH_3D_BAR_FILL) * 0.5;
            guint      *cs = all_counts + i * PN_GRAPH_3D_HIST_BINS;

            for (j = 0; j < PN_GRAPH_3D_HIST_BINS; j++)
            {
                PnGraph3DBar bar;
                PLFLT        xc, yc;

                if (cs[j] == 0)
                    continue;

                bar.xl = (PLFLT) (vmin + ((gdouble) j + gap_half) * bin_w);
                bar.xr = (PLFLT) (vmin + ((gdouble) (j + 1) - gap_half) * bin_w);
                bar.y0 = y0;
                bar.y1 = y1;
                bar.zb = zbot;
                bar.zt = ps->log_y
                       ? (PLFLT) log10 ((gdouble) cs[j])
                       : (PLFLT) cs[j];
                bar.series_idx = i;

                xc = (bar.xl + bar.xr) * 0.5f;
                yc = (bar.y0 + bar.y1) * 0.5f;
                bar.depth = proj_depth (&proj, xc, yc,
                                        (bar.zb + bar.zt) * 0.5f);

                g_array_append_val (bars, bar);
            }
        }

        g_array_sort (bars, bar_depth_cmp);

        plwidth (1);
        for (b = 0; b < bars->len; b++)
        {
            const PnGraph3DBar *bar = &g_array_index (bars, PnGraph3DBar, b);
            PnColor             base;
            PnColor             c_top, c_yface, c_right;

            color_for_series (ps, bar->series_idx, &base);
            c_top   = shade_l (base, 0.75);
            c_yface = shade_l (base, 0.55);
            c_right = shade_l (base, 0.40);

            /* Right face (x = xr). */
            {
                PLFLT fx[4] = { bar->xr, bar->xr, bar->xr, bar->xr };
                PLFLT fy[4] = { bar->y0, bar->y1, bar->y1, bar->y0 };
                PLFLT fz[4] = { bar->zb, bar->zb, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_right, &ps->axis_color);
            }
            /* Y-max face (y = y1). */
            {
                PLFLT fx[4] = { bar->xl, bar->xr, bar->xr, bar->xl };
                PLFLT fy[4] = { bar->y1, bar->y1, bar->y1, bar->y1 };
                PLFLT fz[4] = { bar->zb, bar->zb, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_yface, &ps->axis_color);
            }
            /* Top face (z = zt). */
            {
                PLFLT fx[4] = { bar->xl, bar->xr, bar->xr, bar->xl };
                PLFLT fy[4] = { bar->y0, bar->y0, bar->y1, bar->y1 };
                PLFLT fz[4] = { bar->zt, bar->zt, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_top, &ps->axis_color);
            }
        }

        g_array_unref (bars);
    }

    g_free (per_series_data); g_free (per_series_n); g_free (all_counts);

    pleop  ();
    plend1 ();
}

/* ------------------------------------------------------------------ */
/*  Top-level dispatch                                                 */
/* ------------------------------------------------------------------ */

static void
pn_graph_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnGraph           *self    = PN_GRAPH (node);
    PnGraphSeriesView *views   = NULL;
    guint              nseries = 0;
    PnGraphPaintState  ps;
    PLINT              plstream;
    gint64             width;

    pn_graph_get_paint_state (self, &ps);

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

    views = pn_graph_collect_series_sorted (self, &nseries);
    if (nseries == 0)
        return;

    plstream = pn_graph_plstream (self);
    width    = pn_graph_bin_width_us (self);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    switch (ps.view)
    {
    case PN_GRAPH_VIEW_DISTRIBUTION:
        if (nseries == 1)
            paint_distribution_2d (plstream, &ps, cr, w, h, views[0].series);
        else
            paint_histogram_3d (plstream, &ps, cr, w, h, views, nseries);
        break;
    case PN_GRAPH_VIEW_TIME_SERIES:
    default:
        if (nseries == 1)
            paint_timeseries_2d (plstream, &ps, cr, w, h, width,
                                 views[0].series);
        else
            paint_line_3d (plstream, &ps, cr, w, h, width, views, nseries);
        break;
    }

    cairo_restore (cr);

    g_free (views);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog                                                    */
/*                                                                     */
/*  The two-page grouping (Appearance | Data) is now declared as a     */
/*  GTK-free #PnSettingsSchema on the class in the core pn-graph.c      */
/*  (Phase 7.4); the dialog framework's renderer builds the same       */
/*  notebook pages this file's build_class_tabs used to.  This file    */
/*  is the graph's painter only.                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_graph_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_GRAPH));

    node_class->paint_plot       = pn_graph_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
