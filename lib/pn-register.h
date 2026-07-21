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

#ifndef PN_REGISTER_H
#define PN_REGISTER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRegister                                                         */
/*                                                                     */
/*  A wire-writable value latch — the CPU accumulator / general        */
/*  register / flag store.  It holds one number and has three inputs:  */
/*                                                                     */
/*    input 0  write  — latch the incoming data.value.  Emits NOTHING. */
/*    input 1  read   — emit the stored value on data.value.           */
/*    input 2  reset  — store #PnRegister:initial.  Emits NOTHING.     */
/*                                                                     */
/*  The write and reset ports are deliberately silent: a store cannot  */
/*  re-trigger the graph, so a register wired into a feedback loop      */
/*  (write-back into the same accumulator, a jump target into a        */
/*  program counter) never forms a *synchronous* cycle — the new value */
/*  is read back out on the next clock tick instead.  This is the core */
/*  rule that keeps a toy-CPU worksheet under the engine's synchronous- */
/*  dispatch recursion cap.                                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_REGISTER (pn_register_get_type ())

G_DECLARE_FINAL_TYPE (PnRegister, pn_register, PN, REGISTER, PnNode)

PnRegister *pn_register_new (void);

G_END_DECLS

#endif /* PN_REGISTER_H */
