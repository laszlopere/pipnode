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

#ifndef PN_LED_DISPLAY_H
#define PN_LED_DISPLAY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized seven-segment countdown readout.
 *
 * This is a tiny, self-contained restatement of the Countdown node's
 * cairo client area: red seven-segment digits on a near-black LED face,
 * laid out as a zero-padded days block followed by HH:MM:SS, i.e.
 * "ddd hh:mm:ss".  It is fed a count of seconds remaining (the same
 * data.value the Countdown node reads) and breaks it down the same way.
 *
 * It deliberately depends on GTK/cairo only — no pipnode library — so it
 * can live inside the panel applet without putting node code in
 * xfce4-panel's address space. */

#define PN_TYPE_LED_DISPLAY (pn_led_display_get_type ())
G_DECLARE_FINAL_TYPE (PnLedDisplay, pn_led_display, PN, LED_DISPLAY,
                      GtkDrawingArea)

GtkWidget *pn_led_display_new            (void);

/* Set the value to display, in seconds remaining.  Negative counts are
 * clamped to zero (a passed deadline reads as all-zeroes).  Repaints
 * live whenever the shown digits change. */
void       pn_led_display_set_seconds    (PnLedDisplay *self,
                                          gint64        seconds);

/* Number of seven-segment cells reserved for the days field (default 3,
 * counting up to 999 days). */
void       pn_led_display_set_day_digits (PnLedDisplay *self,
                                          guint         digits);

/* Set the overall pixel height the readout should occupy (typically the
 * panel's icon size); the digit geometry is derived from it. */
void       pn_led_display_set_height     (PnLedDisplay *self,
                                          gint          height);

/* Parse @text (as produced by the Panel Display: an int64 or a real,
 * locale-independent) into a whole count of seconds.  Returns FALSE when
 * @text is not a plain number, so the caller can fall back to text. */
gboolean   pn_led_display_parse_seconds  (const gchar  *text,
                                          gint64       *out_seconds);

G_END_DECLS

#endif /* PN_LED_DISPLAY_H */
