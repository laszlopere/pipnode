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

#ifndef PN_MESH_PAGE_CHANNELS_H
#define PN_MESH_PAGE_CHANNELS_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Channels page of the Meshtastic dialog.
 *
 * Lists every channel slot the device exposes (typically 8), one row
 * per slot showing index / name / role badge / PSK indicator.  "Add
 * channel" picks the first DISABLED slot with index > 0 and opens a
 * small dialog for name + PSK (with a "Generate" button that pulls 32
 * bytes from /dev/urandom for an AES-256 key).  Each row carries its
 * own Delete button, disabled for the PRIMARY slot (you cannot
 * delete primary -- removing it would orphan the device from every
 * mesh).  All writes are bare set_channel admin messages (no flash
 * commit, no reboot); the verify-cycle handshake refreshes the page
 * from the device's authoritative reply after each write. */
GtkWidget *pn_mesh_page_channels_new (void);

void       pn_mesh_page_channels_set_state (GtkWidget         *page,
                                            const PnMeshState *state,
                                            PnMeshConnection  *connection);

/* Same status-bar bridge contract as the Identity / Region pages. */
typedef void (*PnMeshChannelsStatusFunc) (const gchar *status,
                                          gpointer     user_data);

void       pn_mesh_page_channels_set_status_callback (
        GtkWidget                *page,
        PnMeshChannelsStatusFunc  callback,
        gpointer                  user_data);

/* See PnMeshPageBusyFunc in pn-mesh-page-busy.h. */
void       pn_mesh_page_channels_set_busy_callback (
        GtkWidget                *page,
        PnMeshPageBusyFunc        callback,
        gpointer                  user_data);

/* Fired after a channel write (add / edit / delete) has been verified
 * and the page has re-read the connection's channel snapshot.  The
 * dialog uses this to re-push the now-updated state to sibling pages
 * that mirror the channel list (notably Share), which would otherwise
 * keep showing the channels as they were when the device first
 * connected. */
typedef void (*PnMeshChannelsChangedFunc) (gpointer user_data);

void       pn_mesh_page_channels_set_changed_callback (
        GtkWidget                 *page,
        PnMeshChannelsChangedFunc  callback,
        gpointer                   user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_CHANNELS_H */
