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

#ifndef PN_PLOT_H
#define PN_PLOT_H

#include "pn-graph.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPlot                                                             */
/*                                                                     */
/*  Vector-fed sibling of #PnGraph.  Where PnGraph time-bins SCALAR    */
/*  values by their wall-clock arrival (one sample per bin, filling    */
/*  in only as real time passes), PnPlot takes a whole $pnvector on    */
/*  data.value in a single message — the snapshot path #PnOscilloscope */
/*  uses — and distributes its N elements across M consecutive         */
/*  X-buckets, aggregating each run into a real mean / sd / min / max.  */
/*  The plot therefore fills instantly and deterministically, which    */
/*  makes the Error-bars draw-style (box-and-whisker per bucket) testable*/
/*  from an example worksheet and a headless test instead of only       */
/*  against a slow live feed.                                          */
/*                                                                     */
/*  The drawing is identical to PnGraph's: PnPlot fills the very same   */
/*  GTK-free #PnGraphBin rings + raw-sample ring and the gui tier       */
/*  (pn-plot-gui.c) reuses PnGraph's PLplot draw path (pn-graph-draw.h),*/
/*  so every draw-style (Points / Lines / Bars / Error bars) and the    */
/*  Distribution view render exactly as they do for PnGraph — only the  */
/*  receive() front-end (vector → buckets) differs.  The X axis is the  */
/*  bucket index rather than time.                                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PLOT (pn_plot_get_type ())

G_DECLARE_FINAL_TYPE (PnPlot, pn_plot, PN, PLOT, PnNode)

PnPlot *pn_plot_new (void);

/* ------------------------------------------------------------------ */
/*  GUI / test read seam (GTK-free)                                    */
/*                                                                     */
/*  PnPlot reuses #PnGraphSeries / #PnGraphPaintState (published in     */
/*  pn-graph.h) verbatim, so the gui-tier painter and the headless      */
/*  characterization test read the binned data back through the same    */
/*  plain-data structs the PnGraph painter consumes.                    */
/* ------------------------------------------------------------------ */

/**
 * pn_plot_get_paint_state:
 * @self: plot instance
 * @out:  (out): caller-provided snapshot filled with the current drawing
 *        configuration.  @out->n_bins carries the active X-bucket count.
 */
void pn_plot_get_paint_state (PnPlot *self, PnGraphPaintState *out);

/**
 * pn_plot_peek_series:
 * @self: plot instance
 *
 * Returns: (transfer none) (nullable): the single per-instance series
 * whose bucket ring + raw-sample ring hold the most recent vector, or
 * %NULL when no vector has been received yet.  Owned by the node and
 * valid until the next message or finalize.
 */
const PnGraphSeries *pn_plot_peek_series (PnPlot *self);

/* Active X-bucket count (the "x-buckets" property, 2..PN_GRAPH_MAX_BINS). */
guint pn_plot_get_n_bins (PnPlot *self);

G_END_DECLS

#endif /* PN_PLOT_H */
