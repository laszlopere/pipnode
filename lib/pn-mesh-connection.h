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

#ifndef PN_MESH_CONNECTION_H
#define PN_MESH_CONNECTION_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* One Meshtastic channel slot as parsed out of the device.
 *
 * @name may be NULL or "" for an unnamed primary; the device's UI
 * treats both as "Default".  @psk is owned by the struct and may be
 * NULL on a disabled slot.  @role: 0 = DISABLED, 1 = PRIMARY,
 * 2 = SECONDARY, matching the Meshtastic Channel.Role enum. */
typedef struct
{
    guint32  index;
    gchar   *name;
    guint32  role;
    guint8  *psk;
    gsize    psk_size;
} PnMeshChannel;

void          pn_mesh_channel_free (PnMeshChannel *channel);

/* Read-only snapshot of the device state captured by the handshake. */
typedef struct
{
    guint32     my_node_num;       /* MyNodeInfo.my_node_num                 */
    gchar      *owner_id;          /* User.id    (from the matching NodeInfo) */
    gchar      *owner_long_name;   /* User.long_name (<=39 chars by device)  */
    gchar      *owner_short_name;  /* User.short_name (<=4 chars by device)  */
    guint32     owner_hw_model;    /* User.hw_model enum                     */
    GPtrArray  *channels;          /* PnMeshChannel*, index-ordered          */
    gboolean    config_complete;   /* TRUE iff config_complete_id was seen   */
} PnMeshState;

/* A live session to one device: an open serial fd plus the state
 * captured during the want_config_id handshake.  Owns its fd until
 * close(); designed to be held by the dialog for as long as the user
 * is configuring that device.  Phase 2d does the handshake and
 * exposes the state read-only; later phases (write paths, test
 * message, live monitor) extend it without changing this contract. */
typedef struct _PnMeshConnection PnMeshConnection;

/* Synchronous: open @tty_path, drain stale bytes, send ToRadio{
 * want_config_id = 1 }, read FromRadio frames until config_complete_id
 * arrives or 3 seconds of silence elapse (pip-mesh's pattern: some
 * firmwares never emit config_complete_id, so the budget is the
 * fallback signal).  Designed to run inside a GTask worker thread. */
PnMeshConnection *pn_mesh_connection_open_sync (const gchar  *tty_path,
                                                GError      **error);

void              pn_mesh_connection_close (PnMeshConnection *self);

/* Borrowed snapshot of what the handshake captured.  Valid until
 * pn_mesh_connection_close().  Phase 2e renders this on the Identity
 * page; later phases mutate via dedicated write functions and
 * refresh the state in place. */
const PnMeshState *pn_mesh_connection_get_state (PnMeshConnection *self);

const gchar       *pn_mesh_connection_get_tty   (PnMeshConnection *self);

/* GTask wrapper: runs open_sync() on a worker thread, fires
 * @callback on the calling thread.  Pair with finish() to collect
 * the result. */
void               pn_mesh_connection_open_async (
        const gchar         *tty_path,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

PnMeshConnection *pn_mesh_connection_open_finish (
        GAsyncResult        *result,
        GError             **error);

G_END_DECLS

#endif /* PN_MESH_CONNECTION_H */
