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

#ifndef PN_PANEL_DISPLAY_H
#define PN_PANEL_DISPLAY_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPanelDisplay                                                     */
/*                                                                     */
/*  Sink node that surfaces a worksheet value on an XFCE panel applet. */
/*  It is one half of the panel I/O contract: a worksheet "runs" in    */
/*  the background engine (pipnode-editor --gapplication-service) and  */
/*  the panel button reflects whatever this node last received.        */
/*                                                                     */
/*  On each incoming message the node derives a short display string   */
/*  (the "text" string member if present, else the "value" member      */
/*  formatted, else the topic), stores it, and emits the              */
/*  "value-changed" signal.  The engine connects to that signal and    */
/*  forwards the string to the applet over D-Bus; the applet shows it  */
/*  as the button label.  Because a sink emits no message of its own,  */
/*  this dedicated signal — not the node "message" signal — is the     */
/*  observation seam.                                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PANEL_DISPLAY (pn_panel_display_get_type ())

G_DECLARE_FINAL_TYPE (PnPanelDisplay, pn_panel_display, PN, PANEL_DISPLAY, PnNode)

PnPanelDisplay *pn_panel_display_new (void);

/**
 * pn_panel_display_dup_text:
 * @self: the panel display node
 *
 * Returns a copy of the most recently displayed string (never %NULL;
 * the empty string before any message arrives).  Caller frees with
 * g_free().  This is the read seam the engine uses to answer a
 * GetDisplayValue D-Bus call.
 */
gchar *pn_panel_display_dup_text (PnPanelDisplay *self);

G_END_DECLS

#endif /* PN_PANEL_DISPLAY_H */
