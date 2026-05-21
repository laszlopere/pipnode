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

#ifndef PN_FREE_COMMAND_H
#define PN_FREE_COMMAND_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFreeCommand                                                      */
/*                                                                     */
/*  Auto-trigger node that runs `free` once per #PnAutoTrigger:period  */
/*  seconds, parses the column-aligned output with the same two-pass   */
/*  detector PnTableModel uses, and emits a message carrying           */
/*    data.success - whether `free` exited with status 0,              */
/*    data.output  - the combined stdout/stderr verbatim, and          */
/*    data.table   - the parsed table in PnTableModel's shape so a     */
/*                   downstream PnTableView renders it without an      */
/*                   intermediate filter.                              */
/*                                                                     */
/*  Each cell carries an extra `name` member alongside `text`, so a    */
/*  later consumer can address a specific cell symbolically.  The      */
/*  name shape is <row>.<column>: the row part comes from the first    */
/*  cell of the data row ("Mem:" -> "mem", "Swap:" -> "swap") and the  */
/*  column part comes from the header text ("buff/cache" ->            */
/*  "buff_cache"); the leading row-label column gets the column id     */
/*  "name", and the header row gets the row id "header".               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FREE_COMMAND (pn_free_command_get_type ())

G_DECLARE_FINAL_TYPE (PnFreeCommand, pn_free_command,
                      PN, FREE_COMMAND, PnAutoTrigger)

PnFreeCommand *pn_free_command_new (void);

G_END_DECLS

#endif /* PN_FREE_COMMAND_H */
