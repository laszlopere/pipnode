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
 * Phase 2e: read-only display of what the want_config_id handshake
 * captured -- mesh node number, owner long/short name, hardware
 * model (raw enum integer), and channel count.  Firmware version
 * waits for Phase 3 when the admin protocol lands; owner names
 * become editable in Phase 3 too.
 *
 * Built as a plain GtkGrid so the dialog can drop it straight into
 * its right-hand stack.  Holds its own labels through qdata so the
 * dialog can call set_state() any time without re-walking the
 * widget tree. */
GtkWidget *pn_mesh_page_identity_new (void);

/* Push a fresh device state into the page.  Pass NULL to blank
 * every field (e.g. when the user picks a different device and we
 * are reconnecting).  Borrowed pointer; the page copies what it
 * needs. */
void       pn_mesh_page_identity_set_state (GtkWidget         *page,
                                            const gchar       *device_kind,
                                            const gchar       *tty_path,
                                            const PnMeshState *state);

G_END_DECLS

#endif /* PN_MESH_PAGE_IDENTITY_H */
