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

#ifndef PN_DEVICE_DIALOG_H
#define PN_DEVICE_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The reusable frame of a device-configuration dialog: a modeless
 * #GtkDialog whose body is a #GtkNotebook (with a ref-counted busy
 * spinner floated over it) above a status bar, plus a Close button.
 * The plugin / host POPULATES it -- appends notebook tabs, drives the
 * busy overlay around device round-trips, and writes one-line progress
 * to the status bar -- rather than subclassing anything.
 *
 * It is the "dialog shell" half of the device-dialog toolkit; the form
 * helpers in pn-device-form.h fill the tabs.  Extracted from the
 * Meshtastic dialog (pn-mesh-dialog.c), which is refactored onto it.
 *
 * #PnDeviceDialog is an opaque boxed-lifetime handle owned by the
 * underlying #GtkDialog (freed when the dialog is destroyed); never
 * free it yourself.  A left-hand sidebar (a device list) is supplied
 * by the caller for now via pn_device_dialog_set_sidebar(); a built-in
 * device-list pane lands in a later toolkit phase. */
typedef struct _PnDeviceDialog PnDeviceDialog;

/* Create the shell.  @parent may be NULL (the dialog is parentless).
 * @title is the window title.  The dialog is modeless and carries a
 * Close button that destroys it.  The shell is owned by the dialog;
 * fetch the widget with pn_device_dialog_get_dialog(). */
PnDeviceDialog *pn_device_dialog_new (GtkWindow   *parent,
                                      const gchar *title);

/* The underlying #GtkDialog widget -- present it, size it, or connect
 * to its signals. */
GtkWidget *pn_device_dialog_get_dialog (PnDeviceDialog *self);

/* The #GtkNotebook the tabs live in, for callers that need direct
 * access (e.g. to connect ::switch-page). */
GtkNotebook *pn_device_dialog_get_notebook (PnDeviceDialog *self);

/* Pack @sidebar as the left pane of an #GtkPaned, the notebook on the
 * right, with the divider at @position px.  Pass NULL for @sidebar to
 * drop the sidebar (the notebook then fills the body).  Intended to be
 * called once, right after construction. */
void pn_device_dialog_set_sidebar (PnDeviceDialog *self,
                                   GtkWidget      *sidebar,
                                   gint            position);

/* Append a notebook tab showing @child, labelled @tab_label. */
void pn_device_dialog_append_page (PnDeviceDialog *self,
                                   GtkWidget      *child,
                                   const gchar    *tab_label);

/* Get / set the visible tab by index. */
gint pn_device_dialog_get_current_page (PnDeviceDialog *self);
void pn_device_dialog_set_current_page (PnDeviceDialog *self, gint index);

/* Enable or disable the whole notebook (tabs + content).  Disabled,
 * the user can see which tabs exist but cannot navigate into them --
 * used while no device is selected. */
void pn_device_dialog_set_pages_sensitive (PnDeviceDialog *self,
                                           gboolean        sensitive);

/* Ref-counted busy overlay.  push_busy() floats the big spinner over
 * the notebook and disables it on the 0->1 edge; pop_busy() hides the
 * spinner and re-enables the notebook on the 1->0 edge.  Nesting keeps
 * the spinner up until the last waiter settles, so a page write
 * completing during a device handshake does not hide it early.
 * pop_busy() clamps at zero (a stray pop is a no-op). */
void pn_device_dialog_push_busy (PnDeviceDialog *self);
void pn_device_dialog_pop_busy  (PnDeviceDialog *self);

/* TRUE while a busy round-trip is in flight (push count > 0) -- e.g.
 * for a monitor timer that must not touch the device's fd while a
 * worker thread owns it. */
gboolean pn_device_dialog_is_busy (PnDeviceDialog *self);

/* Status-bar text.  set_statusf() is the printf variant. */
void pn_device_dialog_set_status  (PnDeviceDialog *self,
                                   const gchar    *text);
void pn_device_dialog_set_statusf (PnDeviceDialog *self,
                                   const gchar    *fmt,
                                   ...) G_GNUC_PRINTF (2, 3);

G_END_DECLS

#endif /* PN_DEVICE_DIALOG_H */
