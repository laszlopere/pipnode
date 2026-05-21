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

#ifndef PN_DF_COMMAND_H
#define PN_DF_COMMAND_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDfCommand                                                        */
/*                                                                     */
/*  Auto-trigger node that runs `df` once per #PnAutoTrigger:period    */
/*  seconds and emits a message carrying                               */
/*    data.success - whether `df` exited with status 0,                */
/*    data.output  - the combined stdout/stderr verbatim, and          */
/*    data.table   - the parsed table in PnTableModel's shape so a     */
/*                   downstream PnTableView renders it without an      */
/*                   intermediate filter.                              */
/*                                                                     */
/*  Unlike PnFreeCommand (whose output is space-separated and fixed,   */
/*  so a plain whitespace split suffices), `df` has right-aligned      */
/*  numeric columns wider than their headers and mount paths that may  */
/*  contain spaces, so the node uses the same two-pass column-         */
/*  position detector PnTableModel applies: word-starts on the header  */
/*  in pass 1, leftward shifts driven by data-row content in pass 2.   */
/*                                                                     */
/*  Each cell carries an extra `name` member alongside `text`, shaped  */
/*  <row>.<column> -- the column id comes from the sanitised header    */
/*  word ("1K-blocks" -> "1k_blocks") and the row id from the          */
/*  sanitised first-column value ("/dev/nvme0n1p2" -> "dev_nvme0n1p2", */
/*  "tmpfs" -> "tmpfs"); the header row's row id is the literal        */
/*  "header".                                                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DF_COMMAND (pn_df_command_get_type ())

G_DECLARE_FINAL_TYPE (PnDfCommand, pn_df_command,
                      PN, DF_COMMAND, PnAutoTrigger)

PnDfCommand *pn_df_command_new (void);

G_END_DECLS

#endif /* PN_DF_COMMAND_H */
