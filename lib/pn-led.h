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
/*  PnLedMode                                                          */
/*                                                                     */
/*  The two ways the lamp reacts to incoming messages:                 */
/*                                                                     */
/*    - FLASH:  every message lights the LED for the configured hold   */
/*              period, a momentary activity blink.  This is the       */
/*              historical behaviour and the default.                  */
/*    - STEADY: the LED latches to the boolean carried on data.value   */
/*              (on while value > 0.5, off otherwise), so it reads as  */
/*              a permanent state lamp.  The hold period is irrelevant */
/*              here -- the lamp follows the value, it never            */
/*              self-extinguishes on a timer.                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LED_MODE (pn_led_mode_get_type ())

typedef enum
{
    PN_LED_MODE_FLASH,
    PN_LED_MODE_STEADY,
} PnLedMode;

GType pn_led_mode_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnLed                                                              */
/*                                                                     */
/*  Sink node shaped like a standard Node-RED rectangle with a small  */
/*  rectangular LED indicator inset on the right side of the header.   */
/*  In Flash mode every incoming message lights the LED in its         */
/*  configured colour and resets the off timer, so the LED stays on    */
/*  until the configured hold period elapses with no further messages. */
/*  In Steady mode the LED instead latches to the boolean on           */
/*  data.value and the hold period is ignored.                         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LED (pn_led_get_type ())

G_DECLARE_FINAL_TYPE (PnLed, pn_led, PN, LED, PnNode)

PnLed *pn_led_new (void);

G_END_DECLS

#endif /* PN_LED_H */
