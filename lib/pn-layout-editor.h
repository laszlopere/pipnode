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

#ifndef PN_LAYOUT_EDITOR_H
#define PN_LAYOUT_EDITOR_H

#include <gtk/gtk.h>

#include "pn-flow.h"

G_BEGIN_DECLS

/**
 * PnLayoutEditorKind:
 * @PN_LAYOUT_EDITOR_PANEL: the XFCE panel-applet layout — widgets are
 *   snapped onto a one-widget-tall horizontal band standing in for the
 *   real desktop panel, and their positions are stored in the flow's
 *   panel layout.
 * @PN_LAYOUT_EDITOR_DESKTOP: the desktop-application layout — widgets are
 *   arranged freely inside a window frame whose size the user picks, and
 *   their positions are stored window-relative in the flow's desktop
 *   layout.
 *
 * Which GUI surface a #PnLayoutEditor lays widgets out for.  One editor
 * class serves the whole family (panel, desktop, and the web and mobile
 * surfaces planned to follow); the kind selects the target geometry, the
 * guide drawn behind the widgets, the snapping rule and which of the
 * flow's layout maps the placements are saved into.
 */
typedef enum
{
    PN_LAYOUT_EDITOR_PANEL,
    PN_LAYOUT_EDITOR_DESKTOP
} PnLayoutEditorKind;

#define PN_TYPE_LAYOUT_EDITOR (pn_layout_editor_get_type ())

G_DECLARE_FINAL_TYPE (PnLayoutEditor,
                      pn_layout_editor,
                      PN, LAYOUT_EDITOR,
                      GtkBox)

/**
 * pn_layout_editor_new:
 * @flow: (transfer none) (not nullable): the shared #PnFlow whose
 *   representable nodes this editor mirrors.  The widget keeps its own
 *   reference; the caller still owns the flow.
 * @kind: which GUI surface to lay out for
 *
 * Creates a GUI layout editor bound to @flow — the visual counterpart to
 * the node-wiring #PnWorksheet, which lays out the widgets a flow drives
 * rather than the dataflow itself.
 *
 * The editor keeps one live widget per representable node across every
 * sheet of @flow (a seven-segment readout for each #PnCountdown, an
 * indicator lamp for each #PnLed, and so on): it is populated for all
 * such nodes on creation, and grows and shrinks automatically as they are
 * added to or removed from the flow.  Each widget mirrors its node's
 * current value live.  They are laid out on a free-positioning canvas and
 * can be dragged around with the mouse; every move is saved into the
 * document, keyed by node UUID, in the layout map belonging to @kind — so
 * the same node can hold a different position on the panel and in the
 * desktop window.
 *
 * Returns: (transfer floating): the new layout editor widget
 */
GtkWidget *pn_layout_editor_new (PnFlow             *flow,
                                 PnLayoutEditorKind  kind);

G_END_DECLS

#endif /* PN_LAYOUT_EDITOR_H */
