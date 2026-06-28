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

#ifndef PN_MESH_PAGE_AMBIENT_LIGHTING_H
#define PN_MESH_PAGE_AMBIENT_LIGHTING_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Ambient Lighting module page (Phase 11, TODO #48.8).
 *
 * Drives an addressable RGB status LED (NeoPixel / WS2812).  The
 * master "LED on" switch gates the per-channel drive current and the
 * red / green / blue colour components (0-255 each).
 *
 * Lives under the "Notifications" top tab of the dialog as one
 * expander alongside External Notification. */
GtkWidget *pn_mesh_page_ambient_lighting_new (void);

void       pn_mesh_page_ambient_lighting_set_state (
        GtkWidget         *page,
        const PnMeshState *state,
        PnMeshConnection  *connection);

typedef void (*PnMeshAmbientLightingStatusFunc) (const gchar *status,
                                                 gpointer     user_data);

void       pn_mesh_page_ambient_lighting_set_status_callback (
        GtkWidget                       *page,
        PnMeshAmbientLightingStatusFunc  callback,
        gpointer                         user_data);

/* See PnMeshPageBusyFunc in pn-mesh-page-busy.h. */
void       pn_mesh_page_ambient_lighting_set_busy_callback (
        GtkWidget                       *page,
        PnMeshPageBusyFunc               callback,
        gpointer                         user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_AMBIENT_LIGHTING_H */
