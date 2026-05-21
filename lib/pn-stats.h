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

#ifndef PN_STATS_H
#define PN_STATS_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnStats                                                            */
/*                                                                     */
/*  Statistics filter.  Counts incoming messages and tracks the time   */
/*  between consecutive arrivals.  For every message that arrives it   */
/*  emits a brand-new message (topic "stats") whose data bag carries   */
/*  the running count, the minimum / maximum / average inter-arrival   */
/*  time in seconds, and the throughput expressed as messages per      */
/*  minute, per hour and per day.  The original incoming message is    */
/*  consumed; only the freshly built statistics message is forwarded.  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_STATS (pn_stats_get_type ())

G_DECLARE_FINAL_TYPE (PnStats, pn_stats, PN, STATS, PnNode)

PnStats *pn_stats_new (void);

G_END_DECLS

#endif /* PN_STATS_H */
