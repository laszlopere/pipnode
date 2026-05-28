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

#ifndef PN_MESH_PAGE_POWER_H
#define PN_MESH_PAGE_POWER_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Power page of the Meshtastic dialog (Phase 12).
 *
 * Power-saving switch, the four sleep/wake interval spinners, the
 * on-battery shutdown threshold and the ADC multiplier override,
 * with a single "Apply power settings" button that ships the whole
 * PowerConfig at once.  device_battery_ina_address is round-tripped
 * verbatim. */
GtkWidget *pn_mesh_page_power_new (void);

void       pn_mesh_page_power_set_state (GtkWidget         *page,
                                         const PnMeshState *state,
                                         PnMeshConnection  *connection);

typedef void (*PnMeshPowerStatusFunc) (const gchar *status,
                                       gpointer     user_data);

void       pn_mesh_page_power_set_status_callback (
        GtkWidget              *page,
        PnMeshPowerStatusFunc   callback,
        gpointer                user_data);

void       pn_mesh_page_power_set_busy_callback (
        GtkWidget              *page,
        PnMeshPageBusyFunc      callback,
        gpointer                user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_POWER_H */
