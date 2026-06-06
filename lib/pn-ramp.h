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

#ifndef PN_RAMP_H
#define PN_RAMP_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRamp                                                             */
/*                                                                     */
/*  Three-input vector generator and the worked example of the large- */
/*  numeric-vector machinery (TODO #43) — the seed of the analog-      */
/*  computer toy.  Inputs "from" (0), "to" (1) and "N" (2) each carry  */
/*  a numeric data.value; the node latches the most recent value seen  */
/*  on each and, once all three have arrived, emits a fresh message    */
/*  whose data.value is a $pnvector holding the linear ramp: N evenly- */
/*  spaced values running from "from" to "to" (inclusive of both ends  */
/*  when N >= 2; a single "from" when N == 1).                         */
/*                                                                     */
/*  Because it remembers each input, a new value on ANY input after    */
/*  the trio is complete recomputes the ramp from the remembered       */
/*  values and emits again — drive "N" alone and watch the resolution  */
/*  change, drive "to" alone and watch the span stretch.  N is rounded */
/*  to the nearest integer and clamped to a sane maximum so a stray    */
/*  huge value cannot exhaust memory; an N below 1 produces nothing.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_RAMP (pn_ramp_get_type ())

G_DECLARE_FINAL_TYPE (PnRamp, pn_ramp, PN, RAMP, PnNode)

PnRamp *pn_ramp_new (void);

G_END_DECLS

#endif /* PN_RAMP_H */
