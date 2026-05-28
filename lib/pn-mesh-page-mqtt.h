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

#ifndef PN_MESH_PAGE_MQTT_H
#define PN_MESH_PAGE_MQTT_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* MQTT module page (Phase 10).
 *
 * Lives under the "Network" top tab as one expander among others
 * (Serial / Bluetooth / WiFi / Ethernet land in later phases).
 *
 * Surface: master Enabled switch, server address, username/password,
 * encryption + TLS toggles, root topic, "proxy to client" toggle,
 * and the map-reporting master switch.  The MapReportSettings
 * sub-message is captured opaquely during the handshake and shipped
 * back verbatim on Apply -- the sub-fields are not exposed here
 * (lat/lon precision, interval, etc.). */
GtkWidget *pn_mesh_page_mqtt_new (void);

void       pn_mesh_page_mqtt_set_state (GtkWidget         *page,
                                        const PnMeshState *state,
                                        PnMeshConnection  *connection);

typedef void (*PnMeshMqttStatusFunc) (const gchar *status,
                                      gpointer     user_data);

void       pn_mesh_page_mqtt_set_status_callback (
        GtkWidget            *page,
        PnMeshMqttStatusFunc  callback,
        gpointer              user_data);

/* See PnMeshPageBusyFunc in pn-mesh-page-busy.h. */
void       pn_mesh_page_mqtt_set_busy_callback (
        GtkWidget            *page,
        PnMeshPageBusyFunc    callback,
        gpointer              user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_MQTT_H */
