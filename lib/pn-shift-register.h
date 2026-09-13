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

#ifndef PN_SHIFT_REGISTER_H
#define PN_SHIFT_REGISTER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnShiftRegister                                                    */
/*                                                                     */
/*  One input, N outputs (the "outputs" property, 2..16): an open-     */
/*  ended, non-looping shift register of whole messages.  Every        */
/*  arriving message is pushed into stage 1 and every stored message   */
/*  moves one stage along; the message that was in the last stage      */
/*  falls off.  After the push each filled stage k goes out on output  */
/*  k — so the newest message leaves on output 1, the one before it on */
/*  output 2, and so on.  Stages not yet filled stay silent.           */
/*                                                                     */
/*  Emission order is the oldest stage first and output 1 last, so a   */
/*  downstream multi-input node triggered from output 1 already holds  */
/*  the fresh values of the other stages when it fires.                */
/*                                                                     */
/*  The stored messages are runtime state only: they are not saved     */
/*  with the worksheet, and changing the output count keeps the newest */
/*  stages that still fit.                                             */
/* ------------------------------------------------------------------ */

#define PN_SHIFT_REGISTER_MIN_OUTPUTS 2
#define PN_SHIFT_REGISTER_MAX_OUTPUTS 16
#define PN_SHIFT_REGISTER_DEF_OUTPUTS 4

#define PN_TYPE_SHIFT_REGISTER (pn_shift_register_get_type ())

G_DECLARE_FINAL_TYPE (PnShiftRegister, pn_shift_register,
                      PN, SHIFT_REGISTER, PnNode)

PnShiftRegister *pn_shift_register_new (void);

/**
 * pn_shift_register_get_n_filled:
 *
 * Number of stages currently holding a message (0 .. outputs).
 */
guint            pn_shift_register_get_n_filled (PnShiftRegister *self);

/**
 * pn_shift_register_clear:
 *
 * Empties every stage without emitting anything.
 */
void             pn_shift_register_clear (PnShiftRegister *self);

G_END_DECLS

#endif /* PN_SHIFT_REGISTER_H */
