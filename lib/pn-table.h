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

#ifndef PN_TABLE_H
#define PN_TABLE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTable                                                            */
/*                                                                     */
/*  Sink node that displays the most recent messages it receives as a  */
/*  scrolling table of rows, with one column per entry in a            */
/*  comma-separated "Title:path" spec.  Each path is the same          */
/*  "/"-separated JSON pointer the rest of the codebase already uses   */
/*  (PnGraph, PnFormat, PnDedup, PnStats); on every receive the path   */
/*  is resolved against the message's root object and the result is    */
/*  stringified into the corresponding cell.  Newest rows appear at    */
/*  the top; the buffer is capped at the configured limit.  Visually   */
/*  the node mirrors PnGraph: a 280 px standard header with a         */
/*  separately-painted 173 px tall body below it, so a row of mixed    */
/*  Graph + Table sinks lines up cleanly on the worksheet.  Clicking   */
/*  the body lifts the table into the same centred zoom overlay the   */
/*  graph uses, where the mouse wheel scrolls through the rows.       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TABLE (pn_table_get_type ())

G_DECLARE_FINAL_TYPE (PnTable, pn_table, PN, TABLE, PnNode)

PnTable *pn_table_new (void);

/* Number of rows currently buffered.  Read-only inspection seam:
 * receive() appends one row per message and trims to #limit, but the
 * row store is private, so headless tests observe accumulation and
 * trimming through this accessor. */
guint    pn_table_get_row_count (PnTable *self);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo table painter lives in the gui-tier file pn-table-gui.c,  */
/*  but it has to read the same parsed column spec and row buffer that  */
/*  receive() fills in the core file.  Those are plain data (no GTK),   */
/*  so the structs and the data-access accessors are published here.    */
/*  The pointers handed back are owned by the node and valid for the    */
/*  duration of one paint call.                                         */
/* ------------------------------------------------------------------ */

/* One parsed column: header label + the JSON pointer its cells read. */
typedef struct
{
    gchar *title;
    gchar *path;
} PnTableColumn;

/* One buffered row: the stringified cells stamped at receive time. */
typedef struct
{
    gint64   received_us;
    gchar  **values;        /* values[i] for column i */
    guint    n_values;
} PnTableRow;

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
} PnTablePaintState;

/**
 * pn_table_get_paint_state:
 * @self: table instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_table_get_paint_state (PnTable *self, PnTablePaintState *out);

/* Parsed column spec (borrowed): the array of #PnTableColumn and its
 * length.  @n_out receives the count. */
const PnTableColumn *pn_table_peek_columns (PnTable *self, guint *n_out);

/* The live row buffer (borrowed): a #GQueue of #PnTableRow*, newest at
 * the head. */
GQueue *pn_table_peek_rows (PnTable *self);

/* Scroll bookkeeping.  The painter is the only place that knows the live
 * row height / visible-row count, so it computes the maximum offset and
 * asks the core to clamp the stored value into [0, max] before reading
 * it back.  Both are GTK-free (they only touch an int). */
int  pn_table_get_scroll_offset    (PnTable *self);
void pn_table_clamp_scroll_offset  (PnTable *self, int max_offset);

G_END_DECLS

#endif /* PN_TABLE_H */
