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

G_END_DECLS

#endif /* PN_GRAPH_H */
