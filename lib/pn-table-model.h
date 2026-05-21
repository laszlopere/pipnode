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

#ifndef PN_TABLE_MODEL_H
#define PN_TABLE_MODEL_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTableModel                                                       */
/*                                                                     */
/*  Filter that parses an ASCII / fixed-width text table out of a      */
/*  message's `data.output` string and attaches the structured result  */
/*  to the same message as `data.table` before passing it through.    */
/*                                                                     */
/*  The emitted `data.table` is an object of the shape:                */
/*                                                                     */
/*    {                                                                */
/*      "header": { "cells": ["...", "...", ...] },   // optional      */
/*      "rows":   [                                                    */
/*        { "cells": ["...", "...", ...] },                            */
/*        ...                                                          */
/*      ]                                                              */
/*    }                                                                */
/*                                                                     */
/*  Column boundaries are detected from the first non-empty line by    */
/*  finding the positions where a non-space character follows a space; */
/*  subsequent lines are then sliced at those same positions and each  */
/*  cell is whitespace-trimmed.  Suitable for output produced by Unix  */
/*  tools that align their columns (`df`, `ps`, `ls -l`, `free`, ...).*/
/*                                                                     */
/*  When #PnTableModel:parse-header is %TRUE (the default) the first   */
/*  non-empty line becomes `data.table.header.cells` and the remaining */
/*  lines become `data.table.rows`; when %FALSE the first line is      */
/*  still used to detect column positions but is emitted as the first  */
/*  data row and no `header` member is produced.                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TABLE_MODEL (pn_table_model_get_type ())

G_DECLARE_FINAL_TYPE (PnTableModel, pn_table_model,
                      PN, TABLE_MODEL, PnNode)

PnTableModel *pn_table_model_new (void);

G_END_DECLS

#endif /* PN_TABLE_MODEL_H */
