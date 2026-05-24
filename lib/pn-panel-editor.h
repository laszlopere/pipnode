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

#include "pn-flow.h"

G_BEGIN_DECLS

#define PN_TYPE_PANEL_EDITOR (pn_panel_editor_get_type ())

G_DECLARE_FINAL_TYPE (PnPanelEditor,
                      pn_panel_editor,
                      PN, PANEL_EDITOR,
                      GtkBox)

/**
 * pn_panel_editor_new:
 * @flow: (transfer none) (not nullable): the shared #PnFlow whose
 *   #PnCountdown nodes this editor mirrors.  The widget keeps its own
 *   reference; the caller still owns the flow.
 *
 * Creates a panel-applet GUI layout editor bound to @flow.
 *
 * This is the first of a planned family of GUI-layout editors (panel,
 * desktop, web, mobile) that lay out the widgets a flow drives — distinct
 * from the node-wiring #PnWorksheet, which edits the dataflow itself.
 *
 * The editor keeps one live #PnLedDisplay readout per #PnCountdown node
 * across every sheet of @flow: it is populated for all existing countdown
 * nodes on creation, and it grows and shrinks automatically as countdown
 * nodes are added to or removed from the flow.  Each readout mirrors its
 * node's current value live.  The readouts are laid out on a free-
 * positioning canvas and can be dragged around with the mouse; positions
 * are not yet persisted across sessions.
 *
 * Returns: (transfer floating): the new panel editor widget
 */
GtkWidget *pn_panel_editor_new (PnFlow *flow);

G_END_DECLS

#endif /* PN_PANEL_EDITOR_H */
