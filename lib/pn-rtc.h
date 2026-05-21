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

#ifndef PN_RTC_H
#define PN_RTC_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRtc                                                              */
/*                                                                     */
/*  Real-time-clock node.  Emits a #PnMessage on its output once per  */
/*  #PnAutoTrigger:period seconds carrying the current local-time     */
/*  decomposition (epoch, year, month, dom, hour, minute, second) as  */
/*  named members of the message data bag.  The timer fires from the  */
/*  worker thread inherited from #PnAutoTrigger; emission is hopped   */
/*  back to the main loop by the base class helper.                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_RTC (pn_rtc_get_type ())

G_DECLARE_FINAL_TYPE (PnRtc, pn_rtc, PN, RTC, PnAutoTrigger)

PnRtc *pn_rtc_new (void);

G_END_DECLS

#endif /* PN_RTC_H */
