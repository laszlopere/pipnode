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

#ifndef PN_GRAPH_H
#define PN_GRAPH_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnGraph                                                            */
/*                                                                     */
/*  Sink node that draws a sliding-time graph of a numeric value       */
/*  plucked from each incoming message.  The header is rendered in     */
/*  the standard Node-RED style (twice the canonical width); a PLplot- */
/*  rendered plot sits below it with a small gap.  X axis is time      */
/*  (most recent on the right); Y axis auto-ranges over the data       */
/*  visible in the configured window.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_GRAPH (pn_graph_get_type ())

G_DECLARE_FINAL_TYPE (PnGraph, pn_graph, PN, GRAPH, PnNode)

/**
 * PnGraphResolution:
 * @PN_GRAPH_RES_MINUTE:    60-second window.
 * @PN_GRAPH_RES_15_MINUTES: 15-minute window.
 * @PN_GRAPH_RES_HOUR:      1-hour window.
 * @PN_GRAPH_RES_DAY:       1-day window.
 * @PN_GRAPH_RES_WEEK:      7-day window.
 *
 * Controls the total time span shown on the X axis.  The window is
 * always split into a fixed number of bins so memory use stays
 * constant regardless of arrival rate.
 */
typedef enum
{
    PN_GRAPH_RES_MINUTE,
    PN_GRAPH_RES_15_MINUTES,
    PN_GRAPH_RES_HOUR,
    PN_GRAPH_RES_DAY,
    PN_GRAPH_RES_WEEK,
} PnGraphResolution;

#define PN_TYPE_GRAPH_RESOLUTION (pn_graph_resolution_get_type ())
GType pn_graph_resolution_get_type (void) G_GNUC_CONST;

/**
 * PnGraphView:
 * @PN_GRAPH_VIEW_TIME_SERIES:  Plot each value against time, most recent on
 *                              the right (X axis is time).
 * @PN_GRAPH_VIEW_DISTRIBUTION: Bin the in-window samples by value and plot
 *                              how often each value occurs (X axis is value,
 *                              Y axis is the count) — the shape reads as the
 *                              distribution of where the values cluster.
 *
 * Selects *what* the plot represents.  Orthogonal to #PnGraphStyle, which
 * selects *how* the chosen view is drawn.
 */
typedef enum
{
    PN_GRAPH_VIEW_TIME_SERIES,
    PN_GRAPH_VIEW_DISTRIBUTION,
} PnGraphView;

#define PN_TYPE_GRAPH_VIEW (pn_graph_view_get_type ())
GType pn_graph_view_get_type (void) G_GNUC_CONST;

/**
 * PnGraphStyle:
 * @PN_GRAPH_STYLE_POINTS: Draw each data point as a discrete marker.
 * @PN_GRAPH_STYLE_LINES:  Connect the data points with a polyline.
 * @PN_GRAPH_STYLE_BARS:   Draw each data point as a filled bar rising from
 *                         the baseline.
 * @PN_GRAPH_STYLE_ERROR_BARS: Draw each time bucket as a box-and-whisker:
 *                         whiskers span the bucket's min..max, the box spans
 *                         the mean ±1 standard deviation, and a tick marks the
 *                         mean.  Each bucket is coloured green when its mean
 *                         rose versus the previous bucket and red when it fell,
 *                         so the trend reads at a glance.  Time-series view
 *                         only; the distribution view falls back to bars.
 *
 * Selects *how* the active #PnGraphView is drawn.  Honoured for single-topic
 * (2D) plots; multi-topic 3D plots fall back to the polyline form for the
 * time-series view and to bars for the distribution view.
 */
typedef enum
{
    PN_GRAPH_STYLE_POINTS,
    PN_GRAPH_STYLE_LINES,
    PN_GRAPH_STYLE_BARS,
    PN_GRAPH_STYLE_ERROR_BARS,
} PnGraphStyle;

#define PN_TYPE_GRAPH_STYLE (pn_graph_style_get_type ())
GType pn_graph_style_get_type (void) G_GNUC_CONST;

/**
 * PnGraphMode:
 * @PN_GRAPH_MODE_LINE:      Legacy: time-series view drawn as a polyline.
 * @PN_GRAPH_MODE_HISTOGRAM: Legacy: distribution view drawn as bars.
 *
 * Retained only so worksheets saved before the view/style split (which
 * persisted a single "mode" property) still load: the write-only "mode"
 * property maps each legacy value onto the matching #PnGraphView +
 * #PnGraphStyle pair.  New code should use #PnGraphView and #PnGraphStyle.
 */
typedef enum
{
    PN_GRAPH_MODE_LINE,
    PN_GRAPH_MODE_HISTOGRAM,
} PnGraphMode;

#define PN_TYPE_GRAPH_MODE (pn_graph_mode_get_type ())
GType pn_graph_mode_get_type (void) G_GNUC_CONST;

PnGraph *pn_graph_new (void);

