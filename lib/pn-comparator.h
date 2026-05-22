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

#ifndef PN_COMPARATOR_H
#define PN_COMPARATOR_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnComparator                                                       */
/*                                                                     */
/*  Two-input ordering comparator — the live-reference sibling of      */
/*  PnThreshold.  Input 1 (A) and input 2 (B) each carry a numeric     */
/*  data.value; the node latches the most recent value seen on each    */
/*  and tracks a single ON/OFF state answering "is A at or above B?".  */
/*                                                                     */
/*  The #PnComparator:hysteresis defines a dead-band centered on       */
/*  equality (A == B): the state turns ON once A reaches B +           */
/*  hysteresis/2 and turns OFF again only once A falls below B -        */
/*  hysteresis/2, so a pair of values hovering around each other does  */
/*  not flap the output.  A message is forwarded only when the state   */
/*  actually changes, and its data.value is overwritten with 1.0       */
/*  (A >= B) or 0.0 (A < B) so downstream nodes see a clean boolean.   */
/*                                                                     */
/*  No comparison happens until both inputs have received at least one */
/*  numeric value; with #PnComparator:hysteresis 0 the node is a plain */
/*  "A >= B" comparator that emits on every crossing.                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_COMPARATOR (pn_comparator_get_type ())

G_DECLARE_FINAL_TYPE (PnComparator, pn_comparator, PN, COMPARATOR, PnNode)

PnComparator *pn_comparator_new (void);

G_END_DECLS

#endif /* PN_COMPARATOR_H */
