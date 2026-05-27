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

/* ------------------------------------------------------------------ */
/*  PnMeshConnection — handshake + per-device live session.            */
/*                                                                     */
/*  Synchronous handshake mirrors pip-mesh's load_node_data() flow:    */
/*    stty configure (done in pn-mesh-serial.c)                        */
/*    drain stale bytes (200 ms)                                       */
/*    send ToRadio { want_config_id = 1 }                              */
/*    read FromRadio frames for up to 3 s OR until config_complete_id  */
/*                                                                     */
/*  Frame field numbers come straight from /usr/bin/pip-mesh's         */
/*  parse_from_radio (lines 2176-) and friends; the proto definitions  */
/*  upstream are in meshtastic/protobufs.                              */
/*                                                                     */
/*  Scope (Phase 2d): only the handshake.  Owner names come from the   */
/*  NodeInfo whose num == my_node_num.  Firmware version, hardware     */
/*  model, role -- those live in DeviceMetadata which needs an admin   */
/*  round-trip (get_device_metadata_request) wrapped in MeshPacket +   */
/*  Data + AdminMessage; deferred to Phase 3 when the admin protocol   */
/*  lands.  Phase 2e's Identity page shows what is in scope here.      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-connection.h"
#include "pn-mesh-pb.h"
#include "pn-mesh-serial.h"

#include <string.h>

/* Handshake budget: pip-mesh hardcodes 3 s and explicitly notes that
 * "some firmware versions never send config_complete_id, so we rely
 * on the timeout to end the configuration stage."  Same here. */
#define HANDSHAKE_TOTAL_MS 3000

/* Per-poll wait inside the handshake loop.  Small enough to bail
 * promptly when config_complete_id arrives early, large enough that
 * the loop is not busy. */
#define HANDSHAKE_POLL_MS  200

/* Stale-byte drain when opening (matches pip-mesh's 0.2 s). */
#define OPEN_DRAIN_MS      200

/* ------------------------------------------------------------------ */
/*  Field numbers (mirror /usr/bin/pip-mesh)                            */
/* ------------------------------------------------------------------ */

/* FromRadio */
#define FR_MY_INFO              3
#define FR_NODE_INFO            4
#define FR_CONFIG_COMPLETE_ID   7
#define FR_CHANNEL             10

/* ToRadio */
#define TR_WANT_CONFIG_ID       3

/* MyNodeInfo */
#define MNI_MY_NODE_NUM         1

/* NodeInfo */
#define NI_NUM                  1
#define NI_USER                 2

/* User */
#define U_ID                    1
#define U_LONG_NAME             2
#define U_SHORT_NAME            3
#define U_HW_MODEL              5

/* Channel */
#define CH_INDEX                1
#define CH_SETTINGS             2
#define CH_ROLE                 3

/* ChannelSettings */
#define CS_PSK                  2
#define CS_NAME                 3

/* ------------------------------------------------------------------ */
/*  Channel helper                                                      */
/* ------------------------------------------------------------------ */

void
pn_mesh_channel_free (PnMeshChannel *channel)
{
    if (channel == NULL)
        return;
    g_free (channel->name);
    g_free (channel->psk);
    g_slice_free (PnMeshChannel, channel);
}

/* ------------------------------------------------------------------ */
/*  Connection state                                                    */
/* ------------------------------------------------------------------ */

struct _PnMeshConnection
{
    gchar             *tty_path;
    PnMeshSerial      *serial;
    PnMeshFrameReader *frames;
    PnMeshState        state;

    /* While parsing NodeInfo entries we don't yet know which one is
     * the owner: the rule is "the NodeInfo whose num == my_node_num".
     * Stash every (num, long, short, id, hw) we see, then resolve at
     * the end.  Keeps the parser stateless across frames. */
    GArray            *seen_nodes;   /* SeenNode                       */
};

typedef struct
{
    guint32 num;
    gchar  *id;
    gchar  *long_name;
    gchar  *short_name;
    guint32 hw_model;
} SeenNode;

static void
seen_node_clear (SeenNode *s)
{
    g_free (s->id);
    g_free (s->long_name);
    g_free (s->short_name);
}

/* ------------------------------------------------------------------ */
/*  Parsers                                                             */
/* ------------------------------------------------------------------ */

/* Read a length-delimited string into a freshly-allocated NUL-
 * terminated buffer, or NULL on EOF/error.  Used for every protobuf
 * string field below. */
static gchar *
read_string (PnMeshPbReader *r)
{
    const guint8 *data;
    gsize         size;

    if (!pn_mesh_pb_read_length (r, &data, &size))
        return NULL;
    return g_strndup ((const gchar *) data, size);
}

