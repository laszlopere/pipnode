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

#ifndef PN_MATRIX57_DISPLAY_H
#define PN_MATRIX57_DISPLAY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized 5x7 dot-matrix LCD readout — the panel-applet face of
 * the Matrix57 node.  Draws @cells character cells of 5x7 black square
 * pixels over a greenish-yellow LCD face inside a black plastic bezel,
 * fitted to the panel row so width follows from the requested height
 * (height-driven aspect ratio, matching how the worksheet node paints).
 *
 * Like the other panel widgets it deliberately depends on GTK/cairo
 * only — no pipnode library — so it can live inside the panel applet
 * without putting node code in xfce4-panel's address space. */

#define PN_TYPE_MATRIX57_DISPLAY (pn_matrix57_display_get_type ())
G_DECLARE_FINAL_TYPE (PnMatrix57Display, pn_matrix57_display,
                      PN, MATRIX57_DISPLAY, GtkDrawingArea)

GtkWidget *pn_matrix57_display_new (void);

/* The text to show.  %NULL or "" blanks the row; longer than @cells is
 * cropped on the right. */
void pn_matrix57_display_set_text (PnMatrix57Display *self,
                                   const gchar       *text);

/* Number of 5x7 character cells in the row (1..40).  Width is a
 * function of cells × height, so the widget queues a resize on change. */
void pn_matrix57_display_set_cells (PnMatrix57Display *self,
                                    guint              cells);

/* LCD face fill.  Defaults to the classic greenish-yellow. */
void pn_matrix57_display_set_background_color (PnMatrix57Display *self,
                                               gdouble red,
                                               gdouble green,
                                               gdouble blue);

/* Lit-dot colour.  Near-black by default, the way an LCD pixel reads. */
void pn_matrix57_display_set_pixel_color (PnMatrix57Display *self,
                                          gdouble red,
                                          gdouble green,
                                          gdouble blue);

/* Unlit-dot colour — the very faint off-state ghost. */
void pn_matrix57_display_set_unlit_pixel_color (PnMatrix57Display *self,
                                                gdouble red,
                                                gdouble green,
                                                gdouble blue);

/* Set the overall pixel height the readout should occupy (typically the
 * panel's icon size); the dot pitch is derived from it. */
void pn_matrix57_display_set_height (PnMatrix57Display *self, gint height);

G_END_DECLS

#endif /* PN_MATRIX57_DISPLAY_H */
