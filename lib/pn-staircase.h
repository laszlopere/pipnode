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

#ifndef PN_STAIRCASE_H
#define PN_STAIRCASE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnStaircase                                                        */
/*                                                                     */
/*  Monostable "staircase timer", named after the stairwell light      */
/*  switch that you press once and that turns itself off a while       */
/*  later.  A trigger message — one whose data.value is greater than   */
/*  0.5 — is forwarded unchanged, after which the node arms a single   */
/*  one-shot timer and stays silent.  When #PnStaircase:on-time-ms     */
/*  milliseconds elapse it emits a fresh data.value = 0.0 message as   */
/*  the turn-off signal and returns to its resting state.              */
/*                                                                     */
/*  Messages that are not triggers (data.value <= 0.5, or no numeric   */
/*  data.value at all) are dropped: the node owns the off edge and     */
/*  never lets external traffic disturb the timed window.              */
/*                                                                     */
/*  While the node is on, further triggers are governed by             */
/*  #PnStaircase:retriggerable.  When TRUE each trigger restarts the   */
/*  timer, pushing the turn-off further into the future (the stairwell */
/*  light you keep tapping to stay lit); when FALSE they are ignored   */
/*  and the original turn-off time stands.  Either way a re-trigger    */
/*  is not re-forwarded — downstream is already on.                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_STAIRCASE (pn_staircase_get_type ())

G_DECLARE_FINAL_TYPE (PnStaircase, pn_staircase, PN, STAIRCASE, PnNode)

PnStaircase *pn_staircase_new (void);

G_END_DECLS

#endif /* PN_STAIRCASE_H */