static void
parse_user (PnMeshConnection *self, const guint8 *data, gsize size,
            SeenNode *out)
{
    PnMeshPbReader r;
    guint32        field, wire;

    (void) self;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case U_ID:
            g_free (out->id);
            out->id = read_string (&r);
            break;
        case U_LONG_NAME:
            g_free (out->long_name);
            out->long_name = read_string (&r);
            break;
        case U_SHORT_NAME:
            g_free (out->short_name);
            out->short_name = read_string (&r);
            break;
        case U_HW_MODEL:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                out->hw_model = (guint32) v;
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

static void
parse_node_info (PnMeshConnection *self, const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;
    SeenNode       node;

    memset (&node, 0, sizeof node);

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case NI_NUM:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                node.num = (guint32) v;
            break;
        }
        case NI_USER:
        {
            const guint8 *user_data;
            gsize         user_size;
            if (pn_mesh_pb_read_length (&r, &user_data, &user_size))
                parse_user (self, user_data, user_size, &node);
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }

    g_array_append_val (self->seen_nodes, node);
}

static void
parse_my_node_info (PnMeshConnection *self,
                    const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case MNI_MY_NODE_NUM:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.my_node_num = (guint32) v;
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

static void
parse_channel_settings (const guint8 *data, gsize size,
                        PnMeshChannel *out)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case CS_PSK:
        {
            const guint8 *psk_data;
            gsize         psk_size;
            if (pn_mesh_pb_read_length (&r, &psk_data, &psk_size))
            {
                g_free (out->psk);
                out->psk = g_memdup2 (psk_data, psk_size);
                out->psk_size = psk_size;
            }
            break;
        }
        case CS_NAME:
            g_free (out->name);
            out->name = read_string (&r);
            break;
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

static void
parse_channel (PnMeshConnection *self, const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;
    PnMeshChannel *ch;

    ch = g_slice_new0 (PnMeshChannel);

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case CH_INDEX:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                ch->index = (guint32) v;
            break;
        }
        case CH_SETTINGS:
        {
            const guint8 *s_data;
            gsize         s_size;
            if (pn_mesh_pb_read_length (&r, &s_data, &s_size))
                parse_channel_settings (s_data, s_size, ch);
            break;
        }
        case CH_ROLE:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                ch->role = (guint32) v;
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }

    g_ptr_array_add (self->state.channels, ch);
}

/* Parse one FromRadio frame.  Returns TRUE if config_complete_id was
 * seen in this frame (the caller can then stop reading early). */
static gboolean
parse_from_radio (PnMeshConnection *self,
                  const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;
    gboolean       complete = FALSE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case FR_MY_INFO:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_my_node_info (self, p, s);
            break;
        }
        case FR_NODE_INFO:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_node_info (self, p, s);
            break;
        }
        case FR_CHANNEL:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_channel (self, p, s);
            break;
        }
        case FR_CONFIG_COMPLETE_ID:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                complete = TRUE;
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
    return complete;
}

/* ------------------------------------------------------------------ */
/*  Handshake                                                           */
/* ------------------------------------------------------------------ */

/* Wire-form for ToRadio { want_config_id = 1 }:
 *   tag = (3 << 3) | 0 = 0x18
 *   value (varint) = 0x01
 */
static const guint8 WANT_CONFIG_PAYLOAD[] = { 0x18, 0x01 };

/* Pick the owner out of seen_nodes by matching node->num against
 * my_node_num, and move its strings into state.owner_*. */
static void
resolve_owner (PnMeshConnection *self)
{
    guint i;

    for (i = 0; i < self->seen_nodes->len; i++)
    {
        SeenNode *n = &g_array_index (self->seen_nodes, SeenNode, i);
        if (n->num == self->state.my_node_num && n->num != 0)
        {
            /* Move strings into state, leaving the array entry NULLed
             * so the array's clear-func doesn't double-free. */
            self->state.owner_id         = n->id;         n->id         = NULL;
            self->state.owner_long_name  = n->long_name;  n->long_name  = NULL;
            self->state.owner_short_name = n->short_name; n->short_name = NULL;
            self->state.owner_hw_model   = n->hw_model;
            return;
        }
    }
}

/* Send the want_config_id and pull frames until config_complete_id
 * arrives or HANDSHAKE_TOTAL_MS has passed without progress.  Returns
 * TRUE iff at least one valid frame was parsed (the device responded
 * at all); the caller treats a totally-silent device as a failure. */
