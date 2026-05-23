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

#ifndef PN_PALETTE_H
#define PN_PALETTE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPalette                                                          */
/*                                                                     */
/*  Sidebar widget that lists the available node types and lets the    */
/*  user drag instances onto a #PnWorksheet.  Each entry is a button   */
/*  registered as a drag source whose payload is the registered        */
/*  #GType name of the node class.                                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PALETTE (pn_palette_get_type ())

G_DECLARE_FINAL_TYPE (PnPalette, pn_palette, PN, PALETTE, GtkBox)

GtkWidget *pn_palette_new (void);

/**
 * pn_palette_drag_target_name:
 *
 * Atom name used for the drag-and-drop payload that carries a node
 * type's #GType name.  The worksheet registers this same name as
 * its accepted drag target.
 */
const gchar *pn_palette_drag_target_name (void);

/**
 * pn_palette_get_drag_node_type:
 * @self: the palette
 *
 * Returns the #GType of the node currently being dragged out of the
 * palette, or %G_TYPE_INVALID when no palette drag is in progress.
 * Lets a same-app drop site (the worksheet) learn the dragged type
 * during drag-motion without a selection round-trip — fetching the
 * payload via gtk_drag_get_data() while the DnD pointer grab is held
 * can spin a nested main loop and lock up the display.
 */
GType pn_palette_get_drag_node_type (PnPalette *self);

G_END_DECLS

#endif /* PN_PALETTE_H */
