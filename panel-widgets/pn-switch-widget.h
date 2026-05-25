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

#ifndef PN_SWITCH_WIDGET_H
#define PN_SWITCH_WIDGET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized slide toggle.
 *
 * A self-contained restatement of the Switch node's header decoration: a
 * rounded "track" with a circular "thumb", green and parked right when on,
 * dark grey and parked left when off.  Unlike the other panel widgets
 * (PnLedDisplay, PnLedLamp) it can be made *interactive*: when interactive
 * it owns its own input window and emits #PnSwitchWidget::toggled on a
 * primary click, so the panel applet can flip the underlying Switch node.
 * When not interactive (the default) it is a passive, windowless drawing
 * area exactly like the other widgets — clicks fall through to whatever
 * parent wants them (e.g. the panel editor's drag handle).
 *
 * It deliberately depends on GTK/cairo only — no pipnode library — so it
 * can live inside the panel applet without putting node code in
 * xfce4-panel's address space. */

#define PN_TYPE_SWITCH_WIDGET (pn_switch_widget_get_type ())
G_DECLARE_FINAL_TYPE (PnSwitchWidget, pn_switch_widget, PN, SWITCH_WIDGET,
                      GtkDrawingArea)

GtkWidget *pn_switch_widget_new (void);

/* Set the latch position drawn: TRUE shows the green, thumb-right "on"
 * pill, FALSE the grey, thumb-left "off" pill.  Repaints live on change. */
void       pn_switch_widget_set_on (PnSwitchWidget *self, gboolean on);

/* Set the overall pixel height the toggle should occupy (typically the
 * panel's widget size); the pill width is derived from it. */
void       pn_switch_widget_set_height (PnSwitchWidget *self, gint height);

/* Make the toggle clickable (TRUE) or passive (FALSE, the default).  When
 * interactive it grabs its own input window and emits ::toggled on a
 * primary click; otherwise it is windowless and clicks pass through.  Must
 * be called before the widget is realized (the applet does so right after
 * construction). */
void       pn_switch_widget_set_interactive (PnSwitchWidget *self,
                                             gboolean        interactive);

G_END_DECLS

#endif /* PN_SWITCH_WIDGET_H */
