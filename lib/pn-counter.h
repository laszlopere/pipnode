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

#ifndef PN_COUNTER_H
#define PN_COUNTER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnCounter                                                          */
/*                                                                     */
/*  A self-advancing value register — the CPU program counter.  It     */
/*  holds one number and has three inputs:                             */
/*                                                                     */
/*    input 0  tick  — emit the current value, then advance it by      */
/*                     #PnCounter:step (wrapping at #PnCounter:modulo   */
/*                     when that is non-zero).                          */
/*    input 1  load  — store the incoming data.value (a JUMP target).  */
/*                     Emits NOTHING.                                   */
/*    input 2  reset — store #PnCounter:initial.  Emits NOTHING.       */
/*                                                                     */
/*  A `load` that arrives *while a tick's downstream chain is running* */
/*  (a jump decoded from the very instruction the tick fetched) wins    */
/*  over the implicit advance for that cycle, so the jump target is     */
/*  what is emitted next tick.  Like #PnRegister, the load and reset    */
/*  ports are silent, so the counter can sit in a feedback loop without */
/*  forming a synchronous cycle.                                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_COUNTER (pn_counter_get_type ())

G_DECLARE_FINAL_TYPE (PnCounter, pn_counter, PN, COUNTER, PnNode)

PnCounter *pn_counter_new (void);

G_END_DECLS

#endif /* PN_COUNTER_H */
