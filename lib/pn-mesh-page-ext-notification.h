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

#ifndef PN_MESH_PAGE_EXT_NOTIFICATION_H
#define PN_MESH_PAGE_EXT_NOTIFICATION_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* External Notification module page (Phase 9).
 *
 * Drives the device's onboard LED / buzzer / vibration motor on
 * incoming traffic.  The headline knobs are "Enabled" and "Nag
 * timeout"; Nag=0 turns the "keeps beeping" behaviour off.
 *
 * Lives under the "Notifications" top tab of the dialog as one
 * expander among many; Phases 10/11 add Canned Message, Status
 * Message, Audio, Ambient Lighting beside it. */
GtkWidget *pn_mesh_page_ext_notification_new (void);

void       pn_mesh_page_ext_notification_set_state (
        GtkWidget         *page,
        const PnMeshState *state,
        PnMeshConnection  *connection);

typedef void (*PnMeshExtNotificationStatusFunc) (const gchar *status,
                                                 gpointer     user_data);

void       pn_mesh_page_ext_notification_set_status_callback (
        GtkWidget                       *page,
        PnMeshExtNotificationStatusFunc  callback,
        gpointer                         user_data);

/* See PnMeshPageBusyFunc in pn-mesh-page-busy.h. */
void       pn_mesh_page_ext_notification_set_busy_callback (
        GtkWidget                       *page,
        PnMeshPageBusyFunc               callback,
        gpointer                         user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_EXT_NOTIFICATION_H */