/* Number of distinct per-topic series currently tracked.  Read-only
 * inspection seam: receive() lazily creates one series per message
 * topic (up to PN_GRAPH_MAX_SERIES), but the series table is private,
 * so headless tests observe series creation through this accessor. */
guint    pn_graph_get_series_count (PnGraph *self);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The PLplot/cairo plot painter lives in the gui-tier file           */
/*  pn-graph-gui.c, but it has to walk the same per-topic time-bucket  */
/*  rings and raw-sample rings that receive() fills in the core file.  */
/*  Those rings are plain data (no GTK / no PLplot), so the structs    */
/*  and the data-extraction accessors are published here and shared    */
/*  across the tier boundary.  The collected snapshot is owned by the  */
/*  caller (free with g_free); the pointed-to series structs and topic */
/*  strings remain owned by the node and are valid for the duration of */
/*  the paint call.                                                     */
/* ------------------------------------------------------------------ */

/* Capacity of the per-series time-bucket ring (largest X-bucket count
 * the configured window can be split into) and the raw-sample ring used
 * by the distribution view.  The painter sizes its stack scratch arrays
 * to these caps, so they live in the public header alongside the ring
 * struct rather than only in the core .c. */
#define PN_GRAPH_MAX_BINS  200
#define PN_GRAPH_SAMPLES   2048

/* Cap on how many of the most recent raw samples per series are written
 * into the worksheet file when the "save-data" property is on (samples
 * older than the active resolution window are dropped first).  With at
 * most PN_GRAPH_MAX_SERIES series the persisted store is bounded, so a
 * graph left running for a month cannot grow the document without end.
 * Comfortably above PN_GRAPH_MAX_BINS, so every time bucket the plot can
 * draw still has samples behind it after a reload. */
#define PN_GRAPH_PERSIST_SAMPLES  512

/* One time bucket: a circular-buffer slot indexed by
 * monotonic_time / bin_width_us.  @epoch is the absolute bin index this
 * slot currently represents (G_MININT64 when never used), so a stale
 * slot can be detected and lazily reset before it is touched again. */
typedef struct
{
    gint64   epoch;
    guint64  count;
    gdouble  sum;
    gdouble  sum_sq;
    gdouble  min;
    gdouble  max;
} PnGraphBin;

/* One raw observation kept verbatim for the distribution view, stamped
 * with the same monotonic clock the time-bin ring uses. */
typedef struct
{
    gint64  time_us;
    gdouble value;
} PnGraphSample;

/* Per-topic series: its time-bucket ring + raw-sample ring + a stable
 * arrival-order index (used as the Z-axis slot in 3D mode and to seed
 * the auto-assigned series colour). */
typedef struct
{
    PnGraphBin     ring[PN_GRAPH_MAX_BINS];

    PnGraphSample  samples[PN_GRAPH_SAMPLES];
    guint          sample_head;
    guint          sample_count;

    guint          arrival_idx;

    /* The "from" label carried on the most recent message routed to this
     * series — the feeding node's display name (its class name when it
     * has no name set), exactly as serialized into a message's top-level
     * "from" field.  Owned by the series; NULL until a message arrives.
     * Used to label the colour key in the multi-series 3D views. */
    gchar         *from;
} PnGraphSeries;

/* A (topic, series) pair, used to walk the series in arrival order on
 * every paint.  Both pointers are borrowed from the node's hashtable.
 * @from mirrors the series' source label (see #PnGraphSeries.from). */
typedef struct
{
    const gchar         *topic;
    const gchar         *from;
    const PnGraphSeries *series;
} PnGraphSeriesView;

/* Scalar drawing configuration, snapshotted by value for one paint.
 * The colours are #PnColor (layout-identical to GdkRGBA). */
typedef struct
{
    PnGraphResolution  resolution;
    guint              n_bins;
    PnGraphView        view;
    PnGraphStyle       style;

    PnColor            line_color;
    PnColor            axis_color;
    PnColor            background_color;
    guint              line_width;
    gboolean           show_grid;
    gboolean           log_y;
    gboolean           y_from_zero;
} PnGraphPaintState;

/**
 * pn_graph_get_paint_state:
 * @self: graph instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_graph_get_paint_state (PnGraph *self, PnGraphPaintState *out);

/**
 * pn_graph_collect_series_sorted:
 * @self:  graph instance
 * @out_n: (out): number of series written
 *
 * Returns: (transfer container): a newly-allocated array of
 * #PnGraphSeriesView in arrival order (free with g_free), or %NULL when
 * no series exist.  The series structs and topic strings are owned by
 * the node.
 */
PnGraphSeriesView *pn_graph_collect_series_sorted (PnGraph *self,
                                                   guint   *out_n);

/* Window length in seconds for @r, and the active bin width in
 * microseconds (always at least 1).  Pure helpers the painter shares
 * with the receive path. */
guint  pn_graph_resolution_seconds (PnGraphResolution r);
gint64 pn_graph_bin_width_us       (PnGraph *self);

G_END_DECLS

#endif /* PN_GRAPH_H */
