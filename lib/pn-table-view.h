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

#ifndef PN_TABLE_VIEW_H
#define PN_TABLE_VIEW_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTableView                                                        */
/*                                                                     */
/*  Sink node that renders the most recent #PnTableModel-style table   */
/*  carried on `data.table` of an incoming message.  The expected      */
/*  payload shape is the one #PnTableModel produces:                   */
/*                                                                     */
/*    { "header": { "cells": ["...", ...] },   // optional             */
/*      "rows":   [ { "cells": ["...", ...] }, ... ] }                 */
/*                                                                     */
/*  Each received message REPLACES the displayed table (rather than    */
/*  appending one row, the way #PnTable does), so a worksheet wiring  */
/*  `PnShellCommand -> PnTableModel -> PnTableView` shows the latest  */
/*  command run as a live, structured table view of e.g. `df -h`.     */
/*                                                                     */
/*  Visually the node mirrors #PnTable: 280 px standard header with    */
/*  a separately-painted 173 px body below it, so a row of mixed      */
/*  Table / TableView sinks lines up cleanly.  Clicking the body      */
/*  lifts the table into the same centred zoom overlay the graph      */
/*  uses, where the mouse wheel scrolls through rows.                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TABLE_VIEW (pn_table_view_get_type ())

G_DECLARE_FINAL_TYPE (PnTableView, pn_table_view, PN, TABLE_VIEW, PnNode)

PnTableView *pn_table_view_new (void);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo painter lives in the gui-tier file pn-table-view-gui.c,   */
/*  but it has to read the same parsed snapshot that receive() fills in  */
/*  the core file.  That snapshot is plain data (no GTK), so the structs */
/*  and the data-access accessors are published here.  The pointers      */
/*  handed back are owned by the node and valid for the duration of one  */
/*  paint call.                                                          */
/* ------------------------------------------------------------------ */

/* One row's stringified cells, stamped at receive time. */
typedef struct
{
    gchar **cells;     /* cells[i] for column i; owned by the node */
    guint   n_cells;
} PnTableViewRow;

/* Scalar drawing configuration, snapshotted by value for one paint.
 * The colours are #PnColor (layout-identical to GdkRGBA). */
typedef struct
{
    PnColor  background_color;
    PnColor  header_background_color;
    PnColor  grid_color;
    PnColor  text_color;
    PnColor  header_text_color;
    gboolean alternate_row_background;
    guint    n_cols;            /* max cells across header + rows */
} PnTableViewPaintState;

/**
 * pn_table_view_get_paint_state:
 * @self: table-view instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_table_view_get_paint_state (PnTableView           *self,
                                    PnTableViewPaintState *out);

/* The latest header cells (borrowed): the array and its length via
 * @n_out.  Returns %NULL / 0 when the most recent message carried no
 * header object. */
const gchar * const *pn_table_view_peek_header (PnTableView *self,
                                                guint       *n_out);

/* The live row buffer (borrowed): a #GPtrArray of #PnTableViewRow*. */
GPtrArray *pn_table_view_peek_rows (PnTableView *self);

/* Scroll bookkeeping.  The painter is the only place that knows the live
 * row height / visible-row count, so it computes the maximum offset and
 * asks the core to clamp the stored value into [0, max] before reading
 * it back.  Both are GTK-free (they only touch an int). */
int  pn_table_view_get_scroll_offset   (PnTableView *self);
void pn_table_view_clamp_scroll_offset (PnTableView *self, int max_offset);

G_END_DECLS

#endif /* PN_TABLE_VIEW_H */
