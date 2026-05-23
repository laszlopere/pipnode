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

#ifndef PN_LOAD_H
#define PN_LOAD_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnLoad                                                             */
/*                                                                     */
/*  Auto-trigger node that samples the system load average once per    */
/*  #PnAutoTrigger:period seconds and emits the 1-minute figure as     */
/*  data.value (with the 5- and 15-minute siblings alongside).         */
/*                                                                     */
/*  When #PnLoad:hostname is empty or "localhost" the sample is read   */
/*  directly from /proc/loadavg.  Any other value is treated as a      */
/*  remote host and the sample is fetched via passwordless SSH:        */
/*  the worker shells out to `ssh -o BatchMode=yes ...` and parses     */
/*  /proc/loadavg from the remote machine.                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LOAD (pn_load_get_type ())

G_DECLARE_FINAL_TYPE (PnLoad, pn_load, PN, LOAD, PnAutoTrigger)

PnLoad *pn_load_new (void);

G_END_DECLS

#endif /* PN_LOAD_H */
