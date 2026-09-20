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

#ifndef PN_CALC_ENGINE_H
#define PN_CALC_ENGINE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnCalcEngine                                                       */
/*                                                                     */
/*  The arithmetic half of a four-function calculator: the part that   */
/*  turns a stream of keystrokes into a running display.  Wire a       */
/*  #PnKeypad into it and a #PnNumeric (the seven-segment readout) out */
/*  of it and the three nodes are a working pocket calculator.         */
/*                                                                     */
/*  Each incoming message is one keystroke, read from its `data.key`   */
/*  member — exactly what #PnKeypad emits, though anything that writes */
/*  the same member (an Injector, an MQTT feed) drives it just as      */
/*  well.  Messages carrying no usable `data.key` are ignored in       */
/*  silence, so a stray reading cannot corrupt a half-typed sum.       */
/*                                                                     */
/*  Every accepted keystroke emits the *whole display*, not just       */
/*  results, so the readout tracks the typing digit by digit:          */
/*                                                                     */
/*    data.value    the displayed number                               */
/*    data.output   the display as text — "12.", "-3.5", "Error"       */
/*    data.success  %FALSE once the calculation has errored            */
/*                                                                     */
/*  The state machine is the conventional one.  An operator key folds  */
/*  the typed entry into the accumulator and shows the running total,  */
/*  so `2 + 3 + 4 =` displays 5 after the second `+` and 9 after `=`.  */
/*  `C` clears everything; `CE` clears only the number being typed,    */
/*  leaving the pending operation intact.  Dividing by zero latches an */
/*  error: the node paints its error state, emits `success = FALSE`,   */
/*  and ignores every key but `C` until it is cleared — the same dead  */
/*  end a real calculator's "E" is.                                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_CALC_ENGINE (pn_calc_engine_get_type ())

G_DECLARE_FINAL_TYPE (PnCalcEngine, pn_calc_engine, PN, CALC_ENGINE, PnNode)

PnCalcEngine *pn_calc_engine_new (void);

/**
 * pn_calc_engine_press:
 * @self: the engine
 * @key:  a key code — "0".."9", ".", "+", "-", "*", "/", "=", "C" or
 *        "CE", as #PnKeypad emits them
 *
 * Feeds one keystroke into the state machine and emits the resulting
 * display.  Returns %FALSE, changing and emitting nothing, when @key
 * names no key the engine knows, or when the engine is latched in its
 * error state and @key is not "C".
 *
 * This is the whole node's logic seam: receive() is a thin wrapper
 * that pulls `data.key` out of a message and calls this.  Exposed so
 * headless tests — and the D-Bus automation surface — can drive a
 * calculation without building messages.
 */
gboolean pn_calc_engine_press (PnCalcEngine *self,
                               const gchar  *key);

/**
 * pn_calc_engine_get_display:
 * @self: the engine
 *
 * Returns the number currently on the display — what the last emitted
 * message carried under `data.value`.  Zero while the engine is in its
 * error state.
 */
gdouble pn_calc_engine_get_display (PnCalcEngine *self);

/**
 * pn_calc_engine_get_display_text:
 * @self: the engine
 *
 * Returns the display as the user sees it being typed (borrowed, never
 * %NULL): "12." keeps the decimal point the user has entered but not
 * yet filled in, and an errored engine reads "Error".  This is what
 * the last emitted message carried under `data.output`.
 */
const gchar *pn_calc_engine_get_display_text (PnCalcEngine *self);

/**
 * pn_calc_engine_get_error:
 * @self: the engine
 *
 * Returns whether the calculation has errored (today: a division by
 * zero).  While set, every key but "C" is ignored.
 */
gboolean pn_calc_engine_get_error (PnCalcEngine *self);

/**
 * pn_calc_engine_reset:
 * @self: the engine
 *
 * Clears the accumulator, the pending operation, the typed entry and
 * any error — what pressing "C" does, minus the emission.  Used by the
 * node itself on construction; exposed for callers that want a known
 * starting point without putting a message on the wire.
 */
void pn_calc_engine_reset (PnCalcEngine *self);

G_END_DECLS

#endif /* PN_CALC_ENGINE_H */
