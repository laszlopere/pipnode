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
 * by the caller via pn_device_dialog_set_sidebar(), or built in by the
 * shell when PN_DEVICE_DIALOG_WITH_DEVICE_LIST is passed to _new() (see
 * the "Device-list pane" section below). */
typedef struct _PnDeviceDialog PnDeviceDialog;

/* Construction flags. */
typedef enum
{
    PN_DEVICE_DIALOG_NONE             = 0,
    /* Build a device-list pane (Scan-on-open + 3-state empty/list stack
     * + GtkListBox) into the left side, wired to the scan / selected
     * callbacks below.  Without this flag the dialog is notebook-only
     * unless the caller supplies its own pn_device_dialog_set_sidebar(). */
    PN_DEVICE_DIALOG_WITH_DEVICE_LIST = 1 << 0
} PnDeviceDialogFlags;

/* Create the shell.  @parent may be NULL (the dialog is parentless).
 * @title is the window title.  The dialog is modeless and carries a
 * Close button that destroys it.  The shell is owned by the dialog;
 * fetch the widget with pn_device_dialog_get_dialog(). */
PnDeviceDialog *pn_device_dialog_new (GtkWindow           *parent,
                                      const gchar         *title,
                                      PnDeviceDialogFlags  flags);

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

/* ------------------------------------------------------------------ */
/*  Device-list pane (PN_DEVICE_DIALOG_WITH_DEVICE_LIST)                */
/* ------------------------------------------------------------------ */

/* One row in the device list.  @id is the stable key the pane matches
 * on (a tty path, a MAC, …) and reports back through the selected
 * callback.  @primary is the bold first line, @secondary the dim
 * second line.  @disabled_reason, when non-NULL, renders the row
 * insensitive with the reason as a third line + tooltip -- a port held
 * open by another process, say -- so it cannot be selected, activated
 * or auto-selected. */
typedef struct
{
    gchar *id;
    gchar *primary;
    gchar *secondary;
    gchar *disabled_reason;
} PnDeviceRow;

PnDeviceRow *pn_device_row_new (const gchar *id,
                                const gchar *primary,
                                const gchar *secondary,
                                const gchar *disabled_reason);
PnDeviceRow *pn_device_row_copy (const PnDeviceRow *row);
void         pn_device_row_free (PnDeviceRow *row);

/* A GPtrArray whose element-free is pn_device_row_free -- fill it with
 * pn_device_row_new() results and hand it to set_device_rows(). */
GPtrArray   *pn_device_row_array_new (void);

/* The pane wants a (re)scan: fired on the Scan-on-open auto-scan and on
 * the right-click Reload.  Run discovery (typically async) and report
 * the result back with pn_device_dialog_set_device_rows(). */
typedef void (*PnDeviceScanFunc) (gpointer user_data);

/* A connectable row was activated (clicked / Enter) or auto-selected.
 * @row is borrowed -- copy it with pn_device_row_copy() to keep it. */
typedef void (*PnDeviceSelectedFunc) (const PnDeviceRow *row,
                                      gpointer           user_data);

void pn_device_dialog_set_scan_callback     (PnDeviceDialog       *self,
                                             PnDeviceScanFunc      callback,
                                             gpointer              user_data);
void pn_device_dialog_set_selected_callback (PnDeviceDialog       *self,
                                             PnDeviceSelectedFunc  callback,
                                             gpointer              user_data);

/* Replace the whole list with @rows (a PnDeviceRow GPtrArray; may be
 * NULL or empty).  The pane then applies its selection policy: it
 * re-selects the previously-selected id when still present and
 * connectable (so the right pane refreshes after a rescan), otherwise
 * auto-selects the first connectable row on the initial scan or when
 * something was selected before.  Selecting fires the selected
 * callback.  Shows the empty-result hint when @rows is empty. */
void pn_device_dialog_set_device_rows (PnDeviceDialog *self,
                                       GPtrArray      *rows);

/* Incremental list edits (match by id).  add appends; update replaces a
 * row's labels/sensitivity in place; remove drops it.  None of them
 * re-run the selection policy -- use for live presence updates between
 * scans. */
void pn_device_dialog_add_device_row    (PnDeviceDialog    *self,
                                         const PnDeviceRow *row);
void pn_device_dialog_update_device_row (PnDeviceDialog    *self,
                                         const PnDeviceRow *row);
void pn_device_dialog_remove_device_row (PnDeviceDialog    *self,
                                         const gchar       *id);

/* Text + themed-icon name for the two empty-state pages: the pre-scan
 * page (shown before the first result) and the empty-result page
 * (shown after a scan that found nothing).  Any string may be NULL to
 * keep the built-in default. */
void pn_device_dialog_set_list_hints (PnDeviceDialog *self,
                                      const gchar    *prescan_icon,
                                      const gchar    *prescan_primary,
                                      const gchar    *prescan_secondary,
                                      const gchar    *empty_icon,
                                      const gchar    *empty_primary,
                                      const gchar    *empty_secondary);

/* Scan automatically once when the dialog opens (deferred to an idle so
 * the caller has wired its callbacks first).  Enabling also arms the
 * one-shot auto-select of the first connectable row. */
void pn_device_dialog_set_auto_scan (PnDeviceDialog *self,
                                     gboolean        enabled);

/* Programmatically select the connectable row with @id (firing the
 * selected callback), if present.  A no-op for an unknown or disabled
 * id. */
void pn_device_dialog_reselect_device (PnDeviceDialog *self,
                                       const gchar    *id);

G_END_DECLS

#endif /* PN_DEVICE_DIALOG_H */
