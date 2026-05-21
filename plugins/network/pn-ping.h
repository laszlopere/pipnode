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

#ifndef PN_PING_H
#define PN_PING_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPing                                                             */
/*                                                                     */
/*  Auto-trigger node that sends a single ICMP echo to the configured  */
/*  host once per #PnAutoTrigger:period seconds and emits the result   */
/*  on its output port.  The actual probe is done by spawning the     */
/*  system "ping" utility from the inherited worker thread, so the    */
/*  GUI never blocks on slow or unreachable targets.                  */
/*                                                                     */
/*  The node has a "configuration required" state: while #PnPing:host */
/*  is unset (or empty) it switches to a red body with a warning      */
/*  exclamation glyph and skips its triggers entirely.                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PING (pn_ping_get_type ())

G_DECLARE_FINAL_TYPE (PnPing, pn_ping, PN, PING, PnAutoTrigger)

PnPing *pn_ping_new (void);

G_END_DECLS

#endif /* PN_PING_H */
