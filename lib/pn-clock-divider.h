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

#ifndef PN_CLOCK_DIVIDER_H
#define PN_CLOCK_DIVIDER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnClockDivider                                                     */
/*                                                                     */
/*  Two inputs (tick, reset), N outputs (the "outputs" property,       */
/*  2..16), one divisor per output (the "divisors" property, a JSON    */
/*  array of integers).  Every message on the tick input advances a    */
/*  shared count; output k forwards that tick message when the count   */
/*  is a multiple of its divisor, so a divisor of 4 fires on the 4th,  */
/*  8th, 12th, ... tick.  A message on the reset input zeroes the count */
/*  and emits nothing.                                                 */
/*                                                                     */
/*  The default divisors are powers of two: output 1 = /2, output 2 =  */
/*  /4, ..., output 16 = /65536.  "divisors" always describes all 16   */
/*  slots, so lowering the output count keeps the divisors of removed  */
/*  outputs.  Each output is labelled on the worksheet with its         */
/*  divisor ("/2").                                                    */
/*                                                                     */
/*  When several outputs fire on the same tick, the slowest (largest   */
/*  divisor) fires first and the fastest last; equal divisors fire     */
/*  from the highest output down.  A downstream multi-input node       */
/*  triggered by the fast output therefore already holds the values    */
/*  the slower outputs just sent.                                      */
/*                                                                     */
/*  The count is runtime state: it is not saved with the worksheet.    */
/* ------------------------------------------------------------------ */

#define PN_CLOCK_DIVIDER_MIN_OUTPUTS 2
#define PN_CLOCK_DIVIDER_MAX_OUTPUTS 16
#define PN_CLOCK_DIVIDER_DEF_OUTPUTS 4

#define PN_CLOCK_DIVIDER_MIN_DIVISOR 1u
#define PN_CLOCK_DIVIDER_MAX_DIVISOR 1000000u

/* Input ports. */
#define PN_CLOCK_DIVIDER_IN_TICK   0
#define PN_CLOCK_DIVIDER_IN_RESET  1
#define PN_CLOCK_DIVIDER_N_INPUTS  2

#define PN_TYPE_CLOCK_DIVIDER (pn_clock_divider_get_type ())

G_DECLARE_FINAL_TYPE (PnClockDivider, pn_clock_divider,
                      PN, CLOCK_DIVIDER, PnNode)

PnClockDivider *pn_clock_divider_new (void);

/**
 * pn_clock_divider_default_divisor:
 * @index: 0-based output index
 *
 * The divisor output @index has until the user changes it: 2^(index+1).
 */
guint           pn_clock_divider_default_divisor (gint index);

/**
 * pn_clock_divider_get_divisor:
 * @index: 0-based output index, 0 .. PN_CLOCK_DIVIDER_MAX_OUTPUTS-1
 */
guint           pn_clock_divider_get_divisor (PnClockDivider *self,
                                              gint            index);

/**
 * pn_clock_divider_set_divisor:
 * @index:   0-based output index, 0 .. PN_CLOCK_DIVIDER_MAX_OUTPUTS-1
 * @divisor: clamped to PN_CLOCK_DIVIDER_MIN_DIVISOR ..
 *           PN_CLOCK_DIVIDER_MAX_DIVISOR
 *
 * Sets one output's divisor; notifies "divisors".
 */
void            pn_clock_divider_set_divisor (PnClockDivider *self,
                                              gint            index,
                                              guint           divisor);

/**
 * pn_clock_divider_get_count:
 *
 * Ticks counted since creation or the last reset.
 */
guint64         pn_clock_divider_get_count (PnClockDivider *self);

/**
 * pn_clock_divider_plan:
 * @count: a tick count (the value after the tick was counted)
 * @order: (out caller-allocates): at least PN_CLOCK_DIVIDER_MAX_OUTPUTS
 *         slots
 *
 * Pure seam, for tests: fills @order with the 0-based outputs that fire
 * at @count, in emission order (see the header comment).
 *
 * Returns: the number of outputs written to @order.
 */
gint            pn_clock_divider_plan (PnClockDivider *self,
                                       guint64         count,
                                       gint           *order);

G_END_DECLS

#endif /* PN_CLOCK_DIVIDER_H */
