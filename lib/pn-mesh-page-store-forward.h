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

#ifndef PN_MESH_PAGE_STORE_FORWARD_H
#define PN_MESH_PAGE_STORE_FORWARD_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Store & Forward module page (Phase 11, TODO #48.2).
 *
 * Lets a mains-powered node act as a message relay/replay server: it
 * buffers mesh traffic and replays it on request so nodes that were
 * asleep or briefly out of range can catch up.  The master "Enabled"
 * switch gates the module; "Is server" promotes this node from a plain
 * client to the storing server and unlocks the server-side knobs
 * (heartbeat, records, history return max / window).
 *
 * Lives under the "Mesh tools" top tab of the dialog as one expander. */
GtkWidget *pn_mesh_page_store_forward_new (void);

void       pn_mesh_page_store_forward_set_state (
        GtkWidget         *page,
        const PnMeshState *state,
        PnMeshConnection  *connection);

typedef void (*PnMeshStoreForwardStatusFunc) (const gchar *status,
                                              gpointer     user_data);

void       pn_mesh_page_store_forward_set_status_callback (
        GtkWidget                     *page,
        PnMeshStoreForwardStatusFunc   callback,
        gpointer                       user_data);

/* See PnMeshPageBusyFunc in pn-mesh-page-busy.h. */
void       pn_mesh_page_store_forward_set_busy_callback (
        GtkWidget                     *page,
        PnMeshPageBusyFunc             callback,
        gpointer                       user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_STORE_FORWARD_H */
