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

#ifndef PN_TAPPED_DELAY_H
#define PN_TAPPED_DELAY_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTappedDelay                                                      */
/*                                                                     */
/*  One input, N outputs (the "outputs" property, 2..16): a time-      */
/*  staggered fan-out.  Every arriving message is forwarded unchanged  */
/*  on each output, output k (1-based) after k * "step-ms"             */
/*  milliseconds — output 1 after one step, output N after N steps.    */
/*                                                                     */
/*  Each (message, output) pair rides its own one-shot main-loop       */
/*  timer, so a burst keeps its order on every output.  Changing       */
/*  "step-ms" only affects messages that arrive afterwards; lowering   */
/*  "outputs" cancels the copies still waiting for a removed output.   */
/*  Pending copies are runtime state and are discarded, not forwarded, */
/*  when the node is destroyed.                                        */
/* ------------------------------------------------------------------ */

#define PN_TAPPED_DELAY_MIN_OUTPUTS 2
#define PN_TAPPED_DELAY_MAX_OUTPUTS 16
#define PN_TAPPED_DELAY_DEF_OUTPUTS 4

#define PN_TAPPED_DELAY_STEP_MS_MIN 0u
#define PN_TAPPED_DELAY_STEP_MS_MAX 3600000u
#define PN_TAPPED_DELAY_STEP_MS_DEF 250u

#define PN_TYPE_TAPPED_DELAY (pn_tapped_delay_get_type ())

G_DECLARE_FINAL_TYPE (PnTappedDelay, pn_tapped_delay,
                      PN, TAPPED_DELAY, PnNode)

PnTappedDelay *pn_tapped_delay_new (void);

/**
 * pn_tapped_delay_get_n_pending:
 *
 * Number of message copies still waiting on their timers, summed over
 * every output.
 */
guint          pn_tapped_delay_get_n_pending (PnTappedDelay *self);

/**
 * pn_tapped_delay_format_delay:
 * @ms: a delay in milliseconds
 *
 * The worksheet label for an output delayed by @ms: "250 ms" below one
 * second, "1.5 s" from one second up.  Free with g_free().
 */
gchar         *pn_tapped_delay_format_delay (guint ms);

G_END_DECLS

#endif /* PN_TAPPED_DELAY_H */
