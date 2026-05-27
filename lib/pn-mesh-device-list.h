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

#ifndef PN_MESH_DEVICE_LIST_H
#define PN_MESH_DEVICE_LIST_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Left pane of the Meshtastic dialog: a Scan button at the top and a
 * GtkListBox underneath holding one row per discovered device.  The
 * list is empty on construction and on every "Scan" press is replaced
 * with the current scan result -- there is no append/refresh
 * semantic, because a device may have been unplugged while the
 * previous list was visible and pipnode is not allowed to remember
 * what it saw last time.
 *
 * Phase 1 ships only the layout and a STUBBED scan (one fake "Heltec
 * V3 -- /dev/ttyUSB0" row) so the dialog look & feel can be reviewed
 * before pn-mesh-discover.c lands in Phase 2.  The widget is built
 * around a real GtkListBox already so Phase 2 only has to replace the
 * scan worker, not the UI. */
GtkWidget *pn_mesh_device_list_new (void);

G_END_DECLS

#endif /* PN_MESH_DEVICE_LIST_H */
