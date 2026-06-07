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
/*  PnPlot — gui tier.                                                  */
/*                                                                     */
/*  The PLplot/cairo painter for the vector-fed plot.  The node's       */
/*  GType, properties, receive() logic and the vector→bucket binning    */
/*  live in the GTK-free core pn-plot.c; this file installs the drawing  */
/*  vfunc slot onto that class at editor startup (pn_plot_gui_install).  */
/*                                                                       */
/*  PnPlot fills the very same #PnGraphBin rings PnGraph does, so this    */
/*  painter does NOT re-implement the renderers: it reuses PnGraph's     */
/*  exposed draw path (pn-graph-draw.h) — the ring extractors            */
/*  (pn_graph_collect_line_points / pn_graph_collect_error_bars, the     */
/*  latter carrying the neighbour-reaching whisker fix) and the 2D       */
/*  draw-style renderers — so every draw-style and the Distribution view */
/*  render byte-for-byte like PnGraph.  The only difference is the X      */
/*  axis: PnPlot anchors the buckets at fixed indices 0..M-1 (not        */
/*  PnGraph's live wall-clock epoch), so the snapshot it binned at        */
/*  receive time draws at a stable position.                             */
/*                                                                       */
/*  The per-instance PLplot stream is owned entirely here (allocated     */
/*  lazily on first paint, torn down via a GObject data destroy-notify),  */
/*  so the core never references PLplot.                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-plot-gui.h"
#include "pn-plot.h"
#include "pn-graph-draw.h"

#include <gtk/gtk.h>
#include <math.h>
#include <plplot/plplot.h>

/* GObject data key holding the per-instance PLplot stream id (boxed in a
 * heap PLINT so a destroy-notify can run plend1 at finalize). */
#define PN_PLOT_PLSTREAM_KEY  "pn-plot-plstream"

/* ------------------------------------------------------------------ */
/*  Per-instance PLplot stream (mirrors pn-graph-gui.c)                 */
/* ------------------------------------------------------------------ */

static void
pn_plot_plstream_free (gpointer data)
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
pn_plot_plstream (PnPlot *self)
{
    PLINT *id = g_object_get_data (G_OBJECT (self), PN_PLOT_PLSTREAM_KEY);

    if (id == NULL)
    {
        id = g_new0 (PLINT, 1);
        plmkstrm (id);
        g_object_set_data_full (G_OBJECT (self), PN_PLOT_PLSTREAM_KEY,
                                id, pn_plot_plstream_free);
    }

    return *id;
}

/* ------------------------------------------------------------------ */
/*  X-axis tick labels                                                 */
/* ------------------------------------------------------------------ */

/** PnPlot's X axis is the bucket index, so a tick value is just printed
 *  as a plain number (PnGraph's formatter would render it as elapsed
 *  time, which is meaningless here). */
static void
plot_label_format (
        PLINT     axis,
        PLFLT     value,
        char     *label,
        PLINT     length,
        PLPointer user)
{
    (void) axis;
    (void) user;
    g_snprintf (label, length, "%g", (gdouble) value);
}

/* ------------------------------------------------------------------ */
/*  Time-series painter (bucket-indexed X)                             */
/* ------------------------------------------------------------------ */

/** Draw the binned series as the active draw-style, with the buckets
 *  anchored at fixed X indices 0..n-1.  This is the bucket-X counterpart
 *  of PnGraph's paint_timeseries_2d: it reuses the exact same ring
 *  extractors and draw-style renderers (pn-graph-draw.h), only the X
 *  framing differs. */
static void
paint_buckets_2d (
        PLINT                    plstream,
        const PnGraphPaintState *ps,
        cairo_t                 *cr,
        double                   w,
        double                   h,
        const PnGraphSeries     *s)
{
    PLFLT     xs[PN_GRAPH_MAX_BINS];
    PLFLT     ys[PN_GRAPH_MAX_BINS];
    PLFLT     e_sdlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_sdhi[PN_GRAPH_MAX_BINS];
    PLFLT     e_wlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_whi[PN_GRAPH_MAX_BINS];
    gboolean  error_bars = (ps->style == PN_GRAPH_STYLE_ERROR_BARS);
    guint     n_bins     = ps->n_bins;
    guint     n;
    guint     i;
    /* Buckets are stored in slots 0..M-1 with epoch == slot, so handing
     * the ring walker cur_epoch = M-1 and oldest_valid = 0 reads them all
     * back in order.  The bin width only scales the seconds-ago X that the
     * extractor computes — which we discard and replace with the bucket
     * index — so any positive value works. */
    gint64    width        = G_TIME_SPAN_SECOND;
    gint64    cur_epoch    = (gint64) n_bins - 1;
    gint64    oldest_valid = 0;
    gdouble   ymin = 0.0, ymax = 1.0;
    gboolean  have_range = FALSE;

    if (error_bars)
        n = pn_graph_collect_error_bars (s, n_bins, width, cur_epoch,
                                         oldest_valid, xs, ys,
                                         e_sdlo, e_sdhi, e_wlo, e_whi,
                                         &ymin, &ymax, &have_range);
    else
        n = pn_graph_collect_line_points (s, n_bins, width, cur_epoch,
                                          oldest_valid, xs, ys,
                                          &ymin, &ymax, &have_range);

    if (n < 1 || !have_range)
        return;

    /* Re-anchor X at the bucket index (0..n-1): one world unit per bucket,
     * so the bar/box half-width below is a fixed 0.4 units. */
    for (i = 0; i < n; i++)
        xs[i] = (PLFLT) i;

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

    pn_graph_paint_setup_page (plstream, ps, cr, w, h);

    plvpor (0.10, 0.97, 0.18, 0.93);
    plwind (-0.5f, (PLFLT) n - 0.5f, (PLFLT) ymin, (PLFLT) ymax);

    plslabelfunc (plot_label_format, NULL);
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
        const PLFLT bar_hw   = 0.40f;   /* one bucket = one world unit */

        if (error_bars)
            pn_graph_draw_error_bars_2d (ps, n, xs, ys, e_sdlo, e_sdhi,
                                         e_wlo, e_whi, bar_hw);
        else
            pn_graph_draw_series_2d (ps, n, xs, ys, baseline, bar_hw);
    }

    pleop  ();
    plend1 ();
}

/* ------------------------------------------------------------------ */
/*  Top-level dispatch                                                 */
/* ------------------------------------------------------------------ */

static void
pn_plot_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnPlot              *self = PN_PLOT (node);
    const PnGraphSeries *s;
    PnGraphPaintState    ps;
    PLINT                plstream;

    pn_plot_get_paint_state (self, &ps);

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

    s = pn_plot_peek_series (self);
    if (s == NULL)
        return;

    plstream = pn_plot_plstream (self);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    switch (ps.view)
    {
    case PN_GRAPH_VIEW_DISTRIBUTION:
        pn_graph_paint_distribution_2d (plstream, &ps, cr, w, h, s);
        break;
    case PN_GRAPH_VIEW_TIME_SERIES:
    default:
        paint_buckets_2d (plstream, &ps, cr, w, h, s);
        break;
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_plot_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_PLOT));

    node_class->paint_plot = pn_plot_paint_plot;

    /* The class ref is intentionally held for the process lifetime — the
     * same lifetime the factory keeps it alive for — so the slot we just
     * wrote stays valid.  (One leaked ref on a singleton class, mirroring
     * pn_graph_gui_install.) */
}
