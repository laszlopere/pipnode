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

#ifndef PN_PANEL_INPUT_H
#define PN_PANEL_INPUT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPanelInput                                                       */
/*                                                                     */
/*  Source node driven by an XFCE panel applet — the other half of the */
/*  panel I/O contract (see #PnPanelDisplay).  The applet forwards a   */
/*  click (or a value) to the background engine over D-Bus, which      */
/*  calls pn_panel_input_send() to make this node emit a message       */
/*  carrying the value on its "value" data member into the running     */
/*  worksheet.                                                          */
/*                                                                     */
/*  Like #PnKnob and #PnSwitch it announces its current value once     */
/*  shortly after the worksheet loads, so downstream nodes learn the   */
/*  starting value without waiting for the first click.                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PANEL_INPUT (pn_panel_input_get_type ())

G_DECLARE_FINAL_TYPE (PnPanelInput, pn_panel_input, PN, PANEL_INPUT, PnNode)

PnPanelInput *pn_panel_input_new (void);

/**
 * pn_panel_input_send:
 * @self:  the panel input node
 * @value: the numeric value to carry on the emitted message
 *
 * Records @value as the node's current value and emits a single
 * message carrying it on the "value" data member.  Must be called from
 * the main thread.  This is the entry point the engine's SetInput
 * D-Bus method uses to drive the input with a plain numeric value.
 */
void pn_panel_input_send (PnPanelInput *self, gdouble value);

/**
 * pn_panel_input_send_event:
 * @self:   the panel input node
 * @event:  the mouse event name, e.g. "click" (also "press"/"release")
 * @button: the mouse button, GDK numbering (1 left, 2 middle, 3 right)
 *
 * Emits a single message describing a mouse event on the panel applet.
 * The message topic is "<event>/<button-name>" (e.g. "press/left") and
 * its data bag carries the standard human-readable summary on "output"
 * (e.g. "Applet right mouse button clicked.", the member a Text to Speech
 * node reads aloud), the button number on "value", the button name on
 * "button" and the event name on "event", so a worksheet can react to a
 * specific button.  Does not touch the node's "value" property — that
 * stays the configured startup value.
 * Must be called from the main thread; the entry point for the engine's
 * SendEvent D-Bus method.
 */
void pn_panel_input_send_event (PnPanelInput *self,
                                const gchar  *event,
                                guint         button);

G_END_DECLS

#endif /* PN_PANEL_INPUT_H */
