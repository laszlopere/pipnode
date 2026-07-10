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

#ifndef PN_DAILY_TIMER_H
#define PN_DAILY_TIMER_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDailyTimer                                                       */
/*                                                                     */
/*  Weekly on/off schedule.  The user builds a list of intervals --    */
/*  each one a day of the week (or "Every day") plus an on time and    */
/*  an off time -- and the node emits data.value = 1.0 when local      */
/*  wall-clock time enters any interval and data.value = 0.0 when it   */
/*  leaves the last one.  Only the transitions are emitted: the node   */
/*  polls once per #PnAutoTrigger:period second on the inherited       */
/*  worker thread but stays silent while the computed state is         */
/*  unchanged, so a downstream relay sees one message per switch-over  */
/*  rather than one per tick.                                          */
/*                                                                     */
/*  The very first poll after construction always emits, whichever     */
/*  state it finds: opening a worksheet at 08:00 with a 07:00-09:00    */
/*  interval must leave the heating on, and opening it at 10:00 must   */
/*  leave it off.  #PnAutoTrigger clamps that first tick to one second */
/*  after construction, by which time the load path has finished        */
/*  wiring the graph and applying the :schedule property, so the        */
/*  announce reaches the nodes downstream.                             */
/*                                                                     */
/*  An interval whose off time is at or before its on time wraps       */
/*  through midnight into the following day -- 22:00-06:00 on Friday   */
/*  runs until Saturday morning.  An interval whose two times are      */
/*  equal is degenerate and ignored.                                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DAILY_TIMER (pn_daily_timer_get_type ())

G_DECLARE_FINAL_TYPE (PnDailyTimer, pn_daily_timer,
                      PN, DAILY_TIMER, PnAutoTrigger)

PnDailyTimer *pn_daily_timer_new (void);

/* pn_daily_timer_state_at:
 * @self:        a #PnDailyTimer
 * @day_of_week: 1 = Monday … 7 = Sunday, matching
 *               g_date_time_get_day_of_week()
 * @hour:        0-23
 * @minute:      0-59
 *
 * Evaluates the compiled schedule against an arbitrary wall-clock
 * instant and reports whether the timer is "on" at that moment.  Pure
 * (it reads no clock), which is what makes the schedule semantics --
 * interval containment, midnight wrap, "Every day" rows -- testable
 * without waiting for real time to pass.  The trigger calls this with
 * the current local time.
 *
 * Returns: %TRUE when @day_of_week / @hour / @minute falls inside any
 *   configured interval.
 */
gboolean pn_daily_timer_state_at (PnDailyTimer *self,
                                  gint          day_of_week,
                                  gint          hour,
                                  gint          minute);

/* pn_daily_timer_get_active:
 *
 * The state carried by the most recently emitted message, or %FALSE if
 * the node has not polled yet.
 */
gboolean pn_daily_timer_get_active (PnDailyTimer *self);

G_END_DECLS

#endif /* PN_DAILY_TIMER_H */
