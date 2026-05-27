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

#ifndef PN_DEVICES_MENU_H
#define PN_DEVICES_MENU_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The editor's "Devices" menu.
 *
 * Hosts one entry per device kind the editor knows how to set up
 * (Meshtastic today; Tasmota / Zigbee / Kodi expected to join when the
 * Phase 8 plugin-registered variant of TODO #29 lands).  Each entry
 * opens a per-device configuration dialog: a transient window with a
 * left-hand device list (populated only on explicit Scan) and a
 * right-hand stage-based settings stack.  Nothing the user does in
 * those dialogs is persisted on pipnode's side -- the device is the
 * source of truth and closing the dialog drops the session.
 *
 * Returned widget is a #GtkMenu suitable for use as the submenu of a
 * top-level menubar item; the caller wraps it in the "_Devices" menu
 * item and appends to the menubar. */
GtkWidget *pn_devices_menu_new (GtkWindow     *parent_window,
                                GtkAccelGroup *accel_group);

G_END_DECLS

#endif /* PN_DEVICES_MENU_H */
