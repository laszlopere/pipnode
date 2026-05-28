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

#ifndef PN_MESH_PAGE_SECURITY_H
#define PN_MESH_PAGE_SECURITY_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* SecurityConfig page (Phase 14).  Read-only base64 view of the X25519
 * public / private / admin keys; switches for is_managed, serial-API,
 * debug-log-API and admin-channel.  A bad write here can lock the user
 * out of admin, so Apply is gated behind an explicit "I understand the
 * risks" check.  Key bytes are round-tripped verbatim — typing 32
 * random bytes by hand is not a useful UI in this phase. */
GtkWidget *pn_mesh_page_security_new (void);

void       pn_mesh_page_security_set_state (GtkWidget         *page,
                                            const PnMeshState *state,
                                            PnMeshConnection  *connection);

typedef void (*PnMeshSecurityStatusFunc) (const gchar *status,
                                          gpointer     user_data);

void       pn_mesh_page_security_set_status_callback (
        GtkWidget                 *page,
        PnMeshSecurityStatusFunc   callback,
        gpointer                   user_data);

void       pn_mesh_page_security_set_busy_callback (
        GtkWidget                 *page,
        PnMeshPageBusyFunc         callback,
        gpointer                   user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_SECURITY_H */
