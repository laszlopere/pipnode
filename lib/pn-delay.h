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

#ifndef PN_DELAY_H
#define PN_DELAY_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDelay                                                            */
/*                                                                     */
/*  Time-shifting filter.  Forwards every message it receives          */
/*  unchanged, but only after #PnDelay:delay-ms milliseconds have      */
/*  elapsed.  Each message is held independently and emitted on its    */
/*  own one-shot main-loop timer, so a burst arrives downstream as     */
/*  the same burst shifted later in time (no coalescing, no dropping,  */
/*  order preserved).  Pending messages are released without emission  */
/*  when the node is destroyed.                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DELAY (pn_delay_get_type ())

G_DECLARE_FINAL_TYPE (PnDelay, pn_delay, PN, DELAY, PnNode)

PnDelay *pn_delay_new (void);

G_END_DECLS

#endif /* PN_DELAY_H */
