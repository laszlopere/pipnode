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

G_END_DECLS

#endif /* PN_TABLE_VIEW_H */
