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

#ifndef PN_MESH_PAGE_IDENTITY_H
#define PN_MESH_PAGE_IDENTITY_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Identity page of the Meshtastic dialog.
 *
 * Read-only display of what the want_config_id + get_device_metadata
 * exchange captured (firmware version, HW model with friendly name,
 * device role, capability flags, mesh node #, channels) plus two
 * editable fields with their own Apply buttons: the owner long name
 * (<= 39 chars) and short name (<= 4 chars).  Pressing Apply hands
 * off to the connection's set_owner_async, then re-paints the page
 * from the verified post-write state. */
GtkWidget *pn_mesh_page_identity_new (void);

/* Push a fresh device state into the page.  Pass NULL to blank every
 * field (e.g. when the user picks a different device and we are
 * reconnecting).  The optional @connection is bound to the page so
 * Apply has somewhere to send writes; pass NULL to leave Apply
 * disabled (the empty / "not connected" path). */
void       pn_mesh_page_identity_set_state (GtkWidget         *page,
                                            const gchar       *device_kind,
                                            const gchar       *tty_path,
                                            const PnMeshState *state,
                                            PnMeshConnection  *connection);

/* Callback the page invokes when the user successfully writes owner
 * names: the dialog's status bar updates and the device list row
 * subtitle could refresh.  Pass NULL to clear. */
typedef void (*PnMeshIdentityStatusFunc) (const gchar *status,
                                          gpointer     user_data);

void       pn_mesh_page_identity_set_status_callback (
        GtkWidget                *page,
        PnMeshIdentityStatusFunc  callback,
        gpointer                  user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_IDENTITY_H */
