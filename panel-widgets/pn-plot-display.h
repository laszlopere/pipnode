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

#ifndef PN_PLOT_DISPLAY_H
#define PN_PLOT_DISPLAY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A rectangular plot area showing a picture the engine painted for us.
 *
 * The odd one out among the panel widgets.  Every other one restates a
 * node's look in self-contained cairo code, because the look is simple
 * enough to say twice.  A plot is not: PnGraph draws through PLplot,
 * PnGraph's 3D views and the weather card through MathGL and Pango, and
 * those libraries may never be dragged into xfce4-panel or the viewer —
 * the whole point of this library is that it links no pipnode runtime and
 * no plotting stack.  So the engine, which HAS all of that, renders the
 * node's own paint_plot output to a PNG and ships the bytes; this widget
 * decodes them and blits the result.  The picture is authoritative: what
 * the viewer shows is pixel-for-pixel what the editor's canvas shows.
 *
 * Sized in pixels by the layout (see pn_plot_display_set_size), not by the
 * surface's row height, because a plot needs an area rather than a line.
 * Until the first picture arrives — and whenever one fails to decode — it
 * paints an empty framed rectangle, so a plot that has not received data
 * yet reads as a deliberate area rather than a hole in the window. */

#define PN_TYPE_PLOT_DISPLAY (pn_plot_display_get_type ())
G_DECLARE_FINAL_TYPE (PnPlotDisplay, pn_plot_display, PN, PLOT_DISPLAY,
                      GtkDrawingArea)

GtkWidget *pn_plot_display_new (void);

/* Set the pixel area the plot occupies.  Non-positive values are ignored,
 * so a state that carries no size leaves the last one standing. */
void pn_plot_display_set_size (PnPlotDisplay *self, gint width, gint height);

/* Replace the picture with the base64-encoded PNG in @png_base64.  An
 * empty or unparsable string clears it back to the empty frame.  The
 * decoded surface is cached, so repeated identical states are cheap. */
void pn_plot_display_set_png_base64 (PnPlotDisplay *self,
                                     const gchar   *png_base64);

G_END_DECLS

#endif /* PN_PLOT_DISPLAY_H */
