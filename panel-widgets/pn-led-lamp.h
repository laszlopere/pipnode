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

#ifndef PN_LED_LAMP_H
#define PN_LED_LAMP_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized circular indicator lamp.
 *
 * This is a tiny, self-contained restatement of the LED node's header
 * indicator: a through-hole LED clipped into a black panel-mount bracket,
 * a coloured glass dome with a catch-light, and — while lit — a soft halo
 * of the lamp's colour spilling out around the bracket.  It carries a
 * boolean lit state and a lit colour; the unlit dome is a neutral grey so
 * an off lamp still reads as an LED rather than empty space.
 *
 * It deliberately depends on GTK/cairo only — no pipnode library — so it
 * can live inside the panel applet without putting node code in
 * xfce4-panel's address space.  The colour is taken as plain RGB doubles
 * rather than a node #PnColor for exactly that reason. */

#define PN_TYPE_LED_LAMP (pn_led_lamp_get_type ())
G_DECLARE_FINAL_TYPE (PnLedLamp, pn_led_lamp, PN, LED_LAMP, GtkDrawingArea)

GtkWidget *pn_led_lamp_new       (void);

/* Light the lamp (TRUE) or darken it (FALSE).  Repaints live whenever the
 * state changes. */
void       pn_led_lamp_set_lit   (PnLedLamp *self, gboolean lit);

/* Set the colour the dome glows in while lit, as RGB in 0..1.  The unlit
 * dome is always a neutral grey; only the lit state uses this colour. */
void       pn_led_lamp_set_color (PnLedLamp *self,
                                  gdouble    red,
                                  gdouble    green,
                                  gdouble    blue);

/* Set the overall pixel size (a square) the lamp should occupy, typically
 * the panel's icon size; the bracket/dome geometry is derived from it. */
void       pn_led_lamp_set_size  (PnLedLamp *self, gint size);

G_END_DECLS

#endif /* PN_LED_LAMP_H */
