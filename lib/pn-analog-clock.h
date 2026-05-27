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

#ifndef PN_ANALOG_CLOCK_H
#define PN_ANALOG_CLOCK_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAnalogClock                                                      */
/*                                                                     */
/*  Sink node that paints a round analogue wall-clock face: a white     */
/*  dial behind black hour markers and three pivoted hands (hour,       */
/*  minute, second).  Sized as a square like #PnDial — a 220-px header  */
/*  on top of a 220-px circular face — but driven by the same data feed */
/*  as #PnDigitalClock: each incoming message carries a count of        */
/*  seconds on data.value, taken modulo a day and shown on a 24-hour    */
/*  face (the hour hand sweeps twice).  A configurable text in place of */
/*  the manufacturer logo sits below the centre pivot.                  */
/*                                                                     */
/*  Pure sink — never forwards.  The cairo face/hands drawing lives in  */
/*  the gui tier (pn-analog-clock-gui.c); the headless core keeps the   */
/*  logic, geometry and the GTK-free paint-state seam.                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ANALOG_CLOCK (pn_analog_clock_get_type ())

G_DECLARE_FINAL_TYPE (PnAnalogClock, pn_analog_clock, PN, ANALOG_CLOCK,
                      PnNode)

PnAnalogClock *pn_analog_clock_new (void);

/**
 * pn_analog_clock_set_value:
 * @self:    analogue-clock instance
 * @seconds: a count of seconds
 *
 * Push a fresh seconds reading into the face.  Identical to what the
 * built-in receiver does when a numeric data.value lands on the input:
 * stores the value and schedules a repaint when the displayed hands
 * would change.  The value is taken modulo a day, so the readout reads
 * as a 24-hour clock face (the hour hand sweeps twice); zero or below
 * parks the hands at 12 o'clock.  A non-finite value is ignored.
 */
void pn_analog_clock_set_value (PnAnalogClock *self, gdouble seconds);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Every field the cairo face painter (pn-analog-clock-gui.c) needs to */
/*  draw a frame, snapshotted by value through                          */
/*  pn_analog_clock_get_paint_state().  The breakdown into hours /      */
/*  minutes / seconds is done here (clamped, taken modulo a day) so the */
/*  arithmetic is GTK-free and unit-testable; the painter only          */
/*  positions glyphs and hands.  The colours are #PnColor (layout-      */
/*  identical to GdkRGBA); @text is a borrowed pointer owned by the     */
/*  node, valid for the duration of the paint call.                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint      hours;         /* 0..23                                  */
    gint      minutes;       /* 0..59                                  */
    gint      seconds;       /* 0..59                                  */

    const gchar *text;       /* configurable text under the pivot      */
    gboolean  show_seconds;  /* draw the seconds hand                  */

    PnColor   face_color;
    PnColor   marker_color;       /* hour markers + minute pips        */
    PnColor   hour_hand_color;
    PnColor   minute_hand_color;
    PnColor   second_hand_color;
    PnColor   text_color;
} PnAnalogClockPaintState;

/**
 * pn_analog_clock_get_paint_state:
 * @self: analogue-clock instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out, with the live
 * seconds reading already split into the clamped, day-wrapped hours /
 * minutes / seconds the face shows.  GTK-free.
 */
void pn_analog_clock_get_paint_state (PnAnalogClock           *self,
                                      PnAnalogClockPaintState *out);

G_END_DECLS

#endif /* PN_ANALOG_CLOCK_H */
