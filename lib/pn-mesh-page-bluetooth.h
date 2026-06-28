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

#ifndef PN_MESH_PAGE_BLUETOOTH_H
#define PN_MESH_PAGE_BLUETOOTH_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Bluetooth page of the Meshtastic dialog (Phase 13).
 *
 * Enabled switch, pairing-mode combo (RANDOM_PIN / FIXED_PIN / NO_PIN)
 * and the fixed PIN spinner (only relevant in FIXED_PIN mode), with a
 * single "Apply Bluetooth settings" button that ships the whole
 * BluetoothConfig at once. */
GtkWidget *pn_mesh_page_bluetooth_new (void);

void       pn_mesh_page_bluetooth_set_state (GtkWidget         *page,
                                             const PnMeshState *state,
                                             PnMeshConnection  *connection);

typedef void (*PnMeshBluetoothStatusFunc) (const gchar *status,
                                           gpointer     user_data);

void       pn_mesh_page_bluetooth_set_status_callback (
        GtkWidget                  *page,
        PnMeshBluetoothStatusFunc   callback,
        gpointer                    user_data);

void       pn_mesh_page_bluetooth_set_busy_callback (
        GtkWidget              *page,
        PnMeshPageBusyFunc      callback,
        gpointer                user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_BLUETOOTH_H */
