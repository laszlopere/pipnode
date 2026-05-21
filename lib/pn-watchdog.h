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

#ifndef PN_WATCHDOG_H
#define PN_WATCHDOG_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWatchdog                                                         */
/*                                                                     */
/*  Auto-triggered filter that watches an upstream message stream and  */
/*  raises an alarm when it goes quiet.  Incoming messages are         */
/*  consumed (never forwarded) and only set an internal "saw a         */
/*  message since the last tick" flag.  On every #PnAutoTrigger tick   */
/*  the flag is read-and-cleared: when no message arrived, the node    */
/*  emits a fresh #PnMessage carrying data.success = false and        */
/*  data.output = #PnWatchdog:output-text.                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WATCHDOG (pn_watchdog_get_type ())

G_DECLARE_FINAL_TYPE (PnWatchdog, pn_watchdog, PN, WATCHDOG, PnAutoTrigger)

PnWatchdog *pn_watchdog_new (void);

G_END_DECLS

#endif /* PN_WATCHDOG_H */
