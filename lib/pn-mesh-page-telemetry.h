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

#ifndef PN_MESH_PAGE_TELEMETRY_H
#define PN_MESH_PAGE_TELEMETRY_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Telemetry module page (Phase 11).
 *
 * Lives under the "Telemetry" top tab as one expander.  Surfaces
 * five logical sub-systems -- device, environment, air quality,
 * power, health -- each with its own enable switch, broadcast
 * interval, and (where applicable) screen-display toggle.
 * Disabling a sub-system greys out its detail fields so the user
 * sees at a glance which intervals are actually in play. */
GtkWidget *pn_mesh_page_telemetry_new (void);

void       pn_mesh_page_telemetry_set_state (GtkWidget         *page,
                                             const PnMeshState *state,
                                             PnMeshConnection  *connection);

typedef void (*PnMeshTelemetryStatusFunc) (const gchar *status,
                                           gpointer     user_data);

void       pn_mesh_page_telemetry_set_status_callback (
        GtkWidget                 *page,
        PnMeshTelemetryStatusFunc  callback,
        gpointer                   user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_TELEMETRY_H */