static gboolean
run_handshake (PnMeshConnection *self, GError **error)
{
    gint64   start;
    gboolean got_anything = FALSE;
    gboolean complete     = FALSE;

    if (!pn_mesh_serial_write_frame (self->serial,
                                     WANT_CONFIG_PAYLOAD,
                                     sizeof WANT_CONFIG_PAYLOAD,
                                     error))
        return FALSE;

    start = g_get_monotonic_time ();
    while (!complete)
    {
        guint8  buf[1024];
        gint64  elapsed_ms;
        gssize  n;
        GBytes *frame;

        elapsed_ms = (g_get_monotonic_time () - start) / 1000;
        if (elapsed_ms >= HANDSHAKE_TOTAL_MS)
            break;

        n = pn_mesh_serial_read (self->serial, buf, sizeof buf,
                                 HANDSHAKE_POLL_MS, error);
        if (n < 0)
            return FALSE;
        if (n == 0)
            continue;   /* poll timeout, try again until total budget */

        pn_mesh_frame_reader_feed (self->frames, buf, (gsize) n);
        while ((frame = pn_mesh_frame_reader_take (self->frames)) != NULL)
        {
            gsize         frame_size;
            const guint8 *frame_data = g_bytes_get_data (frame, &frame_size);

            got_anything = TRUE;
            if (parse_from_radio (self, frame_data, frame_size))
                complete = TRUE;
            g_bytes_unref (frame);
        }
    }

    self->state.config_complete = complete;
    resolve_owner (self);
    return got_anything;
}

/* ------------------------------------------------------------------ */
/*  Public open/close                                                   */
/* ------------------------------------------------------------------ */

PnMeshConnection *
pn_mesh_connection_open_sync (const gchar *tty_path, GError **error)
{
    PnMeshConnection *self;
    PnMeshSerial     *serial;

    g_return_val_if_fail (tty_path != NULL, NULL);

    serial = pn_mesh_serial_open (tty_path, error);
    if (serial == NULL)
        return NULL;

    self = g_slice_new0 (PnMeshConnection);
    self->tty_path = g_strdup (tty_path);
    self->serial   = serial;
    self->frames   = pn_mesh_frame_reader_new ();
    self->state.channels = g_ptr_array_new_with_free_func (
            (GDestroyNotify) pn_mesh_channel_free);
    self->seen_nodes = g_array_new (FALSE, TRUE, sizeof (SeenNode));
    g_array_set_clear_func (self->seen_nodes,
                            (GDestroyNotify) seen_node_clear);

    /* Drain stale bytes the device may have emitted before we opened.
     * Skipping this corrupts the first response (pip-mesh comment). */
    pn_mesh_serial_drain (self->serial, OPEN_DRAIN_MS);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device on %s did not respond to the "
                         "configuration handshake within %d ms.",
                         tty_path, HANDSHAKE_TOTAL_MS);
        pn_mesh_connection_close (self);
        return NULL;
    }

    return self;
}

void
pn_mesh_connection_close (PnMeshConnection *self)
{
    if (self == NULL)
        return;

    pn_mesh_serial_close (self->serial);
    pn_mesh_frame_reader_free (self->frames);
    g_free (self->tty_path);

    g_free (self->state.owner_id);
    g_free (self->state.owner_long_name);
    g_free (self->state.owner_short_name);
    g_ptr_array_unref (self->state.channels);

    g_array_unref (self->seen_nodes);

    g_slice_free (PnMeshConnection, self);
}

const PnMeshState *
pn_mesh_connection_get_state (PnMeshConnection *self)
{
    return self != NULL ? &self->state : NULL;
}

const gchar *
pn_mesh_connection_get_tty (PnMeshConnection *self)
{
    return self != NULL ? self->tty_path : NULL;
}

/* ------------------------------------------------------------------ */
/*  Async wrapper                                                       */
/* ------------------------------------------------------------------ */

static void
open_thread_func (GTask *task, gpointer source, gpointer task_data,
                  GCancellable *cancellable)
{
    const gchar      *tty_path = task_data;
    GError           *error    = NULL;
    PnMeshConnection *conn;

    (void) source;

    if (g_task_return_error_if_cancelled (task))
        return;

    conn = pn_mesh_connection_open_sync (tty_path, &error);

    /* Re-check cancellation: the user may have closed the dialog
     * while we were waiting for the handshake to time out.  Drop the
     * fd in that case so we do not leak a serial port. */
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable))
    {
        if (conn != NULL)
            pn_mesh_connection_close (conn);
        g_clear_error (&error);
        g_task_return_error_if_cancelled (task);
        return;
    }

    if (conn == NULL)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, conn,
                               (GDestroyNotify) pn_mesh_connection_close);
}

void
pn_mesh_connection_open_async (const gchar         *tty_path,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
    GTask *task;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_open_async);
    g_task_set_task_data (task, g_strdup (tty_path), g_free);
    g_task_run_in_thread (task, open_thread_func);
    g_object_unref (task);
}

PnMeshConnection *
pn_mesh_connection_open_finish (GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
    return g_task_propagate_pointer (G_TASK (result), error);
}
