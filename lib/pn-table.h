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

G_END_DECLS

#endif /* PN_TABLE_H */
