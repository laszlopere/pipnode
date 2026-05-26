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
/*  Four ways the lamp reacts to incoming messages.  FLASH is per-     */
/*  message activity; the other three are level-driven and latch to    */
/*  the boolean on data.value (on while value > 0.5, off otherwise):   */
/*                                                                     */
/*    - FLASH:      every message lights the LED for the configured    */
/*                  hold period, a momentary activity blink.  This is  */
/*                  the historical behaviour and the default.          */
/*    - STEADY:     latches to data.value as a permanent state lamp.   */
/*    - BLINK_SLOW: while data.value is on the LED blinks at ~1 Hz     */
/*                  (500 ms on / 500 ms off); off otherwise.           */
/*    - BLINK_FAST: while data.value is on the LED blinks at ~4 Hz     */
/*                  (125 ms on / 125 ms off); off otherwise.           */
/*                                                                     */
/*  The hold period only governs FLASH; it is ignored in the other     */
/*  modes (which run from the value, not a self-extinguish timer).     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LED_MODE (pn_led_mode_get_type ())

typedef enum
{
    PN_LED_MODE_FLASH,
    PN_LED_MODE_STEADY,
    PN_LED_MODE_BLINK_SLOW,
    PN_LED_MODE_BLINK_FAST,
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
/*  In Steady, Blink Slow and Blink Fast modes the LED instead latches */
/*  to the boolean on data.value: Steady holds the level, Blink Slow   */
/*  oscillates at ~1 Hz while on, Blink Fast at ~4 Hz.  The hold       */
/*  period is ignored outside Flash.                                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LED (pn_led_get_type ())

G_DECLARE_FINAL_TYPE (PnLed, pn_led, PN, LED, PnNode)

PnLed *pn_led_new (void);

/* ------------------------------------------------------------------ */
/*  GUI read seams (GTK-free)                                          */
/*                                                                     */
/*  The lit flag and lit colour live in the private instance struct.   */
/*  The cairo painter — which the gui tier installs onto this class    */
/*  (see pn_led_gui_install in pn-led-gui.c) — reads them through       */
/*  these accessors rather than reaching into the struct, so the       */
/*  drawing code can live in a separate translation unit.              */
/* ------------------------------------------------------------------ */

gboolean pn_led_get_lit    (PnLed *self);
void     pn_led_peek_color (PnLed *self, PnColor *out);

G_END_DECLS

#endif /* PN_LED_H */
