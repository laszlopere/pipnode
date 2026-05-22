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

#ifndef PN_THRESHOLD_H
#define PN_THRESHOLD_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnThreshold                                                        */
/*                                                                     */
/*  Schmitt-trigger comparator.  Watches the numeric data.value of     */
/*  each incoming message and tracks a single ON/OFF state.  The       */
/*  #PnThreshold:hysteresis defines a dead-band centered on            */
/*  #PnThreshold:threshold: the state turns ON once the value reaches  */
/*  the upper edge (threshold + hysteresis/2) and turns OFF again only */
/*  once it falls below the lower edge (threshold - hysteresis/2);     */
/*  readings inside that band leave the state unchanged, which is what */
/*  suppresses chatter around the trip point.  A message is forwarded  */
/*  only when the state actually changes, and its data.value is        */
/*  overwritten with 1.0 (on) or 0.0 (off) so downstream nodes see a   */
/*  clean boolean.                                                     */
/*                                                                     */
/*  With #PnThreshold:hysteresis set to 0 this degenerates to a plain  */
/*  "value >= threshold" comparator that emits on every crossing.      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_THRESHOLD (pn_threshold_get_type ())

G_DECLARE_FINAL_TYPE (PnThreshold, pn_threshold, PN, THRESHOLD, PnNode)

PnThreshold *pn_threshold_new (void);

G_END_DECLS

#endif /* PN_THRESHOLD_H */
