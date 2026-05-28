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

#ifndef PN_MESH_PAGE_POSITION_H
#define PN_MESH_PAGE_POSITION_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Position page of the Meshtastic dialog (Phase 12).
 *
 * GPS mode + smart-broadcast switches + the interval/distance
 * thresholds, with a single "Apply position settings" button that
 * ships the whole PositionConfig at once.  Fields the firmware sent
 * but the UI doesn't expose (rx_gpio, tx_gpio, gps_en_gpio, etc.) are
 * round-tripped verbatim from PnMeshState so an Apply does not silently
 * reset GPIO assignments to zero. */
GtkWidget *pn_mesh_page_position_new (void);

void       pn_mesh_page_position_set_state (GtkWidget         *page,
                                            const PnMeshState *state,
                                            PnMeshConnection  *connection);

typedef void (*PnMeshPositionStatusFunc) (const gchar *status,
                                          gpointer     user_data);

void       pn_mesh_page_position_set_status_callback (
        GtkWidget                 *page,
        PnMeshPositionStatusFunc   callback,
        gpointer                   user_data);

void       pn_mesh_page_position_set_busy_callback (
        GtkWidget                 *page,
        PnMeshPageBusyFunc         callback,
        gpointer                   user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_POSITION_H */
