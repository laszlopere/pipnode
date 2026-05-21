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

#ifndef PN_DEDUP_H
#define PN_DEDUP_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDedup                                                            */
/*                                                                     */
/*  Deduplicating filter.  Reads a configurable JSON path out of each  */
/*  arriving message (e.g. "data/blockhash") and forwards the message  */
/*  only the first time that value is observed.  Subsequent messages   */
/*  carrying the same value are dropped silently.  Each remembered     */
/*  value expires after #PnDedup:timeout minutes, after which the      */
/*  same value is treated as fresh again.                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DEDUP (pn_dedup_get_type ())

G_DECLARE_FINAL_TYPE (PnDedup, pn_dedup, PN, DEDUP, PnNode)

PnDedup *pn_dedup_new (void);

G_END_DECLS

#endif /* PN_DEDUP_H */
