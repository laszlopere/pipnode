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

/* Post-write settle pause: pip-mesh's `sleep 0.5` after every admin
 * write, matched here so the device has time to process the change
 * before the verifying handshake re-reads state. */
#define POST_WRITE_SETTLE_MS    500

/* Admin-response read budget.  pip-mesh's request_device_metadata
 * uses `timeout 5` (5 s) -- longer than the handshake's 3 s because
 * the device sometimes needs the extra time to assemble metadata.
 * Same here. */
#define ADMIN_RESPONSE_TOTAL_MS 5000

/* ------------------------------------------------------------------ */
/*  Field numbers (mirror /usr/bin/pip-mesh)                            */
/* ------------------------------------------------------------------ */

/* FromRadio */
#define FR_MESH_PACKET          2
#define FR_MY_INFO              3
#define FR_NODE_INFO            4
#define FR_CONFIG_COMPLETE_ID   7
#define FR_CHANNEL             10

/* ToRadio */
#define TR_PACKET               1
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

/* MeshPacket */
#define MP_TO                   2
#define MP_DECODED              4

/* Data */
#define D_PORTNUM               1
#define D_PAYLOAD               2
#define D_WANT_RESPONSE         3

/* Meshtastic PortNum enum values we care about. */
#define PORTNUM_ADMIN_APP       6

/* AdminMessage */
#define AM_GET_DEVICE_METADATA_REQUEST   12
#define AM_GET_DEVICE_METADATA_RESPONSE  13
#define AM_SET_OWNER                     32
#define AM_SET_CONFIG                    34

/* DeviceMetadata */
#define DM_FIRMWARE_VERSION     1
#define DM_DEVICE_STATE_VERSION 2
#define DM_CAN_SHUTDOWN         3
#define DM_HAS_WIFI             4
#define DM_HAS_BLUETOOTH        5
#define DM_HAS_ETHERNET         6
#define DM_ROLE                 7
#define DM_HW_MODEL             9

/* FromRadio.config / AdminMessage.set_config wraps a Config message. */
#define FR_CONFIG               5

/* Config sub-fields (one of these is set per Config block). */
#define CFG_LORA                6

/* LoRaConfig.  Numbering jumps from 2 -> 7 because the upstream
 * proto reserves 3..6 for the manual modem-parameter fields we are
 * not exposing in this phase (spread_factor / bandwidth / coding
 * rate / frequency_offset). */
#define LORA_USE_PRESET         1
#define LORA_MODEM_PRESET       2
#define LORA_REGION             7
#define LORA_HOP_LIMIT          8
#define LORA_TX_ENABLED         9
#define LORA_TX_POWER          10
#define LORA_CHANNEL_NUM       11

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

