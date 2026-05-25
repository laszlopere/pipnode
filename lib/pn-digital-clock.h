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

#ifndef PN_DIGITAL_CLOCK_H
#define PN_DIGITAL_CLOCK_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDigitalClock                                                      */
/*                                                                     */
/*  Sink node that paints a digital clock — a black panel of seven-     */
/*  segment LED digits laid out as HOURS:MINUTES:SECONDS.  It is the    */
/*  #PnCountdown node with the days field dropped: same red-on-black     */
/*  look, colours and segment sizes, but a narrower client area showing */
/*  only the HH:MM:SS group.  Each incoming message carries a numeric   */
/*  data.value holding a count of seconds; the node takes it modulo a    */
/*  day and lights the digits as a 24-hour clock face.  A value of zero  */
/*  or below (or nothing arrived yet) shows all zeroes.                  */
/*                                                                     */
/*  Being a pure sink it never forwards a message.  The seven-segment   */
/*  cairo drawing lives in the gui tier (pn-digital-clock-gui.c); the    */
/*  headless core keeps the logic, geometry and the GTK-free paint-      */
/*  state seam the painter reads from.                                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DIGITAL_CLOCK (pn_digital_clock_get_type ())

G_DECLARE_FINAL_TYPE (PnDigitalClock, pn_digital_clock, PN, DIGITAL_CLOCK,
                      PnNode)

PnDigitalClock *pn_digital_clock_new (void);

/**
 * pn_digital_clock_set_value:
 * @self:    digital-clock instance
 * @seconds: a count of seconds
 *
 * Push a fresh seconds reading into the display.  Identical to what the
 * built-in receiver does when a numeric data.value lands on the input:
 * stores the value and schedules a repaint when the displayed digits would
 * change.  The value is taken modulo a day, so the readout reads as a
 * 24-hour clock face; zero or below lights all zeroes.  A non-finite value
 * is ignored.
 */
void pn_digital_clock_set_value (PnDigitalClock *self, gdouble seconds);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Every field the seven-segment painter (pn-digital-clock-gui.c)      */
/*  needs to draw a frame, snapshotted by value through                 */
/*  pn_digital_clock_get_paint_state().  The breakdown into hours /     */
/*  minutes / seconds is done here (clamped, taken modulo a day) so the  */
/*  arithmetic is GTK-free and unit-testable; the painter only positions */
/*  glyphs.  The colours are #PnColor (layout-identical to GdkRGBA).     */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint      hours;         /* 0..23                                  */
    gint      minutes;       /* 0..59                                  */
    gint      seconds;       /* 0..59                                  */

    gboolean  show_labels;   /* draw the HOURS/MINUTES/SECONDS captions */

    PnColor   background_color;
    PnColor   segment_color;        /* lit seven-segment bar             */
    PnColor   unlit_segment_color;  /* dark off-state ghost bar          */
    PnColor   label_color;
} PnDigitalClockPaintState;

/**
 * pn_digital_clock_get_paint_state:
 * @self: digital-clock instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out, with the live
 * seconds reading already split into the clamped, day-wrapped hours /
 * minutes / seconds the display shows.  GTK-free.
 */
void pn_digital_clock_get_paint_state (PnDigitalClock           *self,
                                       PnDigitalClockPaintState *out);

G_END_DECLS

#endif /* PN_DIGITAL_CLOCK_H */
