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

#ifndef PN_TEXT_DISPLAY_H
#define PN_TEXT_DISPLAY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized text readout — the panel-applet face of the Label node.
 *
 * It draws one or two lines of text in a configurable font and colour on
 * an optional rounded background, fitted to the panel row: the text is
 * shrunk and ellipsised as needed so it never spills past the widget.
 * The two-line tail and the source text are computed node-side; this
 * widget only renders the string it is handed.
 *
 * Like the other panel widgets it deliberately depends on GTK/cairo/Pango
 * only — no pipnode library — so it can live inside the panel applet
 * without putting node code in xfce4-panel's address space. */

#define PN_TYPE_TEXT_DISPLAY (pn_text_display_get_type ())
G_DECLARE_FINAL_TYPE (PnTextDisplay, pn_text_display, PN, TEXT_DISPLAY,
                      GtkDrawingArea)

GtkWidget *pn_text_display_new (void);

/* The text to show.  May contain a single '\n' for a two-line label;
 * %NULL or "" blanks the widget. */
void pn_text_display_set_text (PnTextDisplay *self, const gchar *text);

/* How many text lines the widget is laid out for (1 or 2) — the per-line
 * height is derived from this and the widget height. */
void pn_text_display_set_lines (PnTextDisplay *self, gint lines);

/* Colour of the text. */
void pn_text_display_set_color (PnTextDisplay *self,
                                gdouble red, gdouble green, gdouble blue);

/* Fill colour of the rounded background box.  An alpha at or near zero
 * leaves the background transparent so the panel shows through. */
void pn_text_display_set_background (PnTextDisplay *self,
                                     gdouble red, gdouble green,
                                     gdouble blue, gdouble alpha);

/* Font the text is drawn in: family name, scale (text height as a percent
 * of the height one line gets in the box, so it matches the worksheet
 * readout at this widget's scale), Pango weight number (400/500/600/700)
 * and italic flag. */
void pn_text_display_set_font (PnTextDisplay *self,
                               const gchar   *family,
                               gint           scale,
                               gint           weight,
                               gboolean       italic);

/* Horizontal alignment: 0 = left, 1 = centre, 2 = right. */
void pn_text_display_set_align (PnTextDisplay *self, gint align);

/* Set the overall pixel height the readout should occupy (typically the
 * panel's icon size); the text size is fitted within it. */
void pn_text_display_set_height (PnTextDisplay *self, gint height);

G_END_DECLS

#endif /* PN_TEXT_DISPLAY_H */
