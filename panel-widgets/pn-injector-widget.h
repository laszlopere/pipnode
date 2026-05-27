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

#ifndef PN_INJECTOR_WIDGET_H
#define PN_INJECTOR_WIDGET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A panel-sized fire-button.
 *
 * Restates the worksheet's PnInject fire-tab: a grey rounded rectangle
 * carrying either a themed icon (when one is configured) or a cairo-drawn
 * play triangle, sitting above a soft drop shadow.  A primary press sinks
 * the tab down-right onto the shadow for tactile feedback; the release
 * (inside the widget) emits #PnInjectorWidget::clicked so the applet can
 * fire the underlying inject node.
 *
 * Like #PnSwitchWidget it can be made interactive: an interactive button
 * owns its own input window and reports clicks; a non-interactive one is
 * windowless and passive, so the panel-layout editor's drag handle keeps
 * receiving clicks unchanged.
 *
 * GTK / cairo only — no pipnode library — so the panel applet can embed
 * it without dragging the node runtime into xfce4-panel. */

#define PN_TYPE_INJECTOR_WIDGET (pn_injector_widget_get_type ())
G_DECLARE_FINAL_TYPE (PnInjectorWidget, pn_injector_widget,
                      PN, INJECTOR_WIDGET, GtkDrawingArea)

GtkWidget *pn_injector_widget_new (void);

/* Set the freedesktop icon name to draw on the button.  NULL or "" falls
 * back to the cairo-drawn play triangle.  Repaints live on change. */
void       pn_injector_widget_set_icon (PnInjectorWidget *self,
                                        const gchar      *icon_name);

/* Set the overall pixel height the button should occupy (typically the
 * panel's widget size); the tab width is derived from it (wider when an
 * icon is configured, narrow play-triangle tab otherwise). */
void       pn_injector_widget_set_height (PnInjectorWidget *self,
                                          gint              height);

/* Make the button clickable (TRUE) or passive (FALSE, the default).  When
 * interactive it grabs its own input window, shows tactile press feedback
 * and emits ::clicked on a primary release inside it; otherwise clicks
 * fall through to the parent.  Must be called before the widget is
 * realized (the applet does so right after construction). */
void       pn_injector_widget_set_interactive (PnInjectorWidget *self,
                                               gboolean          interactive);

G_END_DECLS

#endif /* PN_INJECTOR_WIDGET_H */
