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

#ifndef PN_MESH_PAGE_GPS_H
#define PN_MESH_PAGE_GPS_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* GPS Settings page of the Meshtastic dialog (Radio tab, above Position).
 *
 * The hardware-facing half of the PositionConfig: GPS mode, the GPS
 * sampling interval, the RX/TX/enable GPIO pins, the position-report
 * flags, plus the device-specific "does this board have GPS, and which
 * pins drive it" note.  A single "Apply GPS settings" button ships the
 * whole PositionConfig at once.  The position-broadcast half of the same
 * config (broadcast interval, smart broadcast, fixed position) lives on
 * the sibling Position page; this page round-trips those fields verbatim
 * from PnMeshState so an Apply here does not reset them. */
GtkWidget *pn_mesh_page_gps_new (void);

void       pn_mesh_page_gps_set_state (GtkWidget         *page,
                                       const PnMeshState *state,
                                       PnMeshConnection  *connection);

typedef void (*PnMeshGpsStatusFunc) (const gchar *status,
                                     gpointer     user_data);

void       pn_mesh_page_gps_set_status_callback (
        GtkWidget             *page,
        PnMeshGpsStatusFunc    callback,
        gpointer               user_data);

void       pn_mesh_page_gps_set_busy_callback (
        GtkWidget             *page,
        PnMeshPageBusyFunc     callback,
        gpointer               user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_GPS_H */