/* Parse a LoRaConfig embedded message into the connection state. */
static void
parse_lora_config (PnMeshConnection *self,
                   const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_lora_config = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;

        switch (field)
        {
        case LORA_USE_PRESET:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_use_preset = v != 0;
            break;
        case LORA_MODEM_PRESET:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_modem_preset = (guint32) v;
            break;
        case LORA_REGION:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_region = (guint32) v;
            break;
        case LORA_HOP_LIMIT:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_hop_limit = (guint32) v;
            break;
        case LORA_TX_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_tx_enabled = v != 0;
            break;
        case LORA_TX_POWER:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_tx_power = (guint32) v;
            break;
        case LORA_CHANNEL_NUM:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.lora_channel_num = (guint32) v;
            break;
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse a Config message, dispatching to the per-subconfig parser
 * for the field that is set.  Only LoRaConfig is wired today; the
 * other sub-configs (Device, Position, Power, Network, Display,
 * Bluetooth, Security) are skipped silently -- later phases can
 * add them by extending this switch. */
static void
parse_config (PnMeshConnection *self,
              const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case CFG_LORA:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_lora_config (self, p, s);
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse a DeviceMetadata embedded message into the connection state. */
static void
parse_device_metadata (PnMeshConnection *self,
                       const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    /* Mark the slot as populated even on a partial parse: any field
     * arriving is more than we had before, and the Identity page will
     * show "—" for whatever stayed at zero. */
    self->state.have_metadata = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case DM_FIRMWARE_VERSION:
            g_free (self->state.firmware_version);
            self->state.firmware_version = read_string (&r);
            break;
        case DM_CAN_SHUTDOWN:
        case DM_HAS_WIFI:
        case DM_HAS_BLUETOOTH:
        case DM_HAS_ETHERNET:
        case DM_ROLE:
        case DM_HW_MODEL:
        case DM_DEVICE_STATE_VERSION:
        {
            guint64 v;
            if (!pn_mesh_pb_read_varint (&r, &v))
                break;
            switch (field)
            {
            case DM_CAN_SHUTDOWN:  self->state.can_shutdown   = v != 0; break;
            case DM_HAS_WIFI:      self->state.has_wifi       = v != 0; break;
            case DM_HAS_BLUETOOTH: self->state.has_bluetooth  = v != 0; break;
            case DM_HAS_ETHERNET:  self->state.has_ethernet   = v != 0; break;
            case DM_ROLE:          self->state.role           = (guint32) v; break;
            case DM_HW_MODEL:      self->state.hw_model       = (guint32) v; break;
            default: /* DM_DEVICE_STATE_VERSION: ignored */              break;
            }
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Walk an AdminMessage payload, pulling out responses we care about. */
static void
parse_admin_message (PnMeshConnection *self,
                     const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case AM_GET_DEVICE_METADATA_RESPONSE:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_device_metadata (self, p, s);
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Unwrap a MeshPacket: we only care about admin responses, which
 * arrive as decoded(Data) with portnum=ADMIN_APP.  Other portnums
 * (TEXT_MESSAGE_APP, NodeInfo broadcasts, etc.) are skipped silently
 * -- Phase 7 will revisit when the test-message monitor lands. */
static void
parse_mesh_packet (PnMeshConnection *self,
                   const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;
    guint64        portnum = 0;
    const guint8  *payload = NULL;
    gsize          payload_size = 0;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        if (field == MP_DECODED && wire == PN_MESH_PB_WIRE_LEN)
        {
            /* Nested Data message: pull out portnum + payload. */
            const guint8  *dp; gsize ds;
            PnMeshPbReader dr;
            guint32        df, dw;

            if (!pn_mesh_pb_read_length (&r, &dp, &ds))
                break;
            pn_mesh_pb_reader_init (&dr, dp, ds);
            while (pn_mesh_pb_read_tag (&dr, &df, &dw))
            {
                switch (df)
                {
                case D_PORTNUM:
                    pn_mesh_pb_read_varint (&dr, &portnum);
                    break;
                case D_PAYLOAD:
                    pn_mesh_pb_read_length (&dr, &payload, &payload_size);
                    break;
                default:
                    pn_mesh_pb_skip_field (&dr, dw);
                    break;
                }
            }
        }
        else
        {
            pn_mesh_pb_skip_field (&r, wire);
        }
    }

    if (portnum == PORTNUM_ADMIN_APP && payload != NULL && payload_size > 0)
        parse_admin_message (self, payload, payload_size);
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
        case FR_MESH_PACKET:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_mesh_packet (self, p, s);
            break;
        }
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
        case FR_CONFIG:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_config (self, p, s);
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
/*  Admin frame builders                                                */
/* ------------------------------------------------------------------ */

/* Wrap @admin_payload in Data { portnum=ADMIN_APP, payload=admin,
 * want_response? } + MeshPacket { to=my_node_num, decoded=Data } +
 * ToRadio { packet=MeshPacket } and write it out as a serial frame.
 * Returns FALSE on serial write failure. */
static gboolean
send_admin (PnMeshConnection *self,
            const guint8     *admin_payload,
            gsize             admin_size,
            gboolean          want_response,
            GError          **error)
{
    PnMeshPbWriter data_w, mesh_w, toradio_w;
    GBytes        *data_bytes;
    GBytes        *mesh_bytes;
    GBytes        *toradio_bytes;
    const guint8  *data_b, *mesh_b, *toradio_b;
    gsize          data_n,  mesh_n,  toradio_n;
    gboolean       ok;

    /* Data { portnum (1) = 6, payload (2) = admin, [want_response (3) = 1] } */
    pn_mesh_pb_writer_init (&data_w);
    pn_mesh_pb_write_varint_field (&data_w, D_PORTNUM, PORTNUM_ADMIN_APP);
    pn_mesh_pb_write_bytes_field  (&data_w, D_PAYLOAD,
                                   admin_payload, admin_size);
    if (want_response)
        pn_mesh_pb_write_varint_field (&data_w, D_WANT_RESPONSE, 1);
    data_bytes = pn_mesh_pb_writer_take_bytes (&data_w);
    pn_mesh_pb_writer_clear (&data_w);
    data_b = g_bytes_get_data (data_bytes, &data_n);

    /* MeshPacket { to (2, fixed32) = my_node_num, decoded (4) = Data } */
    pn_mesh_pb_writer_init (&mesh_w);
    pn_mesh_pb_write_fixed32_field  (&mesh_w, MP_TO,
                                     self->state.my_node_num);
    pn_mesh_pb_write_embedded_field (&mesh_w, MP_DECODED, data_b, data_n);
    mesh_bytes = pn_mesh_pb_writer_take_bytes (&mesh_w);
    pn_mesh_pb_writer_clear (&mesh_w);
    g_bytes_unref (data_bytes);
    mesh_b = g_bytes_get_data (mesh_bytes, &mesh_n);

    /* ToRadio { packet (1) = MeshPacket } */
    pn_mesh_pb_writer_init (&toradio_w);
    pn_mesh_pb_write_embedded_field (&toradio_w, TR_PACKET, mesh_b, mesh_n);
    toradio_bytes = pn_mesh_pb_writer_take_bytes (&toradio_w);
    pn_mesh_pb_writer_clear (&toradio_w);
    g_bytes_unref (mesh_bytes);
    toradio_b = g_bytes_get_data (toradio_bytes, &toradio_n);

    ok = pn_mesh_serial_write_frame (self->serial, toradio_b, toradio_n, error);
    g_bytes_unref (toradio_bytes);
    return ok;
}

/* Drive the serial loop for @budget_ms (or until @complete_cb returns
 * TRUE), feeding every inbound frame to parse_from_radio.  Used both
 * by the verify-cycle handshake and by request_metadata: the caller
 * decides what "done" means via @stop_when_complete (the handshake
 * stops on config_complete_id; metadata stops on have_metadata). */
static void
pump_frames (PnMeshConnection *self,
             gint              budget_ms,
             gboolean         *stop_when_seen_complete)
{
    gint64 start = g_get_monotonic_time ();

    for (;;)
    {
        gint64  elapsed_ms = (g_get_monotonic_time () - start) / 1000;
        guint8  buf[1024];
        gssize  n;
        GBytes *frame;

        if (elapsed_ms >= budget_ms)
            break;
        if (stop_when_seen_complete != NULL && *stop_when_seen_complete)
            break;

        n = pn_mesh_serial_read (self->serial, buf, sizeof buf,
                                 HANDSHAKE_POLL_MS, NULL);
        if (n <= 0)
            continue;

        pn_mesh_frame_reader_feed (self->frames, buf, (gsize) n);
        while ((frame = pn_mesh_frame_reader_take (self->frames)) != NULL)
        {
            gsize         fs;
            const guint8 *fd = g_bytes_get_data (frame, &fs);
            if (parse_from_radio (self, fd, fs)
                && stop_when_seen_complete != NULL)
                *stop_when_seen_complete = TRUE;
            g_bytes_unref (frame);
        }
    }
}

/* AdminMessage { get_device_metadata_request (12, varint) = 1 } */
static void
request_device_metadata (PnMeshConnection *self)
{
    PnMeshPbWriter w;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_varint_field (&w,
                                   AM_GET_DEVICE_METADATA_REQUEST, 1);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);
    data = g_bytes_get_data (bytes, &size);

    if (send_admin (self, data, size, /*want_response=*/TRUE, NULL))
    {
        /* Pump frames until metadata arrives or budget elapses.  Drive
         * the loop's "complete" hook off have_metadata so we exit as
         * soon as a DeviceMetadata response lands. */
        pump_frames (self, ADMIN_RESPONSE_TOTAL_MS,
                     &self->state.have_metadata);
    }
    g_bytes_unref (bytes);
}

/* AdminMessage { set_owner (32, embedded) = User { long_name, short_name } } */
static gboolean
send_set_owner (PnMeshConnection *self,
                const gchar      *long_name,
                const gchar      *short_name,
                GError          **error)
{
    PnMeshPbWriter user_w, admin_w;
    GBytes        *user_bytes;
    GBytes        *admin_bytes;
    const guint8  *user_b, *admin_b;
    gsize          user_n,  admin_n;
    gboolean       ok;

    /* Empty string == "leave unchanged" per pip-mesh; we just omit
     * the field in that case (proto3 default is the empty string). */
    pn_mesh_pb_writer_init (&user_w);
    if (long_name != NULL && *long_name != '\0')
        pn_mesh_pb_write_string_field (&user_w, U_LONG_NAME,  long_name);
    if (short_name != NULL && *short_name != '\0')
        pn_mesh_pb_write_string_field (&user_w, U_SHORT_NAME, short_name);
    user_bytes = pn_mesh_pb_writer_take_bytes (&user_w);
    pn_mesh_pb_writer_clear (&user_w);
    user_b = g_bytes_get_data (user_bytes, &user_n);

    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_OWNER, user_b, user_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (user_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

/* Wipe everything the handshake learned so the re-read after a write
 * starts from a clean slate (the device's reply is authoritative). */
static void
clear_state (PnMeshConnection *self)
{
    g_clear_pointer (&self->state.owner_id,         g_free);
    g_clear_pointer (&self->state.owner_long_name,  g_free);
    g_clear_pointer (&self->state.owner_short_name, g_free);
    g_clear_pointer (&self->state.firmware_version, g_free);

    self->state.my_node_num     = 0;
    self->state.owner_hw_model  = 0;
    self->state.config_complete = FALSE;
    self->state.have_metadata   = FALSE;
    self->state.hw_model        = 0;
    self->state.role            = 0;
    self->state.has_wifi        = FALSE;
    self->state.has_bluetooth   = FALSE;
    self->state.has_ethernet    = FALSE;
    self->state.can_shutdown    = FALSE;
    self->state.have_lora_config = FALSE;
    self->state.lora_use_preset  = FALSE;
    self->state.lora_modem_preset = 0;
    self->state.lora_region      = 0;
    self->state.lora_hop_limit   = 0;
    self->state.lora_tx_enabled  = FALSE;
    self->state.lora_tx_power    = 0;
    self->state.lora_channel_num = 0;

    g_ptr_array_set_size (self->state.channels, 0);
    g_array_set_size (self->seen_nodes, 0);
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

    /* Fetch DeviceMetadata for firmware version + capabilities.  Best
     * effort: a device that doesn't answer the admin request leaves
     * the metadata block empty rather than failing the open -- the
     * handshake state is already useful by itself. */
    if (self->state.my_node_num != 0)
        request_device_metadata (self);

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
    g_free (self->state.firmware_version);
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

/* ------------------------------------------------------------------ */
/*  set_owner — write + settle + verify                                 */
/* ------------------------------------------------------------------ */

gboolean
pn_mesh_connection_set_owner_sync (PnMeshConnection *self,
                                   const gchar      *long_name,
                                   const gchar      *short_name,
                                   GError          **error)
{
    g_return_val_if_fail (self != NULL, FALSE);

    if (!send_set_owner (self, long_name, short_name, error))
        return FALSE;

    /* Settle: the device needs a beat to process the admin message
     * before its next handshake reply will reflect the new owner.
     * pip-mesh uses 0.5 s consistently after every write. */
    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    /* Drain any noise the device emitted while we were waiting; the
     * verifying handshake needs a clean start. */
    pn_mesh_serial_drain (self->serial, 50);

    /* Wipe the in-memory state so the verifying handshake's reply is
     * authoritative; otherwise stale NodeInfo entries would shadow
     * the freshly-written one. */
    clear_state (self);

    /* Re-handshake to pull the new owner.  Errors at this point mean
     * the write probably succeeded but we lost contact -- return TRUE
     * for the write itself but the state will look empty; the caller
     * can decide how to surface that.  For now treat it as failure
     * because the UI promises "applied" only when we have proof. */
    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    /* Re-fetch metadata too: a firmware that includes the owner in
     * metadata responses (or that bumps a counter) stays consistent. */
    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  set_owner async wrapper                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnMeshConnection *conn;
    gchar            *long_name;
    gchar            *short_name;
} SetOwnerCall;

static void
set_owner_call_free (gpointer data)
{
    SetOwnerCall *c = data;
    g_free (c->long_name);
    g_free (c->short_name);
    g_slice_free (SetOwnerCall, c);
}

static void
set_owner_thread_func (GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable)
{
    SetOwnerCall *c   = task_data;
    GError       *err = NULL;
    gboolean      ok;

    (void) source;
    (void) cancellable;   /* the read pump checks the GTask budget via
                             return path; we honour cancellation by
                             dropping the result, not by interrupting
                             a write mid-frame. */

    ok = pn_mesh_connection_set_owner_sync (c->conn,
                                            c->long_name,
                                            c->short_name,
                                            &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_owner_async (PnMeshConnection    *self,
                                    const gchar         *long_name,
                                    const gchar         *short_name,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    GTask        *task;
    SetOwnerCall *c;

    g_return_if_fail (self != NULL);

    c = g_slice_new0 (SetOwnerCall);
    c->conn       = self;
    c->long_name  = g_strdup (long_name);
    c->short_name = g_strdup (short_name);

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_owner_async);
    g_task_set_task_data  (task, c, set_owner_call_free);
    g_task_run_in_thread  (task, set_owner_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_owner_finish (GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  set_lora_config — Phase 4 write path                                */
/* ------------------------------------------------------------------ */

/* AdminMessage { set_config (34) = Config { lora (6) = LoRaConfig {…} } } */
static gboolean
send_set_lora_config (PnMeshConnection            *self,
                      const PnMeshLoraConfigWrite *cfg,
                      GError                     **error)
{
    PnMeshPbWriter lora_w, config_w, admin_w;
    GBytes        *lora_bytes, *config_bytes, *admin_bytes;
    const guint8  *lora_b, *config_b, *admin_b;
    gsize          lora_n,  config_n,  admin_n;
    gboolean       ok;

    /* LoRaConfig.  Every field is written even when zero -- omitting
     * proto3 defaults here would silently reset whatever the device
     * had to its enum-zero value (region=UNSET would brick the
     * radio).  The caller is responsible for reading current state
     * first and only mutating what it wants to change. */
    pn_mesh_pb_writer_init (&lora_w);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_USE_PRESET,
                                   cfg->use_preset ? 1 : 0);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_MODEM_PRESET,
                                   cfg->modem_preset);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_REGION,
                                   cfg->region);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_HOP_LIMIT,
                                   cfg->hop_limit);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_TX_ENABLED,
                                   cfg->tx_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_TX_POWER,
                                   cfg->tx_power);
    pn_mesh_pb_write_varint_field (&lora_w, LORA_CHANNEL_NUM,
                                   cfg->channel_num);
    lora_bytes = pn_mesh_pb_writer_take_bytes (&lora_w);
    pn_mesh_pb_writer_clear (&lora_w);
    lora_b = g_bytes_get_data (lora_bytes, &lora_n);

    /* Config { lora (6) = LoRaConfig } */
    pn_mesh_pb_writer_init (&config_w);
    pn_mesh_pb_write_embedded_field (&config_w, CFG_LORA, lora_b, lora_n);
    config_bytes = pn_mesh_pb_writer_take_bytes (&config_w);
    pn_mesh_pb_writer_clear (&config_w);
    g_bytes_unref (lora_bytes);
    config_b = g_bytes_get_data (config_bytes, &config_n);

    /* AdminMessage { set_config (34) = Config } */
    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_CONFIG,
                                     config_b, config_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (config_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_lora_config_sync (PnMeshConnection            *self,
                                         const PnMeshLoraConfigWrite *cfg,
                                         GError                     **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_lora_config (self, cfg, error))
        return FALSE;

    /* set_config writes that change region or modem_preset commonly
     * reboot the device -- the Meshtastic firmware re-initialises
     * the radio on those changes.  pip-mesh's same-pattern code
     * waits 0.5s and then re-handshakes; if the device is mid-
     * reboot the handshake's 3s budget covers it. */
    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    pn_mesh_serial_drain (self->serial, 50);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms (a "
                         "region or modem-preset change may have "
                         "triggered a reboot -- try the dialog again).",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

typedef struct
{
    PnMeshConnection      *conn;
    PnMeshLoraConfigWrite  cfg;
} SetLoraConfigCall;

static void
set_lora_config_call_free (gpointer data)
{
    g_slice_free (SetLoraConfigCall, data);
}

static void
set_lora_config_thread_func (GTask *task, gpointer source,
                             gpointer task_data,
                             GCancellable *cancellable)
{
    SetLoraConfigCall *c   = task_data;
    GError            *err = NULL;
    gboolean           ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_lora_config_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_lora_config_async (PnMeshConnection            *self,
                                          const PnMeshLoraConfigWrite *cfg,
                                          GCancellable                *cancellable,
                                          GAsyncReadyCallback          callback,
                                          gpointer                     user_data)
{
    GTask             *task;
    SetLoraConfigCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetLoraConfigCall);
    c->conn = self;
    c->cfg  = *cfg;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_lora_config_async);
    g_task_set_task_data  (task, c, set_lora_config_call_free);
    g_task_run_in_thread  (task, set_lora_config_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_lora_config_finish (GAsyncResult *result,
                                           GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}
