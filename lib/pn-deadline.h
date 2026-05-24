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

#ifndef PN_DEADLINE_H
#define PN_DEADLINE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDeadline                                                         */
/*                                                                     */
/*  Reports how far away a fixed point in time is.  Each incoming      */
/*  message must carry a numeric data.value holding the "now" instant  */
/*  as a Unix epoch (exactly what the #PnRtc "Clock" node emits).  The */
/*  node interprets its configured #PnDeadline:target wall-clock       */
/*  string ("yyyy-mm-dd hh:MM:ss") in the chosen                       */
/*  #PnDeadline:timezone, turns it into a Unix epoch, and forwards the */
/*  message with data.value rewritten to the seconds remaining,        */
/*  target_epoch - now_epoch.  The value counts down to 0 at the       */
/*  target and keeps going negative afterwards, so a downstream node   */
/*  can detect "passed" with value <= 0.                               */
/*                                                                     */
/*  The timezone is a fixed-offset abbreviation (e.g. "CET", "EST");   */
/*  when none is configured GMT (UTC+0) is assumed.  Other message     */
/*  members are passed through untouched.                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DEADLINE (pn_deadline_get_type ())

G_DECLARE_FINAL_TYPE (PnDeadline, pn_deadline, PN, DEADLINE, PnNode)

PnDeadline *pn_deadline_new (void);

G_END_DECLS

#endif /* PN_DEADLINE_H */
