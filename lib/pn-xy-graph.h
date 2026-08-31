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

#ifndef PN_XY_GRAPH_H
#define PN_XY_GRAPH_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnXYGraph                                                          */
/*                                                                     */
/*  Sink node that scatter-plots a (x, y) pair plucked from each       */
/*  incoming message.  Unlike #PnGraph — which bins one value against  */
/*  arrival time — both coordinates come from the message: the Y value */
/*  from the #key path (default data/value) and the X value from the   */
/*  #x-key path (default data/x).  The most recent points are kept in  */
/*  a per-topic ring (last N samples) and plotted as a scatter; both   */
/*  axes auto-range over the visible data.  The header is rendered in  */
/*  the standard Node-RED style (twice the canonical width); a PLplot- */
/*  rendered plot sits below it with a small gap.                      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_XY_GRAPH (pn_xy_graph_get_type ())

G_DECLARE_FINAL_TYPE (PnXYGraph, pn_xy_graph, PN, XY_GRAPH, PnNode)

/**
 * PnXYGraphStyle:
 * @PN_XY_GRAPH_STYLE_POINTS: Draw each (x, y) sample as a discrete marker.
 * @PN_XY_GRAPH_STYLE_LINES:  Connect the samples, in arrival order, with a
 *                            polyline (reads as a trajectory).
 *
 * Selects how each series is drawn.  Both forms are honoured for every
 * series in the 2D overlay.
 */
typedef enum
{
    PN_XY_GRAPH_STYLE_POINTS,
    PN_XY_GRAPH_STYLE_LINES,
} PnXYGraphStyle;

#define PN_TYPE_XY_GRAPH_STYLE (pn_xy_graph_style_get_type ())
GType pn_xy_graph_style_get_type (void) G_GNUC_CONST;

PnXYGraph *pn_xy_graph_new (void);

/* Number of distinct per-topic series currently tracked.  Read-only
 * inspection seam: receive() lazily creates one series per message
 * topic (up to PN_XY_GRAPH_MAX_SERIES), but the series table is
 * private, so headless tests observe series creation through this
 * accessor. */
guint pn_xy_graph_get_series_count (PnXYGraph *self);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The PLplot/cairo plot painter lives in the gui-tier file           */
/*  pn-xy-graph-gui.c, but it has to walk the same per-topic sample    */
/*  rings that receive() fills in the core file.  Those rings are      */
/*  plain data (no GTK / no PLplot), so the structs and the data-      */
/*  extraction accessors are published here and shared across the      */
/*  tier boundary.  The collected snapshot is owned by the caller      */
/*  (free with g_free); the pointed-to series structs and topic        */
/*  strings remain owned by the node and are valid for the duration of */
/*  the paint call.                                                    */
/* ------------------------------------------------------------------ */

/* Capacity of the per-series sample ring — the largest "last N points"
 * window the node can keep.  The painter sizes its stack scratch arrays
 * to this cap, so it lives in the public header alongside the sample
 * struct rather than only in the core .c. */
#define PN_XY_GRAPH_SAMPLES  2048

/* Cap on how many of the most recent samples per series are written into
 * the worksheet file when the "save-data" property is on.  With at most
 * PN_XY_GRAPH_MAX_SERIES series the persisted store is bounded, so a
 * graph left running for a month cannot grow the document without end. */
#define PN_XY_GRAPH_PERSIST_SAMPLES  512

/* One raw (x, y) observation kept verbatim. */
typedef struct
{
    gdouble x;
    gdouble y;
} PnXYGraphSample;

/* Per-topic series: its raw-sample ring + a stable arrival-order index
 * (used to seed the auto-assigned series colour). */
typedef struct
{
    PnXYGraphSample  samples[PN_XY_GRAPH_SAMPLES];
    guint            sample_head;
    guint            sample_count;

    guint            arrival_idx;
} PnXYGraphSeries;

/* A (topic, series) pair, used to walk the series in arrival order on
 * every paint.  Both pointers are borrowed from the node's hashtable. */
typedef struct
{
    const gchar           *topic;
    const PnXYGraphSeries  *series;
} PnXYGraphSeriesView;

/* Scalar drawing configuration, snapshotted by value for one paint.
 * The colours are #PnColor (layout-identical to GdkRGBA). */
typedef struct
{
    PnXYGraphStyle  style;
    guint           max_points;

    PnColor         line_color;
    PnColor         axis_color;
    PnColor         background_color;
    guint           line_width;
    gboolean        show_grid;
    gboolean        x_from_zero;
    gboolean        y_from_zero;
} PnXYGraphPaintState;

/**
 * pn_xy_graph_get_paint_state:
 * @self: graph instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_xy_graph_get_paint_state (PnXYGraph *self, PnXYGraphPaintState *out);

/**
 * pn_xy_graph_collect_series_sorted:
 * @self:  graph instance
 * @out_n: (out): number of series written
 *
 * Returns: (transfer container): a newly-allocated array of
 * #PnXYGraphSeriesView in arrival order (free with g_free), or %NULL
 * when no series exist.  The series structs and topic strings are owned
 * by the node.
 */
PnXYGraphSeriesView *pn_xy_graph_collect_series_sorted (PnXYGraph *self,
                                                        guint     *out_n);

/**
 * pn_xy_graph_series_chrono:
 * @s:     series whose ring to read
 * @limit: cap on how many of the most recent samples to return
 * @out_x: (out caller-allocates): X coordinates, oldest-first
 * @out_y: (out caller-allocates): Y coordinates, oldest-first
 *
 * Copies up to @limit of @s's most recent samples into @out_x / @out_y
 * in chronological (arrival) order — the order the Lines style connects
 * them.  @out_x and @out_y must each have room for
 * MIN(@limit, %PN_XY_GRAPH_SAMPLES) entries.
 *
 * Returns: the number of samples written.
 */
guint pn_xy_graph_series_chrono (const PnXYGraphSeries *s,
                                 guint                  limit,
                                 gdouble               *out_x,
                                 gdouble               *out_y);

G_END_DECLS

#endif /* PN_XY_GRAPH_H */
