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

#ifndef PN_NUMERIC_DISPLAY_H
#define PN_NUMERIC_DISPLAY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized seven-segment numeric readout.
 *
 * A miniature of the Numeric node's client area: the same hexagonal-bar
 * geometry, sign cell, leading-blank integer padding, and decimal-point
 * dot, sized to a panel row.  Fed a double value and a layout (integer
 * digits + decimal places), it renders the same look as the worksheet
 * painter (pn-numeric-gui.c) — minus the bezel and screen face, so the
 * panel's transparent background shows through.
 *
 * Self-contained cairo + GTK so the applet never links a pipnode
 * library.  The horizontal size is a function of the requested height
 * and the digit layout; the widget asks the panel for exactly that
 * width (get_preferred_width). */

#define PN_TYPE_NUMERIC_DISPLAY (pn_numeric_display_get_type ())
G_DECLARE_FINAL_TYPE (PnNumericDisplay, pn_numeric_display, PN, NUMERIC_DISPLAY,
                      GtkDrawingArea)

GtkWidget *pn_numeric_display_new            (void);

/* Set the value to display.  Non-finite values are ignored.  The display
 * repaints whenever the visible digits would change. */
void       pn_numeric_display_set_value      (PnNumericDisplay *self,
                                              gdouble           value);

/* Mark the display as "no reading yet": every cell shows only the
 * off-state ghost (and the sign cell is blank), matching the worksheet
 * node's pre-first-message state.  Reset by the first set_value(). */
void       pn_numeric_display_set_has_value  (PnNumericDisplay *self,
                                              gboolean          has_value);

/* Number of seven-segment cells reserved for the integer part of the
 * value (1..12).  Sized once the layout is known; out-of-range values
 * are clamped. */
void       pn_numeric_display_set_digits     (PnNumericDisplay *self,
                                              guint             digits);

/* Number of cells reserved for the fractional part (0..6).  Zero drops
 * the decimal-point dot and the fractional block entirely. */
void       pn_numeric_display_set_decimal_places (PnNumericDisplay *self,
                                                  guint             places);

/* Colour of a lit seven-segment bar (and the decimal-point dot).  The
 * alpha applies to the bar fill, its halo stroke and the dot — so a
 * translucent lit colour lets the panel background show through evenly. */
void       pn_numeric_display_set_segment_color (PnNumericDisplay *self,
                                                 gdouble red, gdouble green,
                                                 gdouble blue, gdouble alpha);

/* Colour of an unlit seven-segment bar — the off-state ghost.  Alpha
 * lets the ghosts fade into the panel background instead of painting
 * a solid block. */
void       pn_numeric_display_set_unlit_color   (PnNumericDisplay *self,
                                                 gdouble red, gdouble green,
                                                 gdouble blue, gdouble alpha);

/* Set the overall pixel height the readout should occupy (typically the
 * panel's icon size); every other dimension — digit width, bar thickness,
 * inter-cell gap, dot size — is derived from it, so the width is fixed
 * by the height and the configured digit layout. */
void       pn_numeric_display_set_height     (PnNumericDisplay *self,
                                              gint              height);

G_END_DECLS

#endif /* PN_NUMERIC_DISPLAY_H */
