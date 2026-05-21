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

G_END_DECLS

#endif /* PN_KNOB_H */
