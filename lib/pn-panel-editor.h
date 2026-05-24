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

#ifndef PN_PANEL_EDITOR_H
#define PN_PANEL_EDITOR_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_PANEL_EDITOR (pn_panel_editor_get_type ())

G_DECLARE_FINAL_TYPE (PnPanelEditor,
                      pn_panel_editor,
                      PN, PANEL_EDITOR,
                      GtkDrawingArea)

/**
 * pn_panel_editor_new:
 *
 * Creates a new panel-applet GUI layout editor widget.
 *
 * This is the first of a planned family of GUI-layout editors (panel,
 * desktop, web, mobile) that will eventually let the user lay out the
 * widgets a pipnode flow drives — distinct from the node-wiring
 * #PnWorksheet, which edits the dataflow itself.  At this stage the
 * widget is a placeholder: it paints a mock of an XFCE panel with an
 * empty applet slot and a caption, and carries no editing behaviour
 * yet.
 *
 * Returns: (transfer floating): the new panel editor widget
 */
GtkWidget *pn_panel_editor_new (void);

G_END_DECLS

#endif /* PN_PANEL_EDITOR_H */
