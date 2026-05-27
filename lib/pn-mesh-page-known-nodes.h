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

#ifndef PN_MESH_PAGE_KNOWN_NODES_H
#define PN_MESH_PAGE_KNOWN_NODES_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Known Nodes page of the Meshtastic dialog.
 *
 * Pure read-out: a sortable list of every entry the device has in
 * its NodeInfo database -- the device itself plus every peer it has
 * heard.  The row whose num matches MyNodeInfo.my_node_num is marked
 * with a "★" and bolded so the user can spot it in a busy mesh.
 *
 * This page is the "everything works" diagnostic: a freshly-flashed
 * device that has never heard a peer still shows its own row, which
 * is enough to prove the handshake round-trips end-to-end. */
GtkWidget *pn_mesh_page_known_nodes_new (void);

void       pn_mesh_page_known_nodes_set_state (GtkWidget         *page,
                                               const PnMeshState *state);

G_END_DECLS

#endif /* PN_MESH_PAGE_KNOWN_NODES_H */
