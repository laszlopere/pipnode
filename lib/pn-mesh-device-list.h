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

#include "pn-mesh-discover.h"

G_BEGIN_DECLS

/* Left pane of the Meshtastic dialog: a Scan button at the top and a
 * GtkListBox underneath holding one row per discovered device.  The
 * list is empty on construction and on every "Scan" press is replaced
 * with the current scan result -- there is no append/refresh
 * semantic, because a device may have been unplugged while the
 * previous list was visible and pipnode is not allowed to remember
 * what it saw last time. */
GtkWidget *pn_mesh_device_list_new (void);

/* Callback fired when the user activates a row (double-click or
 * Enter on a selected row).  The PnMeshDevice* is borrowed from
 * the row's qdata -- copy via pn_mesh_device_copy() if you need to
 * keep it past the activation handler.  Pass @callback as NULL to
 * clear the handler. */
typedef void (*PnMeshDeviceActivatedFunc) (const PnMeshDevice *device,
                                           gpointer            user_data);

void       pn_mesh_device_list_set_activated_callback (
        GtkWidget                  *list_widget,
        PnMeshDeviceActivatedFunc   callback,
        gpointer                    user_data);

G_END_DECLS

#endif /* PN_MESH_DEVICE_LIST_H */
