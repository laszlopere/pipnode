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

#ifndef PN_KNOB_H
#define PN_KNOB_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnKnob                                                             */
/*                                                                     */
/*  Manual source node carrying a rotary knob on the right of its      */
/*  header.  Spinning the mouse wheel while the pointer is over the    */
/*  knob rotates it between a configurable minimum and maximum; every  */
/*  rotation emits a single message whose "value" data member holds    */
/*  the current knob position mapped onto [min, max].                  */
/*                                                                     */
/*  The two range bounds default to 0.0 (min) and 1.0 (max), so out    */
/*  of the box the knob sweeps a normalised 0..1 value.  Both bounds   */
/*  and the current value are exposed as editable properties; writing  */
/*  them updates the dial without emitting -- only a wheel rotation     */
/*  emits, mirroring how PnSwitch only emits on a click.               */
/*                                                                     */
/*  One exception to "only a rotation emits": the knob announces its    */
/*  current position once, shortly after it is constructed, so a        */
/*  freshly-loaded worksheet lets downstream nodes learn the knob's     */
/*  state without the user having to turn it.  This is a single         */
/*  startup shot, not the recurring tick of a PnAutoTrigger data        */
/*  source.                                                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_KNOB (pn_knob_get_type ())

G_DECLARE_FINAL_TYPE (PnKnob, pn_knob, PN, KNOB, PnNode)

PnKnob *pn_knob_new (void);

/**
 * pn_knob_get_value:
 * @self: the knob node
 *
 * Returns the current value the knob represents, in [min, max].
 */
gdouble pn_knob_get_value (PnKnob *self);

/**
 * pn_knob_set_value:
 * @self:  the knob node
 * @value: the new value
 *
 * Sets the knob value (clamped into the [min, max] range) and
 * refreshes the dial.  Does not emit a message -- use it for
 * programmatic / dialog-driven configuration.  A wheel rotation via
 * pn_knob_scroll() is what emits.
 */
void pn_knob_set_value (PnKnob *self, gdouble value);

/**
 * pn_knob_hit_knob:
 * @self: the knob node
 * @px:   x in worksheet coordinates
 * @py:   y in worksheet coordinates
 *
 * Reports whether (@px, @py) lands on the circular knob decoration.
 * Exposed so the worksheet can route a scroll event straight into
 * pn_knob_scroll() without duplicating the decoration's geometry.
 */
gboolean pn_knob_hit_knob (PnKnob *self, double px, double py);

/**
 * pn_knob_scroll:
 * @self: the knob node
 * @dy:   smooth-scroll delta on the Y axis (negative = wheel up)
 *
 * Rotates the knob by one wheel step in the direction of @dy (wheel
 * up increases the value, wheel down decreases it), clamped to the
 * [min, max] range.  When the value actually changes the dial
 * repaints and a fresh message carrying the new "value" is emitted.
 */
void pn_knob_scroll (PnKnob *self, double dy);

/**
 * pn_knob_turn_to:
 * @self:  the knob node
 * @value: the value to turn the knob to
 *
 * The programmatic twin of a wheel turn: clamps @value into [min, max],
 * refreshes the dial when the value changes, and emits a message
 * carrying the new "value".  Unlike pn_knob_scroll() at an end stop it
 * emits on EVERY call, even when the clamped value equals the current
 * one, so each call yields exactly one message.  Used by the D-Bus
 * SetControlValue method; pn_knob_set_value() stays silent.
 */
void pn_knob_turn_to (PnKnob *self, gdouble value);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The current value and range bounds live in the private instance    */
/*  struct.  The cairo painter — which the gui tier installs onto this  */
/*  class (see pn_knob_gui_install in pn-knob-gui.c) — reads the        */
/*  derived pointer position through this accessor rather than reaching */
/*  into the struct, so the drawing code can live in a separate         */
/*  translation unit.                                                   */
/* ------------------------------------------------------------------ */

/**
 * pn_knob_get_value_fraction:
 * @self: the knob node
 *
 * Returns the knob's normalised pointer position in [0, 1]: 0 at the
 * minimum bound, 1 at the maximum.  Collapses to 0 for a zero-width
 * range.  Used by the gui-tier dial painter.
 */
gdouble pn_knob_get_value_fraction (PnKnob *self);

G_END_DECLS

#endif /* PN_KNOB_H */
