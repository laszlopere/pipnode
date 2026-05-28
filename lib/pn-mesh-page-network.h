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

#ifndef PN_MESH_PAGE_NETWORK_H
#define PN_MESH_PAGE_NETWORK_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* NetworkConfig page (Phase 14).  WiFi switch + SSID/PSK entries (PSK
 * masked), Ethernet switch, IPv6 switch.  IPv4 static-IP sub-fields
 * (ip / gateway / subnet / dns) live in PnMeshState->net_ipv4_config
 * as opaque bytes — the page round-trips them verbatim so an Apply
 * does not clear them when the user only edits WiFi credentials. */
GtkWidget *pn_mesh_page_network_new (void);

void       pn_mesh_page_network_set_state (GtkWidget         *page,
                                           const PnMeshState *state,
                                           PnMeshConnection  *connection);

typedef void (*PnMeshNetworkStatusFunc) (const gchar *status,
                                         gpointer     user_data);

void       pn_mesh_page_network_set_status_callback (
        GtkWidget                *page,
        PnMeshNetworkStatusFunc   callback,
        gpointer                  user_data);

void       pn_mesh_page_network_set_busy_callback (
        GtkWidget                *page,
        PnMeshPageBusyFunc        callback,
        gpointer                  user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_NETWORK_H */
