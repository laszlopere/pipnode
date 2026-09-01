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

#ifndef PN_WIDGET_MIRROR_H
#define PN_WIDGET_MIRROR_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWidgetMirror — a live row of panel widgets driven by layout JSON  */
/*                                                                     */
/*  The client half of the engine's layout protocol, shared by every    */
/*  surface that shows a worksheet's widgets without linking the node   */
/*  runtime: the XFCE panel applet and the desktop viewer today, the    */
/*  planned web and mobile viewers later.                              */
/*                                                                     */
/*  It owns the widget set: given the engine's layout JSON it creates   */
/*  the widgets that are new, destroys the ones that vanished,          */
/*  recreates any whose kind changed, and pushes each one's state into  */
/*  it — then hands the host an ordered list to PLACE.  Placement is    */
/*  deliberately left to the host, because that is the one thing the    */
/*  surfaces genuinely differ on: the panel packs its widgets into a    */
/*  strip, the desktop viewer puts each at its own (x, y) in a window.  */
/*                                                                     */
/*  Like the rest of panel-widgets it depends on GTK / GLib / Cairo and */
/*  json-glib only — never on a pipnode library — so a mirror can be     */
/*  embedded in a process that must survive an engine crash.           */
/* ------------------------------------------------------------------ */

typedef struct _PnWidgetMirror PnWidgetMirror;

/**
 * PnWidgetMirrorActivateFunc:
 * @uuid: the node the activated widget stands for
 * @user_data: as passed to pn_widget_mirror_new()
 *
 * Called when the user activates one of the interactive mirrored widgets
 * (a switch toggle, an injector fire button).  The host forwards this to
 * the engine (ActivateWidget); the widget never guesses its own new state,
 * it waits for the engine to echo one back.
 */
typedef void (*PnWidgetMirrorActivateFunc) (const gchar *uuid,
                                            gpointer     user_data);

/**
 * pn_widget_mirror_new:
 * @canvas: (transfer none): the #GtkFixed mirrored widgets are put on.
 *   The mirror does not own it and must not outlive it.
 * @activate: (nullable) (scope notified): called when an interactive
 *   widget is activated; %NULL makes every widget display-only
 * @user_data: passed to @activate
 *
 * Returns: (transfer full): a new mirror; free with pn_widget_mirror_free()
 */
PnWidgetMirror *pn_widget_mirror_new (GtkFixed                   *canvas,
                                      PnWidgetMirrorActivateFunc  activate,
                                      gpointer                    user_data);

/**
 * pn_widget_mirror_free:
 * @self: (transfer full): the mirror
 *
 * Destroys every mirrored widget and frees @self.
 */
void pn_widget_mirror_free (PnWidgetMirror *self);

/**
 * pn_widget_mirror_reconcile:
 * @self: the mirror
 * @layout_json: the engine's layout document:
 *   `{ "widgets": [ { "uuid", "x", "y", "state": { "kind", … } }, … ] }`
 *
 * Brings the live widget set into line with @layout_json and reorders the
 * mirror's list to match the array.  Widgets are left where they are; the
 * host places them afterwards from the ordered accessors below.
 *
 * Returns: %FALSE when @layout_json could not be parsed, in which case the
 *   live widgets are left untouched.
 */
gboolean pn_widget_mirror_reconcile (PnWidgetMirror *self,
                                     const gchar    *layout_json);

/**
 * pn_widget_mirror_update:
 * @self: the mirror
 * @uuid: the node whose state changed
 * @state_json: that node's render state, the same object the layout
 *   carries inline under "state"
 *
 * Pushes one node's fresh state into its widget.  A state for a node the
 * mirror does not (yet) hold is ignored — the next reconcile picks it up.
 */
void pn_widget_mirror_update (PnWidgetMirror *self,
                              const gchar    *uuid,
                              const gchar    *state_json);

/**
 * pn_widget_mirror_get_n_widgets:
 * @self: the mirror
 *
 * Returns: how many widgets the last reconcile left in place
 */
guint pn_widget_mirror_get_n_widgets (PnWidgetMirror *self);

/**
 * pn_widget_mirror_get_widget:
 * @self: the mirror
 * @index: position in layout order, below pn_widget_mirror_get_n_widgets()
 *
 * Returns: (transfer none): the mirrored widget, owned by the canvas
 */
GtkWidget *pn_widget_mirror_get_widget (PnWidgetMirror *self, guint index);

/**
 * pn_widget_mirror_get_position:
 * @self: the mirror
 * @index: position in layout order
 * @out_x: (out) (optional): the layout's x for this widget
 * @out_y: (out) (optional): the layout's y for this widget
 *
 * The placement the engine published, in whatever coordinates the surface
 * uses (band x for the panel, window-relative x/y for the desktop).  Both
 * default to 0 when the layout omits them.
 */
void pn_widget_mirror_get_position (PnWidgetMirror *self,
                                    guint           index,
                                    gdouble        *out_x,
                                    gdouble        *out_y);

/**
 * pn_widget_mirror_set_height:
 * @self: the mirror
 * @index: position in layout order
 * @height: the pixel height to draw at
 *
 * Sizes one widget the way its kind is sized.  Ignored for a plot area,
 * which carries its own width and height in its state: it is a rectangle
 * the layout editor sized, not a row on a strip.
 */
void pn_widget_mirror_set_height (PnWidgetMirror *self,
                                  guint           index,
                                  gint            height);

/**
 * pn_widget_mirror_fills_height:
 * @self: the mirror
 * @index: position in layout order
 *
 * Returns: %TRUE for the text-shaped kinds (Label, Matrix 5x7) whose font
 *   should fill the row it sits in rather than take a fixed readout size.
 *   The panel applet uses this to give them the whole panel height.
 */
gboolean pn_widget_mirror_fills_height (PnWidgetMirror *self, guint index);

G_END_DECLS

#endif /* PN_WIDGET_MIRROR_H */
