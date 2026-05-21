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
 * PnGraphMode:
 * @PN_GRAPH_MODE_LINE:      Time-series polyline (most recent on the right).
 * @PN_GRAPH_MODE_HISTOGRAM: Frequency distribution over the current window —
 *                           in-window samples are binned by value and shown
 *                           as filled bars, so the shape of the bars reads
 *                           as a probability density of where the values
 *                           are clustered.
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
