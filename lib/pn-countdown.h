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

#ifndef PN_COUNTDOWN_H
#define PN_COUNTDOWN_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnCountdown                                                        */
/*                                                                     */
/*  Sink node that paints a digital countdown clock — a black panel of */
/*  seven-segment LED digits laid out as DAYS HOURS:MINUTES:SECONDS,   */
/*  the look of a wall-mounted event countdown display.  Each incoming */
/*  message carries a numeric data.value holding the seconds remaining */
/*  — exactly what the #PnDeadline node emits.  The node breaks that    */
/*  total into days / hours / minutes / seconds and lights the digits  */
/*  accordingly; a value of zero or below (the deadline has passed, or  */
/*  nothing has arrived yet) shows all zeroes.                          */
/*                                                                     */
/*  Being a pure sink it never forwards a message.  The seven-segment   */
/*  cairo drawing lives in the gui tier (pn-countdown-gui.c); the       */
/*  headless core keeps the logic, geometry and the GTK-free paint-     */
/*  state seam the painter reads from.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_COUNTDOWN (pn_countdown_get_type ())

G_DECLARE_FINAL_TYPE (PnCountdown, pn_countdown, PN, COUNTDOWN, PnNode)

PnCountdown *pn_countdown_new (void);

/**
 * pn_countdown_set_value:
 * @self:    countdown instance
 * @seconds: seconds remaining until the target instant
 *
 * Push a fresh "seconds remaining" reading into the display.  Identical
 * to what the built-in receiver does when a numeric data.value lands on
 * the input: stores the value and schedules a repaint when the displayed
 * digits would change.  Values of zero or below light all zeroes.  A
 * non-finite value is ignored.
 */
void pn_countdown_set_value (PnCountdown *self, gdouble seconds);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Every field the seven-segment painter (pn-countdown-gui.c) needs to */
/*  draw a frame, snapshotted by value through                         */
/*  pn_countdown_get_paint_state().  The breakdown into days / hours /  */
/*  minutes / seconds is done here (clamped at zero) so the arithmetic  */
/*  is GTK-free and unit-testable; the painter only positions glyphs.   */
/*  The colours are #PnColor (layout-identical to GdkRGBA).             */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint64    days;          /* whole days remaining (>= 0)            */
    gint      hours;         /* 0..23                                  */
    gint      minutes;       /* 0..59                                  */
    gint      seconds;       /* 0..59                                  */

    guint     day_digits;    /* how many day digits to show (1..6)     */
    gboolean  show_labels;   /* draw the DAYS/HOURS/… captions         */

    PnColor   background_color;
    PnColor   segment_color;        /* lit seven-segment bar             */
    PnColor   unlit_segment_color;  /* dark off-state ghost bar          */
    PnColor   label_color;
} PnCountdownPaintState;

/**
 * pn_countdown_get_paint_state:
 * @self: countdown instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out, with the live
 * "seconds remaining" already split into the clamped days / hours /
 * minutes / seconds the display shows.  GTK-free.
 */
void pn_countdown_get_paint_state (PnCountdown           *self,
                                   PnCountdownPaintState *out);

G_END_DECLS

#endif /* PN_COUNTDOWN_H */
