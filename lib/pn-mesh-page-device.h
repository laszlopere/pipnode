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

#ifndef PN_MESH_PAGE_DEVICE_H
#define PN_MESH_PAGE_DEVICE_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* DeviceConfig page of the Meshtastic dialog (Phase 14).
 *
 * Combos for role + rebroadcast mode, spinners for the button / buzzer
 * GPIO and the NodeInfo broadcast interval, single Apply button.
 * Fields the firmware sent but the UI does not surface (serial_enabled,
 * double_tap_as_button_press, is_managed (deprecated), tzdef,
 * led_heartbeat_disabled, buzzer_mode) are round-tripped from
 * PnMeshState so an Apply does not silently reset them. */
GtkWidget *pn_mesh_page_device_new (void);

void       pn_mesh_page_device_set_state (GtkWidget         *page,
                                          const PnMeshState *state,
                                          PnMeshConnection  *connection);

typedef void (*PnMeshDeviceStatusFunc) (const gchar *status,
                                        gpointer     user_data);

void       pn_mesh_page_device_set_status_callback (
        GtkWidget               *page,
        PnMeshDeviceStatusFunc   callback,
        gpointer                 user_data);

void       pn_mesh_page_device_set_busy_callback (
        GtkWidget               *page,
        PnMeshPageBusyFunc       callback,
        gpointer                 user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_DEVICE_H */
