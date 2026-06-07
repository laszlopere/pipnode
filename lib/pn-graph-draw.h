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

#ifndef PN_GRAPH_DRAW_H
#define PN_GRAPH_DRAW_H

/* ------------------------------------------------------------------ */
/*  Shared PLplot drawing + ring-extraction helpers (gui tier).        */
/*                                                                     */
/*  These are the reusable building blocks of the PnGraph painter      */
/*  (pn-graph-gui.c): the common PLplot page setup, the two ring       */
/*  extractors (line points and error-bar box-and-whisker, including   */
/*  the neighbour-reaching whisker fix), the three 2D draw-style        */
/*  renderers, and the single-series distribution painter.  They are    */
/*  pure PLplot/cairo code operating on the GTK-free #PnGraphSeries /    */
/*  #PnGraphPaintState data published in pn-graph.h, so the vector-fed   */
/*  sibling node #PnPlot (pn-plot-gui.c) draws "exactly like PnGraph"    */
/*  by calling them with a bucket-indexed X axis instead of PnGraph's    */
/*  wall-clock time one.  Only the gui tier ever includes this header    */
/*  (it pulls in PLplot); the headless core never does.                 */
/* ------------------------------------------------------------------ */

#include "pn-graph.h"

#include <plplot/plplot.h>

G_BEGIN_DECLS

/** Common PLplot setup shared by every 2D/3D painter: select the
 *  per-node stream, bind the extcairo device to @cr, size the page to
 *  @w×@h, and pin cmap0 slots 1 (line colour) and 2 (axis colour). */
void pn_graph_paint_setup_page (PLINT                    plstream,
                                const PnGraphPaintState *ps,
                                cairo_t                 *cr,
                                double                   w,
                                double                   h);

/** Walk @s's bin ring and emit (seconds_ago, mean_value) pairs in
 *  left-to-right time order, tracking the Y range.  Returns the count. */
guint pn_graph_collect_line_points (const PnGraphSeries *s,
                                    guint                n_bins,
                                    gint64               width,
                                    gint64               cur_epoch,
                                    gint64               oldest_valid_epoch,
                                    PLFLT               *xs,
                                    PLFLT               *ys,
                                    gdouble             *inout_ymin,
                                    gdouble             *inout_ymax,
                                    gboolean            *have_range_io);

/** Like pn_graph_collect_line_points, but emits per-bucket box-and-
 *  whisker geometry for the Error-bars draw-style.  Includes the
 *  neighbour-reaching whisker fix (single-sample buckets stretch their
 *  whiskers halfway toward the adjacent buckets' means). */
guint pn_graph_collect_error_bars (const PnGraphSeries *s,
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
                                   gboolean            *have_range_io);

/** Draw @n (@xs, @ys) points with the active Points/Lines/Bars style. */
void pn_graph_draw_series_2d (const PnGraphPaintState *ps,
                              guint                    n,
                              PLFLT                   *xs,
                              PLFLT                   *ys,
                              PLFLT                    baseline,
                              PLFLT                    bar_hw);

/** Draw @n buckets as box-and-whisker glyphs (Error-bars style). */
void pn_graph_draw_error_bars_2d (const PnGraphPaintState *ps,
                                  guint                    n,
                                  const PLFLT             *xs,
                                  const PLFLT             *mean,
                                  const PLFLT             *sd_lo,
                                  const PLFLT             *sd_hi,
                                  const PLFLT             *w_lo,
                                  const PLFLT             *w_hi,
                                  PLFLT                    box_hw);

/** Single-series distribution painter: value-bin @s's in-window raw
 *  samples and draw the histogram (bars / polyline / points). */
void pn_graph_paint_distribution_2d (PLINT                    plstream,
                                     const PnGraphPaintState *ps,
                                     cairo_t                 *cr,
                                     double                   w,
                                     double                   h,
                                     const PnGraphSeries     *s);

G_END_DECLS

#endif /* PN_GRAPH_DRAW_H */
