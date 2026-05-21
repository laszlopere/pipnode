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

#ifndef PN_LED_H
#define PN_LED_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnLed                                                              */
/*                                                                     */
/*  Sink node shaped like a standard Node-RED rectangle with a small  */
/*  rectangular LED indicator inset on the right side of the header.   */
/*  Every incoming message lights the LED in its configured colour     */
/*  and resets the off timer; the LED stays on until the configured    */
/*  hold period elapses with no further messages.                      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LED (pn_led_get_type ())

G_DECLARE_FINAL_TYPE (PnLed, pn_led, PN, LED, PnNode)

PnLed *pn_led_new (void);

G_END_DECLS

#endif /* PN_LED_H */
