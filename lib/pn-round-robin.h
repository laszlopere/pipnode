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

#ifndef PN_ROUND_ROBIN_H
#define PN_ROUND_ROBIN_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRoundRobin                                                       */
/*                                                                     */
/*  Two inputs (in, reset), N outputs (the "outputs" property, 2..16). */
/*  Every message on the "in" input leaves, unchanged, by the next     */
/*  output in turn: the first by output 1, the second by output 2, ... */
/*  and after output N the turn wraps back to output 1.  A message on  */
/*  the "reset" input makes output 1 the next one again and emits      */
/*  nothing.                                                           */
/*                                                                     */
/*  Lowering the output count while the next output is past the new    */
/*  end wraps the turn to output 1.  The turn is runtime state: it is  */
/*  not saved with the worksheet.                                      */
/* ------------------------------------------------------------------ */

#define PN_ROUND_ROBIN_MIN_OUTPUTS 2
#define PN_ROUND_ROBIN_MAX_OUTPUTS 16
#define PN_ROUND_ROBIN_DEF_OUTPUTS 3

/* Input ports. */
#define PN_ROUND_ROBIN_IN_MESSAGE 0
#define PN_ROUND_ROBIN_IN_RESET   1
#define PN_ROUND_ROBIN_N_INPUTS   2

#define PN_TYPE_ROUND_ROBIN (pn_round_robin_get_type ())

G_DECLARE_FINAL_TYPE (PnRoundRobin, pn_round_robin,
                      PN, ROUND_ROBIN, PnNode)

PnRoundRobin *pn_round_robin_new (void);

/**
 * pn_round_robin_get_next:
 *
 * The 0-based output the next message on the "in" input will leave by.
 */
gint          pn_round_robin_get_next (PnRoundRobin *self);

/**
 * pn_round_robin_reset:
 *
 * Makes output 1 the next output, without emitting anything.
 */
void          pn_round_robin_reset (PnRoundRobin *self);

G_END_DECLS

#endif /* PN_ROUND_ROBIN_H */
