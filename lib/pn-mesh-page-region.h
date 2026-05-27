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

#ifndef PN_MESH_PAGE_REGION_H
#define PN_MESH_PAGE_REGION_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Region / LoRa page of the Meshtastic dialog.
 *
 * Region (regulatory domain), modem preset, hop limit, TX power and
 * TX enabled, with a single "Apply LoRa settings" button that ships
 * the whole LoRaConfig at once.  The form initialises from the
 * device's current state; pressing Apply writes everything back
 * (including channel_num, kept verbatim from the read), then the
 * verifying handshake re-reads and refreshes the controls. */
GtkWidget *pn_mesh_page_region_new (void);

void       pn_mesh_page_region_set_state (GtkWidget         *page,
                                          const PnMeshState *state,
                                          PnMeshConnection  *connection);

/* Same status-bar bridge contract as the Identity page; see
 * pn-mesh-page-identity.h for the rationale (the callback's two
 * args are in (text, user_data) order, so callers reach the dialog's
 * set_status via a small adapter, never via a function cast). */
typedef void (*PnMeshRegionStatusFunc) (const gchar *status,
                                        gpointer     user_data);

void       pn_mesh_page_region_set_status_callback (
        GtkWidget              *page,
        PnMeshRegionStatusFunc  callback,
        gpointer                user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_REGION_H */
