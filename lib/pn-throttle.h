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

#ifndef PN_THROTTLE_H
#define PN_THROTTLE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnThrottle                                                         */
/*                                                                     */
/*  Rate-limiting filter.  Forwards a message only when at least       */
/*  #PnThrottle:interval seconds have elapsed since the previously     */
/*  forwarded one; the very first message after construction is        */
/*  always passed through so a quiet stream is never silenced.         */
/*  Dropped messages are discarded; there is no queue.                 */
/* ------------------------------------------------------------------ */

#define PN_TYPE_THROTTLE (pn_throttle_get_type ())

G_DECLARE_FINAL_TYPE (PnThrottle, pn_throttle, PN, THROTTLE, PnNode)

PnThrottle *pn_throttle_new (void);

G_END_DECLS

#endif /* PN_THROTTLE_H */
