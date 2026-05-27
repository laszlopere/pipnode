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

#ifndef PN_NUMERIC_H
#define PN_NUMERIC_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNumeric                                                          */
/*                                                                     */
/*  Sink node that paints a numeric reading on a row of seven-segment  */
/*  LED digits — the generic-number cousin of #PnCountdown.  Each      */
/*  incoming message carries a numeric data.value; the node renders     */
/*  the value as an integer part plus optional fractional digits with   */
/*  a fixed decimal point, and lights a leading minus-sign cell when    */
/*  the value is negative.  No captions are drawn — the display is just */
/*  the LED panel.                                                     */
/*                                                                     */
/*  Layout (always the same width regardless of the value, so a feed   */
/*  that swings across zero does not visually jitter):                  */
/*                                                                     */
/*      [sign][int_digits].[frac_digits]                                */
/*                                                                     */
/*  When @decimal-places is zero the decimal point and the fractional   */
/*  block are omitted.  When |value| does not fit in @digits integer    */
/*  cells the whole row reads as dashes ("- - -.- -"), the LED-meter    */
/*  overload indicator.                                                 */
/*                                                                     */
/*  Being a pure sink the node never forwards.  The seven-segment      */
/*  cairo drawing lives in pn-numeric-gui.c.                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NUMERIC (pn_numeric_get_type ())

G_DECLARE_FINAL_TYPE (PnNumeric, pn_numeric, PN, NUMERIC, PnNode)

PnNumeric *pn_numeric_new (void);

/**
 * pn_numeric_set_value:
 * @self:  numeric instance
 * @value: number to display
 *
 * Push a fresh reading into the display.  Identical to what the built-in
 * receiver does when a numeric data.value lands on the input: stores the
 * value and schedules a repaint when the visible digits would change.
 * A non-finite value is ignored.
 */
void pn_numeric_set_value (PnNumeric *self, gdouble value);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    gdouble   value;            /* the value the display should show     */
    gboolean  has_value;         /* FALSE before the first message lands  */

    guint     digits;            /* integer digit cells (1..12)           */
    guint     decimal_places;    /* fractional digit cells (0..6)         */

    PnColor   background_color;
    PnColor   segment_color;        /* lit seven-segment bar               */
    PnColor   unlit_segment_color;  /* off-state ghost bar                 */
} PnNumericPaintState;

/**
 * pn_numeric_get_paint_state:
 * @self: numeric instance
 * @out:  (out): caller-provided snapshot filled with the current state
 *
 * Copy the fields the gui-tier painter needs into @out.  GTK-free.
 */
void pn_numeric_get_paint_state (PnNumeric           *self,
                                 PnNumericPaintState *out);

G_END_DECLS

#endif /* PN_NUMERIC_H */
