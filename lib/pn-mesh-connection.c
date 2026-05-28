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
#define FR_MODULE_CONFIG        9
#define FR_CHANNEL             10

/* ToRadio */
#define TR_PACKET               1
#define TR_WANT_CONFIG_ID       3

/* MyNodeInfo */
#define MNI_MY_NODE_NUM         1

/* NodeInfo */
#define NI_NUM                  1
#define NI_USER                 2
#define NI_SNR                  4   /* float, wire type 5 (fixed32)   */
#define NI_LAST_HEARD           5   /* fixed32                        */
#define NI_DEVICE_METRICS       6   /* embedded DeviceMetrics         */
#define NI_HOPS_AWAY            9   /* uint32                         */

/* User */
#define U_ID                    1
#define U_LONG_NAME             2
#define U_SHORT_NAME            3
#define U_HW_MODEL              5
#define U_ROLE                  7

/* DeviceMetrics (NodeInfo subfield -- distinct from DeviceMetadata). */
#define DMET_BATTERY_LEVEL      1
#define DMET_VOLTAGE            2

/* Channel */
#define CH_INDEX                1
#define CH_SETTINGS             2
#define CH_ROLE                 3

/* ChannelSettings */
#define CS_PSK                  2
#define CS_NAME                 3

/* MeshPacket */
#define MP_FROM                 1   /* fixed32                       */
#define MP_TO                   2   /* fixed32                       */
#define MP_CHANNEL              3   /* uint32 (channel index)         */
#define MP_DECODED              4

/* Data */
#define D_PORTNUM               1
#define D_PAYLOAD               2
#define D_WANT_RESPONSE         3

/* Meshtastic PortNum enum values we care about. */
#define PORTNUM_TEXT_MESSAGE_APP 1
#define PORTNUM_ADMIN_APP        6

/* MeshPacket.to value meaning "every node on the channel". */
#define MESHTASTIC_BROADCAST_ADDR  0xFFFFFFFFu

/* AdminMessage.
 *
 * Note set_channel is field 33, NOT 32 -- pip-mesh's own header
 * comment on build_set_channel_frame says 32 but its code uses 33,
 * and the upstream Meshtastic proto agrees on 33.  Trust the wire
 * pip-mesh actually sends; the comment is stale. */
#define AM_GET_DEVICE_METADATA_REQUEST   12
#define AM_GET_DEVICE_METADATA_RESPONSE  13
#define AM_SET_OWNER                     32
#define AM_SET_CHANNEL                   33
#define AM_SET_CONFIG                    34
#define AM_SET_MODULE_CONFIG             35

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

/* Config sub-fields (one of these is set per Config block).  The
 * upstream Config message numbers its oneof variants device=1,
 * position=2, power=3, network=4, display=5, lora=6, bluetooth=7,
 * security=8; we wire the ones the dialog touches. */
#define CFG_POSITION            2
#define CFG_POWER               3
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

/* PositionConfig fields.  Numbering skips 5 (deprecated
 * gps_attempt_time) and 7 (deprecated location_share).  gps_enabled
 * (3) is upstream-deprecated in favour of gps_mode (14) but still
 * round-tripped so older firmware keeps a sane GPS state. */
#define POS_POSITION_BROADCAST_SMART_ENABLED   1
#define POS_FIXED_POSITION                     2
#define POS_GPS_ENABLED                        3
#define POS_GPS_UPDATE_INTERVAL                4
#define POS_POSITION_BROADCAST_SECS            6
#define POS_POSITION_FLAGS                     8
#define POS_RX_GPIO                            9
#define POS_TX_GPIO                           10
#define POS_BROADCAST_SMART_MIN_DISTANCE      11
#define POS_BROADCAST_SMART_MIN_INTERVAL_SECS 12
#define POS_GPS_EN_GPIO                       13
#define POS_GPS_MODE                          14

/* PowerConfig fields.  Field 5 (deprecated mesh_sds_timeout_secs) is
 * skipped; powermon_enables (32) is a developer-only debug field that
 * the dialog does not expose -- it gets proto3-zero-reset on Apply,
 * same caveat the LoRa page carries for its skipped advanced fields. */
#define POW_IS_POWER_SAVING                    1
#define POW_ON_BATTERY_SHUTDOWN_AFTER_SECS     2
#define POW_ADC_MULTIPLIER_OVERRIDE            3   /* float, wire type 5 */
#define POW_WAIT_BLUETOOTH_SECS                4
#define POW_SDS_SECS                           6
#define POW_LS_SECS                            7
#define POW_MIN_WAKE_SECS                      8
#define POW_DEVICE_BATTERY_INA_ADDRESS         9

/* ModuleConfig oneof cases. */
#define MC_MQTT                  1
#define MC_EXTERNAL_NOTIFICATION 3
#define MC_TELEMETRY             6

/* ExternalNotificationConfig fields.  Numbering jumps because the
 * proto's per-output triplet (message / vibra / buzzer) was added in
 * stages -- fields 8..13 are the second + third pin variants. */
#define EN_ENABLED              1
#define EN_OUTPUT_MS            2
#define EN_OUTPUT               3
#define EN_ACTIVE               4
#define EN_ALERT_MESSAGE        5
#define EN_ALERT_BELL           6
#define EN_USE_PWM              7
#define EN_OUTPUT_VIBRA         8
#define EN_OUTPUT_BUZZER        9
#define EN_ALERT_MESSAGE_VIBRA 10
#define EN_ALERT_MESSAGE_BUZZER 11
#define EN_ALERT_BELL_VIBRA    12
#define EN_ALERT_BELL_BUZZER   13
#define EN_NAG_TIMEOUT         14
#define EN_USE_I2S_AS_BUZZER   15

/* MqttConfig fields.  Field 6 (json_enabled) is upstream-deprecated
 * and intentionally not exposed; field 11 (map_report_settings) is
 * captured as an opaque byte slice so writes can ship it back
 * unchanged -- proto3 default-zero would silently reset whatever
 * the device had configured. */
#define MQTT_ENABLED                   1
#define MQTT_ADDRESS                   2
#define MQTT_USERNAME                  3
#define MQTT_PASSWORD                  4
#define MQTT_ENCRYPTION_ENABLED        5
#define MQTT_JSON_ENABLED              6   /* deprecated, not exposed */
#define MQTT_TLS_ENABLED               7
#define MQTT_ROOT                      8
#define MQTT_PROXY_TO_CLIENT_ENABLED   9
#define MQTT_MAP_REPORTING_ENABLED    10
#define MQTT_MAP_REPORT_SETTINGS      11

/* TelemetryConfig fields.  Five logical sub-systems (device,
 * environment, air quality, power, health), each with its own
 * enable + interval + screen toggles.  Field numbering interleaves
 * because the proto added each sub-system in stages. */
#define TEL_DEVICE_UPDATE_INTERVAL         1
#define TEL_ENVIRONMENT_UPDATE_INTERVAL    2
#define TEL_ENVIRONMENT_MEASUREMENT_ENABLED 3
#define TEL_ENVIRONMENT_SCREEN_ENABLED     4
#define TEL_ENVIRONMENT_DISPLAY_FAHRENHEIT 5
#define TEL_AIR_QUALITY_ENABLED            6
#define TEL_AIR_QUALITY_INTERVAL           7
#define TEL_POWER_MEASUREMENT_ENABLED      8
#define TEL_POWER_UPDATE_INTERVAL          9
#define TEL_POWER_SCREEN_ENABLED          10
#define TEL_HEALTH_MEASUREMENT_ENABLED    11
#define TEL_HEALTH_UPDATE_INTERVAL        12
#define TEL_HEALTH_SCREEN_ENABLED         13
#define TEL_DEVICE_TELEMETRY_ENABLED      14
#define TEL_AIR_QUALITY_SCREEN_ENABLED    15

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

void
pn_mesh_node_free (PnMeshNode *node)
{
    if (node == NULL)
        return;
    g_free (node->id);
    g_free (node->long_name);
    g_free (node->short_name);
    g_slice_free (PnMeshNode, node);
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

    /* Phase 7 -- Test page text event queue.  Any TEXT_MESSAGE_APP
     * packet seen by parse_mesh_packet (whether it arrived via the
     * main-thread monitor pump or as side-traffic during a worker-
     * thread verify-cycle handshake) is pushed here as a malloc'd
     * PnMeshTextEvent.  The Test page drains the queue on every tick
     * of the dialog's monitor timer.  GAsyncQueue is thread-safe so
     * pushes from either context are race-free. */
    GAsyncQueue       *text_events;
};

typedef struct
{
    guint32 num;
    gchar  *id;
    gchar  *long_name;
    gchar  *short_name;
    guint32 hw_model;
    guint32 role;
    gfloat  snr;
    guint32 last_heard;
    guint32 hops_away;
    guint32 battery_level;
    gfloat  voltage;
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
        case U_ROLE:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                out->role = (guint32) v;
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* DeviceMetrics is the per-NodeInfo battery / voltage / channel
 * utilisation block.  Distinct from DeviceMetadata (firmware / hw
 * capabilities) -- they share a similar name but live on opposite
 * sides of the protocol.  We only pull battery + voltage today; the
 * page treats both as best-effort, so other fields are skipped. */
static void
parse_device_metrics (const guint8 *data, gsize size, SeenNode *out)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case DMET_BATTERY_LEVEL:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                out->battery_level = (guint32) v;
            break;
        }
        case DMET_VOLTAGE:
            pn_mesh_pb_read_float (&r, &out->voltage);
            break;
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
        case NI_SNR:
            pn_mesh_pb_read_float (&r, &node.snr);
            break;
        case NI_LAST_HEARD:
            pn_mesh_pb_read_fixed32 (&r, &node.last_heard);
            break;
        case NI_DEVICE_METRICS:
        {
            const guint8 *m_data;
            gsize         m_size;
            if (pn_mesh_pb_read_length (&r, &m_data, &m_size))
                parse_device_metrics (m_data, m_size, &node);
            break;
        }
        case NI_HOPS_AWAY:
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                node.hops_away = (guint32) v;
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

/* Parse a PositionConfig embedded message into the connection state.
 * broadcast_smart_min_distance is wire int32 -- protobuf varints are
 * unsigned, so a negative value would arrive as a 10-byte all-ones
 * varint; we treat the low 32 bits as int32 to preserve sign. */
static void
parse_position_config (PnMeshConnection *self,
                       const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_position = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;

        switch (field)
        {
        case POS_POSITION_BROADCAST_SMART_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_position_broadcast_smart_enabled = v != 0;
            break;
        case POS_FIXED_POSITION:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_fixed_position = v != 0;
            break;
        case POS_GPS_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_gps_enabled = v != 0;
            break;
        case POS_GPS_UPDATE_INTERVAL:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_gps_update_interval = (guint32) v;
            break;
        case POS_POSITION_BROADCAST_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_position_broadcast_secs = (guint32) v;
            break;
        case POS_POSITION_FLAGS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_position_flags = (guint32) v;
            break;
        case POS_RX_GPIO:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_rx_gpio = (guint32) v;
            break;
        case POS_TX_GPIO:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_tx_gpio = (guint32) v;
            break;
        case POS_BROADCAST_SMART_MIN_DISTANCE:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_broadcast_smart_min_distance = (gint32) v;
            break;
        case POS_BROADCAST_SMART_MIN_INTERVAL_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_broadcast_smart_min_interval_secs = (guint32) v;
            break;
        case POS_GPS_EN_GPIO:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_gps_en_gpio = (guint32) v;
            break;
        case POS_GPS_MODE:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pos_gps_mode = (guint32) v;
            break;
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse a PowerConfig embedded message into the connection state. */
static void
parse_power_config (PnMeshConnection *self,
                    const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_power = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;
        gfloat  f;

        switch (field)
        {
        case POW_IS_POWER_SAVING:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_is_power_saving = v != 0;
            break;
        case POW_ON_BATTERY_SHUTDOWN_AFTER_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_on_battery_shutdown_after_secs = (guint32) v;
            break;
        case POW_ADC_MULTIPLIER_OVERRIDE:
            if (pn_mesh_pb_read_float (&r, &f))
                self->state.pow_adc_multiplier_override = f;
            break;
        case POW_WAIT_BLUETOOTH_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_wait_bluetooth_secs = (guint32) v;
            break;
        case POW_SDS_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_sds_secs = (guint32) v;
            break;
        case POW_LS_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_ls_secs = (guint32) v;
            break;
        case POW_MIN_WAKE_SECS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_min_wake_secs = (guint32) v;
            break;
        case POW_DEVICE_BATTERY_INA_ADDRESS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.pow_device_battery_ina_address = (guint32) v;
            break;
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse a Config message, dispatching to the per-subconfig parser
 * for the field that is set.  Position, Power and LoRa are wired;
 * the remaining sub-configs (Device, Network, Display, Bluetooth,
 * Security) are skipped silently -- later phases can add them by
 * extending this switch. */
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
        case CFG_POSITION:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_position_config (self, p, s);
            break;
        }
        case CFG_POWER:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_power_config (self, p, s);
            break;
        }
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

/* Parse an ExternalNotificationConfig into the connection state. */
static void
parse_ext_notification (PnMeshConnection *self,
                        const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_ext_notification = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;

        switch (field)
        {
        case EN_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_enabled = v != 0;
            break;
        case EN_OUTPUT_MS:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_output_ms = (guint32) v;
            break;
        case EN_OUTPUT:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_output = (guint32) v;
            break;
        case EN_ACTIVE:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_active = v != 0;
            break;
        case EN_ALERT_MESSAGE:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_message = v != 0;
            break;
        case EN_ALERT_BELL:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_bell = v != 0;
            break;
        case EN_USE_PWM:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_use_pwm = v != 0;
            break;
        case EN_OUTPUT_VIBRA:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_output_vibra = (guint32) v;
            break;
        case EN_OUTPUT_BUZZER:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_output_buzzer = (guint32) v;
            break;
        case EN_ALERT_MESSAGE_VIBRA:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_message_vibra = v != 0;
            break;
        case EN_ALERT_MESSAGE_BUZZER:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_message_buzzer = v != 0;
            break;
        case EN_ALERT_BELL_VIBRA:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_bell_vibra = v != 0;
            break;
        case EN_ALERT_BELL_BUZZER:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_alert_bell_buzzer = v != 0;
            break;
        case EN_NAG_TIMEOUT:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_nag_timeout = (guint32) v;
            break;
        case EN_USE_I2S_AS_BUZZER:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.en_use_i2s_as_buzzer = v != 0;
            break;
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse an MqttConfig into the connection state.  Strings are taken
 * via read_string (g_strndup); map_report_settings is captured as a
 * GBytes copy so we don't depend on the reader's backing buffer
 * outliving the call. */
static void
parse_mqtt (PnMeshConnection *self,
            const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_mqtt = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;

        switch (field)
        {
        case MQTT_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.mqtt_enabled = v != 0;
            break;
        case MQTT_ADDRESS:
            g_free (self->state.mqtt_address);
            self->state.mqtt_address = read_string (&r);
            break;
        case MQTT_USERNAME:
            g_free (self->state.mqtt_username);
            self->state.mqtt_username = read_string (&r);
            break;
        case MQTT_PASSWORD:
            g_free (self->state.mqtt_password);
            self->state.mqtt_password = read_string (&r);
            break;
        case MQTT_ENCRYPTION_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.mqtt_encryption_enabled = v != 0;
            break;
        case MQTT_TLS_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.mqtt_tls_enabled = v != 0;
            break;
        case MQTT_ROOT:
            g_free (self->state.mqtt_root);
            self->state.mqtt_root = read_string (&r);
            break;
        case MQTT_PROXY_TO_CLIENT_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.mqtt_proxy_to_client_enabled = v != 0;
            break;
        case MQTT_MAP_REPORTING_ENABLED:
            if (pn_mesh_pb_read_varint (&r, &v))
                self->state.mqtt_map_reporting_enabled = v != 0;
            break;
        case MQTT_MAP_REPORT_SETTINGS:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
            {
                g_clear_pointer (&self->state.mqtt_map_report_settings,
                                 g_bytes_unref);
                self->state.mqtt_map_report_settings =
                        g_bytes_new (p, s);
            }
            break;
        }
        default:
            pn_mesh_pb_skip_field (&r, wire);
            break;
        }
    }
}

/* Parse a TelemetryConfig into the connection state.  Pure varint
 * fields throughout, so the parser is one big switch over the field
 * defines -- no read_string / read_length calls needed. */
static void
parse_telemetry (PnMeshConnection *self,
                 const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    self->state.have_telemetry = TRUE;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        guint64 v;

        if (!pn_mesh_pb_read_varint (&r, &v))
        {
            pn_mesh_pb_skip_field (&r, wire);
            continue;
        }

        switch (field)
        {
        case TEL_DEVICE_TELEMETRY_ENABLED:
            self->state.tel_device_telemetry_enabled = v != 0;
            break;
        case TEL_DEVICE_UPDATE_INTERVAL:
            self->state.tel_device_update_interval = (guint32) v;
            break;
        case TEL_ENVIRONMENT_MEASUREMENT_ENABLED:
            self->state.tel_environment_measurement_enabled = v != 0;
            break;
        case TEL_ENVIRONMENT_UPDATE_INTERVAL:
            self->state.tel_environment_update_interval = (guint32) v;
            break;
        case TEL_ENVIRONMENT_SCREEN_ENABLED:
            self->state.tel_environment_screen_enabled = v != 0;
            break;
        case TEL_ENVIRONMENT_DISPLAY_FAHRENHEIT:
            self->state.tel_environment_display_fahrenheit = v != 0;
            break;
        case TEL_AIR_QUALITY_ENABLED:
            self->state.tel_air_quality_enabled = v != 0;
            break;
        case TEL_AIR_QUALITY_INTERVAL:
            self->state.tel_air_quality_interval = (guint32) v;
            break;
        case TEL_AIR_QUALITY_SCREEN_ENABLED:
            self->state.tel_air_quality_screen_enabled = v != 0;
            break;
        case TEL_POWER_MEASUREMENT_ENABLED:
            self->state.tel_power_measurement_enabled = v != 0;
            break;
        case TEL_POWER_UPDATE_INTERVAL:
            self->state.tel_power_update_interval = (guint32) v;
            break;
        case TEL_POWER_SCREEN_ENABLED:
            self->state.tel_power_screen_enabled = v != 0;
            break;
        case TEL_HEALTH_MEASUREMENT_ENABLED:
            self->state.tel_health_measurement_enabled = v != 0;
            break;
        case TEL_HEALTH_UPDATE_INTERVAL:
            self->state.tel_health_update_interval = (guint32) v;
            break;
        case TEL_HEALTH_SCREEN_ENABLED:
            self->state.tel_health_screen_enabled = v != 0;
            break;
        default:
            /* Already consumed the value above for unknown fields. */
            break;
        }
    }
}

/* Parse a ModuleConfig message, dispatching to the per-module
 * parser for the field that is set.  Modules not yet wired (Phases
 * 11+) are skipped silently. */
static void
parse_module_config (PnMeshConnection *self,
                     const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;

    pn_mesh_pb_reader_init (&r, data, size);
    while (pn_mesh_pb_read_tag (&r, &field, &wire))
    {
        switch (field)
        {
        case MC_MQTT:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_mqtt (self, p, s);
            break;
        }
        case MC_EXTERNAL_NOTIFICATION:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_ext_notification (self, p, s);
            break;
        }
        case MC_TELEMETRY:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_telemetry (self, p, s);
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

/* Unwrap a MeshPacket.  Admin responses (portnum=ADMIN_APP) are
 * forwarded to parse_admin_message.  TEXT_MESSAGE_APP packets are
 * captured into the text_events queue for the Test page (Phase 7).
 * Any other portnum is silently dropped. */
static void
parse_mesh_packet (PnMeshConnection *self,
                   const guint8 *data, gsize size)
{
    PnMeshPbReader r;
    guint32        field, wire;
    guint64        portnum = 0;
    const guint8  *payload = NULL;
    gsize          payload_size = 0;
    guint32        from_node = 0;
    guint32        channel = 0;

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
        else if (field == MP_FROM)
        {
            pn_mesh_pb_read_fixed32 (&r, &from_node);
        }
        else if (field == MP_CHANNEL)
        {
            guint64 v;
            if (pn_mesh_pb_read_varint (&r, &v))
                channel = (guint32) v;
        }
        else
        {
            pn_mesh_pb_skip_field (&r, wire);
        }
    }

    if (portnum == PORTNUM_ADMIN_APP && payload != NULL && payload_size > 0)
    {
        parse_admin_message (self, payload, payload_size);
    }
    else if (portnum == PORTNUM_TEXT_MESSAGE_APP
             && payload != NULL && payload_size > 0
             && self->text_events != NULL)
    {
        /* g_strndup tolerates non-NUL-terminated payloads -- the
         * device sends raw UTF-8 with no terminator.  GTK can render
         * any valid UTF-8 string; invalid bytes show as replacement
         * characters which is fine for a diagnostic log. */
        PnMeshTextEvent *ev = g_slice_new0 (PnMeshTextEvent);
        ev->from_node = from_node;
        ev->channel   = channel;
        ev->text      = g_strndup ((const gchar *) payload, payload_size);
        ev->epoch_us  = g_get_real_time ();
        ev->outgoing  = FALSE;
        g_async_queue_push (self->text_events, ev);
    }
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
        case FR_MODULE_CONFIG:
        {
            const guint8 *p; gsize s;
            if (pn_mesh_pb_read_length (&r, &p, &s))
                parse_module_config (self, p, s);
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

/* Build state.nodes from seen_nodes and, while iterating, copy the
 * NodeInfo whose num == my_node_num into state.owner_*.  Like
 * pip-mesh, drop entries that lack a User.id -- those are partial
 * NodeInfos the device buffered before the User came in (would show
 * up as a row of em-dashes with no way to tell them apart).
 *
 * The published nodes array carries DUPLICATED strings so the
 * owner_* fields can keep their (move-stolen) copy without
 * double-freeing.  Cheap: the device exposes at most a few hundred
 * nodes, almost always under fifty. */
static void
resolve_owner (PnMeshConnection *self)
{
    guint i;

    g_ptr_array_set_size (self->state.nodes, 0);

    for (i = 0; i < self->seen_nodes->len; i++)
    {
        SeenNode   *n = &g_array_index (self->seen_nodes, SeenNode, i);
        PnMeshNode *node;

        if (n->id == NULL || *n->id == '\0')
            continue;

        node = g_slice_new0 (PnMeshNode);
        node->num           = n->num;
        node->id            = g_strdup (n->id);
        node->long_name     = g_strdup (n->long_name);
        node->short_name    = g_strdup (n->short_name);
        node->hw_model      = n->hw_model;
        node->role          = n->role;
        node->snr           = n->snr;
        node->last_heard    = n->last_heard;
        node->hops_away     = n->hops_away;
        node->battery_level = n->battery_level;
        node->voltage       = n->voltage;
        node->is_us         = (n->num != 0 &&
                               n->num == self->state.my_node_num);

        g_ptr_array_add (self->state.nodes, node);

        /* Side effect: capture the owner into state.owner_* the same
         * way the original resolve_owner did -- moving the strings out
         * of the SeenNode so the array's clear-func doesn't double-
         * free them when the handshake's scratch array is cleared. */
        if (node->is_us && self->state.owner_id == NULL)
        {
            self->state.owner_id         = n->id;         n->id         = NULL;
            self->state.owner_long_name  = n->long_name;  n->long_name  = NULL;
            self->state.owner_short_name = n->short_name; n->short_name = NULL;
            self->state.owner_hw_model   = n->hw_model;
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

    self->state.have_ext_notification    = FALSE;
    self->state.en_enabled               = FALSE;
    self->state.en_output_ms             = 0;
    self->state.en_output                = 0;
    self->state.en_output_vibra          = 0;
    self->state.en_output_buzzer         = 0;
    self->state.en_active                = FALSE;
    self->state.en_alert_message         = FALSE;
    self->state.en_alert_message_vibra   = FALSE;
    self->state.en_alert_message_buzzer  = FALSE;
    self->state.en_alert_bell            = FALSE;
    self->state.en_alert_bell_vibra      = FALSE;
    self->state.en_alert_bell_buzzer     = FALSE;
    self->state.en_use_pwm               = FALSE;
    self->state.en_nag_timeout           = 0;
    self->state.en_use_i2s_as_buzzer     = FALSE;

    self->state.have_mqtt                     = FALSE;
    self->state.mqtt_enabled                  = FALSE;
    g_clear_pointer (&self->state.mqtt_address,  g_free);
    g_clear_pointer (&self->state.mqtt_username, g_free);
    g_clear_pointer (&self->state.mqtt_password, g_free);
    g_clear_pointer (&self->state.mqtt_root,     g_free);
    g_clear_pointer (&self->state.mqtt_map_report_settings, g_bytes_unref);
    self->state.mqtt_encryption_enabled       = FALSE;
    self->state.mqtt_tls_enabled              = FALSE;
    self->state.mqtt_proxy_to_client_enabled  = FALSE;
    self->state.mqtt_map_reporting_enabled    = FALSE;

    self->state.have_position                                = FALSE;
    self->state.pos_position_broadcast_smart_enabled         = FALSE;
    self->state.pos_fixed_position                           = FALSE;
    self->state.pos_gps_enabled                              = FALSE;
    self->state.pos_gps_update_interval                      = 0;
    self->state.pos_position_broadcast_secs                  = 0;
    self->state.pos_position_flags                           = 0;
    self->state.pos_rx_gpio                                  = 0;
    self->state.pos_tx_gpio                                  = 0;
    self->state.pos_broadcast_smart_min_distance             = 0;
    self->state.pos_broadcast_smart_min_interval_secs        = 0;
    self->state.pos_gps_en_gpio                              = 0;
    self->state.pos_gps_mode                                 = 0;

    self->state.have_power                          = FALSE;
    self->state.pow_is_power_saving                 = FALSE;
    self->state.pow_on_battery_shutdown_after_secs  = 0;
    self->state.pow_adc_multiplier_override         = 0.0f;
    self->state.pow_wait_bluetooth_secs             = 0;
    self->state.pow_sds_secs                        = 0;
    self->state.pow_ls_secs                         = 0;
    self->state.pow_min_wake_secs                   = 0;
    self->state.pow_device_battery_ina_address      = 0;

    self->state.have_telemetry                       = FALSE;
    self->state.tel_device_telemetry_enabled         = FALSE;
    self->state.tel_device_update_interval           = 0;
    self->state.tel_environment_measurement_enabled  = FALSE;
    self->state.tel_environment_update_interval      = 0;
    self->state.tel_environment_screen_enabled       = FALSE;
    self->state.tel_environment_display_fahrenheit   = FALSE;
    self->state.tel_air_quality_enabled              = FALSE;
    self->state.tel_air_quality_interval             = 0;
    self->state.tel_air_quality_screen_enabled       = FALSE;
    self->state.tel_power_measurement_enabled        = FALSE;
    self->state.tel_power_update_interval            = 0;
    self->state.tel_power_screen_enabled             = FALSE;
    self->state.tel_health_measurement_enabled       = FALSE;
    self->state.tel_health_update_interval           = 0;
    self->state.tel_health_screen_enabled            = FALSE;

    g_ptr_array_set_size (self->state.channels, 0);
    g_ptr_array_set_size (self->state.nodes,    0);
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
    self->state.nodes = g_ptr_array_new_with_free_func (
            (GDestroyNotify) pn_mesh_node_free);
    self->seen_nodes = g_array_new (FALSE, TRUE, sizeof (SeenNode));
    g_array_set_clear_func (self->seen_nodes,
                            (GDestroyNotify) seen_node_clear);

    /* Text-event queue is created up front (handshake may produce
     * stale text packets too, though in practice we never see any
     * because the handshake only runs admin-request traffic). */
    self->text_events = g_async_queue_new_full (
            (GDestroyNotify) pn_mesh_text_event_free);

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
    g_free (self->state.mqtt_address);
    g_free (self->state.mqtt_username);
    g_free (self->state.mqtt_password);
    g_free (self->state.mqtt_root);
    if (self->state.mqtt_map_report_settings != NULL)
        g_bytes_unref (self->state.mqtt_map_report_settings);
    g_ptr_array_unref (self->state.channels);
    g_ptr_array_unref (self->state.nodes);

    g_array_unref (self->seen_nodes);

    if (self->text_events != NULL)
        g_async_queue_unref (self->text_events);

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

/* ------------------------------------------------------------------ */
/*  set_position_config / set_power_config — Phase 12 write paths       */
/* ------------------------------------------------------------------ */

/* Shared body: wrap an arbitrary serialised sub-config inside
 *
 *     AdminMessage { set_config (34) = Config { <cfg_field> = <sub> } }
 *
 * then run the same "send + settle + re-handshake" cycle as set_lora.
 * Used by both Position and Power so the two writers stay tiny. */
static gboolean
send_set_config_subblock (PnMeshConnection  *self,
                          guint32            cfg_field,
                          const guint8      *sub_b,
                          gsize              sub_n,
                          GError           **error)
{
    PnMeshPbWriter config_w, admin_w;
    GBytes        *config_bytes, *admin_bytes;
    const guint8  *config_b, *admin_b;
    gsize          config_n, admin_n;
    gboolean       ok;

    pn_mesh_pb_writer_init (&config_w);
    pn_mesh_pb_write_embedded_field (&config_w, cfg_field, sub_b, sub_n);
    config_bytes = pn_mesh_pb_writer_take_bytes (&config_w);
    pn_mesh_pb_writer_clear (&config_w);
    config_b = g_bytes_get_data (config_bytes, &config_n);

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

/* Settle + re-handshake after a set_config write.  Same pattern as
 * set_lora_config_sync's tail; broken out so the new Position/Power
 * writers can share it without duplicating the verify cycle. */
static gboolean
finish_set_config (PnMeshConnection *self, GError **error)
{
    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    pn_mesh_serial_drain (self->serial, 50);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

static gboolean
send_set_position_config (PnMeshConnection                 *self,
                          const PnMeshPositionConfigWrite  *cfg,
                          GError                          **error)
{
    PnMeshPbWriter pos_w;
    GBytes        *pos_bytes;
    const guint8  *pos_b;
    gsize          pos_n;
    gboolean       ok;

    pn_mesh_pb_writer_init (&pos_w);
    pn_mesh_pb_write_varint_field (&pos_w, POS_POSITION_BROADCAST_SMART_ENABLED,
                                   cfg->position_broadcast_smart_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&pos_w, POS_FIXED_POSITION,
                                   cfg->fixed_position ? 1 : 0);
    pn_mesh_pb_write_varint_field (&pos_w, POS_GPS_ENABLED,
                                   cfg->gps_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&pos_w, POS_GPS_UPDATE_INTERVAL,
                                   cfg->gps_update_interval);
    pn_mesh_pb_write_varint_field (&pos_w, POS_POSITION_BROADCAST_SECS,
                                   cfg->position_broadcast_secs);
    pn_mesh_pb_write_varint_field (&pos_w, POS_POSITION_FLAGS,
                                   cfg->position_flags);
    pn_mesh_pb_write_varint_field (&pos_w, POS_RX_GPIO,
                                   cfg->rx_gpio);
    pn_mesh_pb_write_varint_field (&pos_w, POS_TX_GPIO,
                                   cfg->tx_gpio);
    /* int32: cast to guint64 with sign-extension so a negative value
     * is encoded as the wire's 10-byte all-ones varint, matching the
     * Meshtastic proto definition.  The dialog clamps the spinner to
     * >= 0 for now, so this path is defensive only. */
    pn_mesh_pb_write_varint_field (&pos_w, POS_BROADCAST_SMART_MIN_DISTANCE,
                                   (guint64) (gint64) cfg->broadcast_smart_min_distance);
    pn_mesh_pb_write_varint_field (&pos_w, POS_BROADCAST_SMART_MIN_INTERVAL_SECS,
                                   cfg->broadcast_smart_min_interval_secs);
    pn_mesh_pb_write_varint_field (&pos_w, POS_GPS_EN_GPIO,
                                   cfg->gps_en_gpio);
    pn_mesh_pb_write_varint_field (&pos_w, POS_GPS_MODE,
                                   cfg->gps_mode);
    pos_bytes = pn_mesh_pb_writer_take_bytes (&pos_w);
    pn_mesh_pb_writer_clear (&pos_w);
    pos_b = g_bytes_get_data (pos_bytes, &pos_n);

    ok = send_set_config_subblock (self, CFG_POSITION, pos_b, pos_n, error);
    g_bytes_unref (pos_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_position_config_sync (PnMeshConnection                 *self,
                                             const PnMeshPositionConfigWrite  *cfg,
                                             GError                          **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_position_config (self, cfg, error))
        return FALSE;

    return finish_set_config (self, error);
}

typedef struct
{
    PnMeshConnection           *conn;
    PnMeshPositionConfigWrite   cfg;
} SetPositionConfigCall;

static void
set_position_config_call_free (gpointer data)
{
    g_slice_free (SetPositionConfigCall, data);
}

static void
set_position_config_thread_func (GTask *task, gpointer source,
                                 gpointer task_data,
                                 GCancellable *cancellable)
{
    SetPositionConfigCall *c   = task_data;
    GError                *err = NULL;
    gboolean               ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_position_config_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_position_config_async (PnMeshConnection                 *self,
                                              const PnMeshPositionConfigWrite  *cfg,
                                              GCancellable                     *cancellable,
                                              GAsyncReadyCallback               callback,
                                              gpointer                          user_data)
{
    GTask                 *task;
    SetPositionConfigCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetPositionConfigCall);
    c->conn = self;
    c->cfg  = *cfg;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_position_config_async);
    g_task_set_task_data  (task, c, set_position_config_call_free);
    g_task_run_in_thread  (task, set_position_config_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_position_config_finish (GAsyncResult *result,
                                               GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ----- Power ------------------------------------------------------- */

static gboolean
send_set_power_config (PnMeshConnection              *self,
                       const PnMeshPowerConfigWrite  *cfg,
                       GError                       **error)
{
    PnMeshPbWriter pow_w;
    GBytes        *pow_bytes;
    const guint8  *pow_b;
    gsize          pow_n;
    union { gfloat f; guint32 u; } adc;
    gboolean       ok;

    pn_mesh_pb_writer_init (&pow_w);
    pn_mesh_pb_write_varint_field (&pow_w, POW_IS_POWER_SAVING,
                                   cfg->is_power_saving ? 1 : 0);
    pn_mesh_pb_write_varint_field (&pow_w, POW_ON_BATTERY_SHUTDOWN_AFTER_SECS,
                                   cfg->on_battery_shutdown_after_secs);
    /* float as fixed32; even when 0.0f, ship it -- proto3 zero IS the
     * "let firmware decide" default, so an unset round-trip is fine. */
    adc.f = cfg->adc_multiplier_override;
    pn_mesh_pb_write_fixed32_field (&pow_w, POW_ADC_MULTIPLIER_OVERRIDE, adc.u);
    pn_mesh_pb_write_varint_field (&pow_w, POW_WAIT_BLUETOOTH_SECS,
                                   cfg->wait_bluetooth_secs);
    pn_mesh_pb_write_varint_field (&pow_w, POW_SDS_SECS,
                                   cfg->sds_secs);
    pn_mesh_pb_write_varint_field (&pow_w, POW_LS_SECS,
                                   cfg->ls_secs);
    pn_mesh_pb_write_varint_field (&pow_w, POW_MIN_WAKE_SECS,
                                   cfg->min_wake_secs);
    pn_mesh_pb_write_varint_field (&pow_w, POW_DEVICE_BATTERY_INA_ADDRESS,
                                   cfg->device_battery_ina_address);
    pow_bytes = pn_mesh_pb_writer_take_bytes (&pow_w);
    pn_mesh_pb_writer_clear (&pow_w);
    pow_b = g_bytes_get_data (pow_bytes, &pow_n);

    ok = send_set_config_subblock (self, CFG_POWER, pow_b, pow_n, error);
    g_bytes_unref (pow_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_power_config_sync (PnMeshConnection              *self,
                                          const PnMeshPowerConfigWrite  *cfg,
                                          GError                       **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_power_config (self, cfg, error))
        return FALSE;

    return finish_set_config (self, error);
}

typedef struct
{
    PnMeshConnection        *conn;
    PnMeshPowerConfigWrite   cfg;
} SetPowerConfigCall;

static void
set_power_config_call_free (gpointer data)
{
    g_slice_free (SetPowerConfigCall, data);
}

static void
set_power_config_thread_func (GTask *task, gpointer source,
                              gpointer task_data,
                              GCancellable *cancellable)
{
    SetPowerConfigCall *c   = task_data;
    GError             *err = NULL;
    gboolean            ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_power_config_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_power_config_async (PnMeshConnection              *self,
                                           const PnMeshPowerConfigWrite  *cfg,
                                           GCancellable                  *cancellable,
                                           GAsyncReadyCallback            callback,
                                           gpointer                       user_data)
{
    GTask              *task;
    SetPowerConfigCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetPowerConfigCall);
    c->conn = self;
    c->cfg  = *cfg;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_power_config_async);
    g_task_set_task_data  (task, c, set_power_config_call_free);
    g_task_run_in_thread  (task, set_power_config_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_power_config_finish (GAsyncResult *result,
                                            GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  set_ext_notification — Phase 9 write path                           */
/* ------------------------------------------------------------------ */

/* AdminMessage { set_module_config (35) = ModuleConfig {
 *     external_notification (3) = ExternalNotificationConfig {…} } }
 *
 * Same proto3-defaults caveat as LoRa: ship every field every time
 * or the device resets the omitted ones to zero on its next save. */
static gboolean
send_set_ext_notification (PnMeshConnection                 *self,
                           const PnMeshExtNotificationWrite *cfg,
                           GError                          **error)
{
    PnMeshPbWriter en_w, mc_w, admin_w;
    GBytes        *en_bytes, *mc_bytes, *admin_bytes;
    const guint8  *en_b, *mc_b, *admin_b;
    gsize          en_n,  mc_n,  admin_n;
    gboolean       ok;

    pn_mesh_pb_writer_init (&en_w);
    pn_mesh_pb_write_varint_field (&en_w, EN_ENABLED,
                                   cfg->enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_OUTPUT_MS,
                                   cfg->output_ms);
    pn_mesh_pb_write_varint_field (&en_w, EN_OUTPUT,
                                   cfg->output);
    pn_mesh_pb_write_varint_field (&en_w, EN_ACTIVE,
                                   cfg->active ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_MESSAGE,
                                   cfg->alert_message ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_BELL,
                                   cfg->alert_bell ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_USE_PWM,
                                   cfg->use_pwm ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_OUTPUT_VIBRA,
                                   cfg->output_vibra);
    pn_mesh_pb_write_varint_field (&en_w, EN_OUTPUT_BUZZER,
                                   cfg->output_buzzer);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_MESSAGE_VIBRA,
                                   cfg->alert_message_vibra ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_MESSAGE_BUZZER,
                                   cfg->alert_message_buzzer ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_BELL_VIBRA,
                                   cfg->alert_bell_vibra ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_ALERT_BELL_BUZZER,
                                   cfg->alert_bell_buzzer ? 1 : 0);
    pn_mesh_pb_write_varint_field (&en_w, EN_NAG_TIMEOUT,
                                   cfg->nag_timeout);
    pn_mesh_pb_write_varint_field (&en_w, EN_USE_I2S_AS_BUZZER,
                                   cfg->use_i2s_as_buzzer ? 1 : 0);
    en_bytes = pn_mesh_pb_writer_take_bytes (&en_w);
    pn_mesh_pb_writer_clear (&en_w);
    en_b = g_bytes_get_data (en_bytes, &en_n);

    /* ModuleConfig { external_notification (3) = ExternalNotificationConfig } */
    pn_mesh_pb_writer_init (&mc_w);
    pn_mesh_pb_write_embedded_field (&mc_w, MC_EXTERNAL_NOTIFICATION,
                                     en_b, en_n);
    mc_bytes = pn_mesh_pb_writer_take_bytes (&mc_w);
    pn_mesh_pb_writer_clear (&mc_w);
    g_bytes_unref (en_bytes);
    mc_b = g_bytes_get_data (mc_bytes, &mc_n);

    /* AdminMessage { set_module_config (35) = ModuleConfig } */
    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_MODULE_CONFIG,
                                     mc_b, mc_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (mc_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_ext_notification_sync (
        PnMeshConnection                 *self,
        const PnMeshExtNotificationWrite *cfg,
        GError                          **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_ext_notification (self, cfg, error))
        return FALSE;

    /* set_module_config writes do not normally reboot the device --
     * they just toggle a runtime feature -- but we still settle 0.5s
     * and re-handshake so the page repaints from authoritative
     * device state. */
    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    pn_mesh_serial_drain (self->serial, 50);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

typedef struct
{
    PnMeshConnection           *conn;
    PnMeshExtNotificationWrite  cfg;
} SetExtNotificationCall;

static void
set_ext_notification_call_free (gpointer data)
{
    g_slice_free (SetExtNotificationCall, data);
}

static void
set_ext_notification_thread_func (GTask *task, gpointer source,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
    SetExtNotificationCall *c   = task_data;
    GError                 *err = NULL;
    gboolean                ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_ext_notification_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_ext_notification_async (
        PnMeshConnection                 *self,
        const PnMeshExtNotificationWrite *cfg,
        GCancellable                     *cancellable,
        GAsyncReadyCallback               callback,
        gpointer                          user_data)
{
    GTask                  *task;
    SetExtNotificationCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetExtNotificationCall);
    c->conn = self;
    c->cfg  = *cfg;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_ext_notification_async);
    g_task_set_task_data  (task, c, set_ext_notification_call_free);
    g_task_run_in_thread  (task, set_ext_notification_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_ext_notification_finish (GAsyncResult *result,
                                                GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  set_mqtt_config — Phase 10 write path                               */
/* ------------------------------------------------------------------ */

/* AdminMessage { set_module_config (35) = ModuleConfig {
 *     mqtt (1) = MqttConfig {…} } }
 *
 * Same proto3-defaults caveat as the other writers: every field is
 * shipped on every Apply, including map_report_settings as opaque
 * bytes captured during the handshake -- omitting it would silently
 * reset the device's map-reporting cadence to zero.  json_enabled
 * (field 6, deprecated upstream) is intentionally NOT shipped --
 * the firmware ignores it, and writing it would round-trip a stale
 * value through future versions. */
static gboolean
send_set_mqtt_config (PnMeshConnection            *self,
                      const PnMeshMqttConfigWrite *cfg,
                      GError                     **error)
{
    PnMeshPbWriter mqtt_w, mc_w, admin_w;
    GBytes        *mqtt_bytes, *mc_bytes, *admin_bytes;
    const guint8  *mqtt_b, *mc_b, *admin_b;
    gsize          mqtt_n,  mc_n,  admin_n;
    gboolean       ok;

    pn_mesh_pb_writer_init (&mqtt_w);
    pn_mesh_pb_write_varint_field (&mqtt_w, MQTT_ENABLED,
                                   cfg->enabled ? 1 : 0);
    pn_mesh_pb_write_string_field (&mqtt_w, MQTT_ADDRESS,
                                   cfg->address != NULL ? cfg->address : "");
    pn_mesh_pb_write_string_field (&mqtt_w, MQTT_USERNAME,
                                   cfg->username != NULL ? cfg->username : "");
    pn_mesh_pb_write_string_field (&mqtt_w, MQTT_PASSWORD,
                                   cfg->password != NULL ? cfg->password : "");
    pn_mesh_pb_write_varint_field (&mqtt_w, MQTT_ENCRYPTION_ENABLED,
                                   cfg->encryption_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&mqtt_w, MQTT_TLS_ENABLED,
                                   cfg->tls_enabled ? 1 : 0);
    pn_mesh_pb_write_string_field (&mqtt_w, MQTT_ROOT,
                                   cfg->root != NULL ? cfg->root : "");
    pn_mesh_pb_write_varint_field (&mqtt_w, MQTT_PROXY_TO_CLIENT_ENABLED,
                                   cfg->proxy_to_client_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&mqtt_w, MQTT_MAP_REPORTING_ENABLED,
                                   cfg->map_reporting_enabled ? 1 : 0);
    /* Opaque MapReportSettings -- shipped back verbatim from the
     * read so the un-exposed sub-fields don't reset.  NULL or 0-size
     * is fine: pb_write_embedded_field accepts both and emits a
     * length-0 sub-message. */
    if (cfg->map_report_settings != NULL)
    {
        gsize         mrs_n;
        const guint8 *mrs_b = g_bytes_get_data (cfg->map_report_settings,
                                                &mrs_n);
        pn_mesh_pb_write_embedded_field (&mqtt_w, MQTT_MAP_REPORT_SETTINGS,
                                         mrs_b, mrs_n);
    }
    mqtt_bytes = pn_mesh_pb_writer_take_bytes (&mqtt_w);
    pn_mesh_pb_writer_clear (&mqtt_w);
    mqtt_b = g_bytes_get_data (mqtt_bytes, &mqtt_n);

    /* ModuleConfig { mqtt (1) = MqttConfig } */
    pn_mesh_pb_writer_init (&mc_w);
    pn_mesh_pb_write_embedded_field (&mc_w, MC_MQTT, mqtt_b, mqtt_n);
    mc_bytes = pn_mesh_pb_writer_take_bytes (&mc_w);
    pn_mesh_pb_writer_clear (&mc_w);
    g_bytes_unref (mqtt_bytes);
    mc_b = g_bytes_get_data (mc_bytes, &mc_n);

    /* AdminMessage { set_module_config (35) = ModuleConfig } */
    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_MODULE_CONFIG,
                                     mc_b, mc_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (mc_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_mqtt_config_sync (
        PnMeshConnection             *self,
        const PnMeshMqttConfigWrite  *cfg,
        GError                      **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_mqtt_config (self, cfg, error))
        return FALSE;

    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    pn_mesh_serial_drain (self->serial, 50);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

typedef struct
{
    PnMeshConnection       *conn;
    /* Strings duplicated for thread-safety -- the worker thread
     * reads them after the calling thread returned from _async. */
    PnMeshMqttConfigWrite   cfg;
    gchar                  *address;
    gchar                  *username;
    gchar                  *password;
    gchar                  *root;
    GBytes                 *map_report_settings;
} SetMqttConfigCall;

static void
set_mqtt_config_call_free (gpointer data)
{
    SetMqttConfigCall *c = data;
    g_free (c->address);
    g_free (c->username);
    g_free (c->password);
    g_free (c->root);
    if (c->map_report_settings != NULL)
        g_bytes_unref (c->map_report_settings);
    g_slice_free (SetMqttConfigCall, c);
}

static void
set_mqtt_config_thread_func (GTask *task, gpointer source,
                             gpointer task_data,
                             GCancellable *cancellable)
{
    SetMqttConfigCall *c   = task_data;
    GError            *err = NULL;
    gboolean           ok;

    (void) source;
    (void) cancellable;

    /* Re-point the const-string pointers at our owned copies before
     * the worker thread reads them; the cfg struct was shallow-
     * copied at _async time so its pointers still reference caller
     * stack/temporaries. */
    c->cfg.address             = c->address;
    c->cfg.username            = c->username;
    c->cfg.password            = c->password;
    c->cfg.root                = c->root;
    c->cfg.map_report_settings = c->map_report_settings;

    ok = pn_mesh_connection_set_mqtt_config_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_mqtt_config_async (
        PnMeshConnection             *self,
        const PnMeshMqttConfigWrite  *cfg,
        GCancellable                 *cancellable,
        GAsyncReadyCallback           callback,
        gpointer                      user_data)
{
    GTask             *task;
    SetMqttConfigCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetMqttConfigCall);
    c->conn = self;
    c->cfg  = *cfg;
    c->address  = g_strdup (cfg->address  != NULL ? cfg->address  : "");
    c->username = g_strdup (cfg->username != NULL ? cfg->username : "");
    c->password = g_strdup (cfg->password != NULL ? cfg->password : "");
    c->root     = g_strdup (cfg->root     != NULL ? cfg->root     : "");
    c->map_report_settings = cfg->map_report_settings != NULL
            ? g_bytes_ref (cfg->map_report_settings) : NULL;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_mqtt_config_async);
    g_task_set_task_data  (task, c, set_mqtt_config_call_free);
    g_task_run_in_thread  (task, set_mqtt_config_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_mqtt_config_finish (GAsyncResult *result,
                                           GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  set_telemetry_config — Phase 11 write path                          */
/* ------------------------------------------------------------------ */

/* AdminMessage { set_module_config (35) = ModuleConfig {
 *     telemetry (6) = TelemetryConfig {…} } }
 *
 * Same proto3-defaults rule: ship every field every time.  No
 * sub-messages in TelemetryConfig, so no opaque round-trip dance. */
static gboolean
send_set_telemetry_config (PnMeshConnection                 *self,
                           const PnMeshTelemetryConfigWrite *cfg,
                           GError                          **error)
{
    PnMeshPbWriter tel_w, mc_w, admin_w;
    GBytes        *tel_bytes, *mc_bytes, *admin_bytes;
    const guint8  *tel_b, *mc_b, *admin_b;
    gsize          tel_n,  mc_n,  admin_n;
    gboolean       ok;

    pn_mesh_pb_writer_init (&tel_w);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_DEVICE_UPDATE_INTERVAL,
                                   cfg->device_update_interval);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_ENVIRONMENT_UPDATE_INTERVAL,
                                   cfg->environment_update_interval);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_ENVIRONMENT_MEASUREMENT_ENABLED,
                                   cfg->environment_measurement_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_ENVIRONMENT_SCREEN_ENABLED,
                                   cfg->environment_screen_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_ENVIRONMENT_DISPLAY_FAHRENHEIT,
                                   cfg->environment_display_fahrenheit ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_AIR_QUALITY_ENABLED,
                                   cfg->air_quality_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_AIR_QUALITY_INTERVAL,
                                   cfg->air_quality_interval);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_POWER_MEASUREMENT_ENABLED,
                                   cfg->power_measurement_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_POWER_UPDATE_INTERVAL,
                                   cfg->power_update_interval);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_POWER_SCREEN_ENABLED,
                                   cfg->power_screen_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_HEALTH_MEASUREMENT_ENABLED,
                                   cfg->health_measurement_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_HEALTH_UPDATE_INTERVAL,
                                   cfg->health_update_interval);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_HEALTH_SCREEN_ENABLED,
                                   cfg->health_screen_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_DEVICE_TELEMETRY_ENABLED,
                                   cfg->device_telemetry_enabled ? 1 : 0);
    pn_mesh_pb_write_varint_field (&tel_w, TEL_AIR_QUALITY_SCREEN_ENABLED,
                                   cfg->air_quality_screen_enabled ? 1 : 0);
    tel_bytes = pn_mesh_pb_writer_take_bytes (&tel_w);
    pn_mesh_pb_writer_clear (&tel_w);
    tel_b = g_bytes_get_data (tel_bytes, &tel_n);

    pn_mesh_pb_writer_init (&mc_w);
    pn_mesh_pb_write_embedded_field (&mc_w, MC_TELEMETRY, tel_b, tel_n);
    mc_bytes = pn_mesh_pb_writer_take_bytes (&mc_w);
    pn_mesh_pb_writer_clear (&mc_w);
    g_bytes_unref (tel_bytes);
    mc_b = g_bytes_get_data (mc_bytes, &mc_n);

    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_MODULE_CONFIG,
                                     mc_b, mc_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (mc_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_telemetry_config_sync (
        PnMeshConnection                 *self,
        const PnMeshTelemetryConfigWrite *cfg,
        GError                          **error)
{
    g_return_val_if_fail (self != NULL && cfg != NULL, FALSE);

    if (!send_set_telemetry_config (self, cfg, error))
        return FALSE;

    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);

    pn_mesh_serial_drain (self->serial, 50);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

typedef struct
{
    PnMeshConnection            *conn;
    PnMeshTelemetryConfigWrite   cfg;
} SetTelemetryConfigCall;

static void
set_telemetry_config_call_free (gpointer data)
{
    g_slice_free (SetTelemetryConfigCall, data);
}

static void
set_telemetry_config_thread_func (GTask *task, gpointer source,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
    SetTelemetryConfigCall *c   = task_data;
    GError                 *err = NULL;
    gboolean                ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_telemetry_config_sync (c->conn, &c->cfg, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_telemetry_config_async (
        PnMeshConnection                  *self,
        const PnMeshTelemetryConfigWrite  *cfg,
        GCancellable                      *cancellable,
        GAsyncReadyCallback                callback,
        gpointer                           user_data)
{
    GTask                  *task;
    SetTelemetryConfigCall *c;

    g_return_if_fail (self != NULL && cfg != NULL);

    c = g_slice_new0 (SetTelemetryConfigCall);
    c->conn = self;
    c->cfg  = *cfg;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_telemetry_config_async);
    g_task_set_task_data  (task, c, set_telemetry_config_call_free);
    g_task_run_in_thread  (task, set_telemetry_config_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_telemetry_config_finish (GAsyncResult *result,
                                                GError      **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  set_channel — Phase 5                                               */
/* ------------------------------------------------------------------ */

/* AdminMessage { set_channel (33, embedded) = Channel {
 *     index (1, varint),
 *     settings (2, embedded) = ChannelSettings { psk (2), name (3) },
 *     role (3, varint) } }
 */
static gboolean
send_set_channel (PnMeshConnection *self,
                  guint32           index,
                  const gchar      *name,
                  const guint8     *psk,
                  gsize             psk_size,
                  guint32           role,
                  GError          **error)
{
    PnMeshPbWriter settings_w, channel_w, admin_w;
    GBytes        *settings_bytes, *channel_bytes, *admin_bytes;
    const guint8  *settings_b, *channel_b, *admin_b;
    gsize          settings_n,  channel_n,  admin_n;
    gboolean       ok;

    /* ChannelSettings: psk first, then name -- pip-mesh writes them in
     * that order and we mirror it (protobuf field order is not
     * required to be ascending but staying consistent eases packet
     * captures from cross-comparing). */
    pn_mesh_pb_writer_init (&settings_w);
    if (psk != NULL && psk_size > 0)
        pn_mesh_pb_write_bytes_field (&settings_w, CS_PSK, psk, psk_size);
    if (name != NULL && *name != '\0')
        pn_mesh_pb_write_string_field (&settings_w, CS_NAME, name);
    settings_bytes = pn_mesh_pb_writer_take_bytes (&settings_w);
    pn_mesh_pb_writer_clear (&settings_w);
    settings_b = g_bytes_get_data (settings_bytes, &settings_n);

    /* Channel.  Index is always written even when zero (it identifies
     * which slot to write); role is always written (0 = DISABLED is
     * the delete signal).  Settings carries the inner payload, even
     * when zero bytes long (for a DISABLED write). */
    pn_mesh_pb_writer_init (&channel_w);
    pn_mesh_pb_write_varint_field   (&channel_w, CH_INDEX, index);
    pn_mesh_pb_write_embedded_field (&channel_w, CH_SETTINGS,
                                     settings_b, settings_n);
    pn_mesh_pb_write_varint_field   (&channel_w, CH_ROLE, role);
    channel_bytes = pn_mesh_pb_writer_take_bytes (&channel_w);
    pn_mesh_pb_writer_clear (&channel_w);
    g_bytes_unref (settings_bytes);
    channel_b = g_bytes_get_data (channel_bytes, &channel_n);

    /* AdminMessage { set_channel (33) = Channel } */
    pn_mesh_pb_writer_init (&admin_w);
    pn_mesh_pb_write_embedded_field (&admin_w, AM_SET_CHANNEL,
                                     channel_b, channel_n);
    admin_bytes = pn_mesh_pb_writer_take_bytes (&admin_w);
    pn_mesh_pb_writer_clear (&admin_w);
    g_bytes_unref (channel_bytes);
    admin_b = g_bytes_get_data (admin_bytes, &admin_n);

    ok = send_admin (self, admin_b, admin_n, /*want_response=*/FALSE, error);
    g_bytes_unref (admin_bytes);
    return ok;
}

gboolean
pn_mesh_connection_set_channel_sync (PnMeshConnection *self,
                                     guint32           index,
                                     const gchar      *name,
                                     const guint8     *psk,
                                     gsize             psk_size,
                                     guint32           role,
                                     GError          **error)
{
    g_return_val_if_fail (self != NULL, FALSE);

    if (!send_set_channel (self, index, name, psk, psk_size, role, error))
        return FALSE;

    /* set_channel quirk: the Meshtastic firmware buffers channel-slot
     * changes in RAM and only persists them when the serial session
     * ends -- the DTR drop on close is the commit signal.  pip-mesh
     * sidesteps this because it uses `echo > $tty` (a fresh
     * open/close per write); our long-lived fd has to bounce
     * explicitly or the write looks accepted but is silently dropped
     * (the verify-cycle reads back the unchanged channel list, and
     * pip-mesh run later also sees no change).  set_owner and
     * set_lora_config don't need this -- they are stored differently
     * by the firmware. */
    g_usleep ((gulong) POST_WRITE_SETTLE_MS * 1000);
    if (!pn_mesh_serial_reopen (self->serial, error))
        return FALSE;
    /* Fresh fd means fresh frame stream: drop any partial bytes the
     * old reader buffered. */
    pn_mesh_frame_reader_free (self->frames);
    self->frames = pn_mesh_frame_reader_new ();
    pn_mesh_serial_drain (self->serial, OPEN_DRAIN_MS);
    clear_state (self);

    if (!run_handshake (self, error))
    {
        if (error != NULL && *error == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                         "Device did not respond to the post-write "
                         "verification handshake within %d ms.",
                         HANDSHAKE_TOTAL_MS);
        return FALSE;
    }

    if (self->state.my_node_num != 0)
        request_device_metadata (self);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  set_channel async wrapper                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnMeshConnection *conn;
    guint32           index;
    gchar            *name;     /* NUL-terminated; may be ""           */
    guint8           *psk;      /* freshly malloc'd; may be NULL       */
    gsize             psk_size;
    guint32           role;
} SetChannelCall;

static void
set_channel_call_free (gpointer data)
{
    SetChannelCall *c = data;
    g_free (c->name);
    g_free (c->psk);
    g_slice_free (SetChannelCall, c);
}

static void
set_channel_thread_func (GTask *task, gpointer source,
                         gpointer task_data, GCancellable *cancellable)
{
    SetChannelCall *c   = task_data;
    GError         *err = NULL;
    gboolean        ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_set_channel_sync (
            c->conn, c->index, c->name,
            c->psk, c->psk_size, c->role, &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error   (task, err);
}

void
pn_mesh_connection_set_channel_async (PnMeshConnection    *self,
                                      guint32              index,
                                      const gchar         *name,
                                      const guint8        *psk,
                                      gsize                psk_size,
                                      guint32              role,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    GTask          *task;
    SetChannelCall *c;

    g_return_if_fail (self != NULL);

    c = g_slice_new0 (SetChannelCall);
    c->conn  = self;
    c->index = index;
    c->name  = g_strdup (name != NULL ? name : "");
    c->role  = role;
    if (psk != NULL && psk_size > 0)
    {
        c->psk      = g_memdup2 (psk, psk_size);
        c->psk_size = psk_size;
    }

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_set_channel_async);
    g_task_set_task_data  (task, c, set_channel_call_free);
    g_task_run_in_thread  (task, set_channel_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_set_channel_finish (GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------ */
/*  Test page (Phase 7) — text send + live receive                      */
/* ------------------------------------------------------------------ */

void
pn_mesh_text_event_free (PnMeshTextEvent *event)
{
    if (event == NULL)
        return;
    g_free (event->text);
    g_slice_free (PnMeshTextEvent, event);
}

PnMeshTextEvent *
pn_mesh_connection_take_text_event (PnMeshConnection *self)
{
    if (self == NULL || self->text_events == NULL)
        return NULL;
    return g_async_queue_try_pop (self->text_events);
}

gint
pn_mesh_connection_pump_monitor (PnMeshConnection *self)
{
    gint    frames_done = 0;
    guint8  buf[1024];
    gssize  n;
    GBytes *frame;

    g_return_val_if_fail (self != NULL, 0);

    /* Tight non-blocking read loop.  Pulls every byte the kernel
     * already has buffered for us, but never waits for more -- the
     * dialog calls us on a timer and any unread bytes will surface
     * on the next tick.  timeout_ms = 0 means "poll once, return
     * immediately if no data". */
    for (;;)
    {
        n = pn_mesh_serial_read (self->serial, buf, sizeof buf, 0, NULL);
        if (n <= 0)
            break;
        pn_mesh_frame_reader_feed (self->frames, buf, (gsize) n);
        while ((frame = pn_mesh_frame_reader_take (self->frames)) != NULL)
        {
            gsize         fs;
            const guint8 *fd = g_bytes_get_data (frame, &fs);
            parse_from_radio (self, fd, fs);
            frames_done++;
            g_bytes_unref (frame);
        }
    }

    return frames_done;
}

/* Build and emit a single ToRadio { packet = MeshPacket { to=BROADCAST,
 * channel=@channel_index, decoded = Data { portnum=TEXT_MESSAGE_APP,
 * payload=@text } } }.  Returns FALSE on serial write failure -- which
 * is the only thing that can actually fail at this layer.  The radio
 * never acknowledges the frame back to us (we did not request a
 * want_ack) so success here means "on the wire", not "delivered". */
static gboolean
send_text (PnMeshConnection *self,
           guint32           channel_index,
           const gchar      *text,
           GError          **error)
{
    PnMeshPbWriter data_w, mesh_w, toradio_w;
    GBytes        *data_bytes;
    GBytes        *mesh_bytes;
    GBytes        *toradio_bytes;
    const guint8  *data_b, *mesh_b, *toradio_b;
    gsize          data_n,  mesh_n,  toradio_n;
    gsize          text_n;
    gboolean       ok;

    text_n = strlen (text);

    /* Data { portnum (1) = TEXT_MESSAGE_APP, payload (2) = text } */
    pn_mesh_pb_writer_init (&data_w);
    pn_mesh_pb_write_varint_field (&data_w, D_PORTNUM,
                                   PORTNUM_TEXT_MESSAGE_APP);
    pn_mesh_pb_write_bytes_field  (&data_w, D_PAYLOAD,
                                   (const guint8 *) text, text_n);
    data_bytes = pn_mesh_pb_writer_take_bytes (&data_w);
    pn_mesh_pb_writer_clear (&data_w);
    data_b = g_bytes_get_data (data_bytes, &data_n);

    /* MeshPacket { to=BROADCAST, channel=@channel_index, decoded=Data } */
    pn_mesh_pb_writer_init (&mesh_w);
    pn_mesh_pb_write_fixed32_field  (&mesh_w, MP_TO,
                                     MESHTASTIC_BROADCAST_ADDR);
    if (channel_index != 0)
        pn_mesh_pb_write_varint_field (&mesh_w, MP_CHANNEL,
                                       channel_index);
    pn_mesh_pb_write_embedded_field (&mesh_w, MP_DECODED,
                                     data_b, data_n);
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

    ok = pn_mesh_serial_write_frame (self->serial, toradio_b, toradio_n,
                                     error);
    g_bytes_unref (toradio_bytes);
    return ok;
}

gboolean
pn_mesh_connection_send_text_sync (PnMeshConnection *self,
                                   guint32           channel_index,
                                   const gchar      *text,
                                   GError          **error)
{
    g_return_val_if_fail (self != NULL, FALSE);
    g_return_val_if_fail (text != NULL, FALSE);

    if (!send_text (self, channel_index, text, error))
        return FALSE;

    /* Push a synthetic outgoing event so the Test page log includes
     * our own sends interleaved with incoming traffic.  from_node = 0
     * marks it as "this is us"; the page renders it differently. */
    if (self->text_events != NULL)
    {
        PnMeshTextEvent *ev = g_slice_new0 (PnMeshTextEvent);
        ev->from_node = 0;
        ev->channel   = channel_index;
        ev->text      = g_strdup (text);
        ev->epoch_us  = g_get_real_time ();
        ev->outgoing  = TRUE;
        g_async_queue_push (self->text_events, ev);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  send_text async wrapper                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnMeshConnection *conn;
    guint32           channel_index;
    gchar            *text;
} SendTextCall;

static void
send_text_call_free (gpointer data)
{
    SendTextCall *c = data;
    g_free (c->text);
    g_slice_free (SendTextCall, c);
}

static void
send_text_thread_func (GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable)
{
    SendTextCall *c   = task_data;
    GError       *err = NULL;
    gboolean      ok;

    (void) source;
    (void) cancellable;

    ok = pn_mesh_connection_send_text_sync (c->conn,
                                            c->channel_index,
                                            c->text,
                                            &err);
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error (task, err);
}

void
pn_mesh_connection_send_text_async (PnMeshConnection    *self,
                                    guint32              channel_index,
                                    const gchar         *text,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    GTask        *task;
    SendTextCall *c;

    g_return_if_fail (self != NULL);
    g_return_if_fail (text != NULL);

    c = g_slice_new0 (SendTextCall);
    c->conn          = self;
    c->channel_index = channel_index;
    c->text          = g_strdup (text);

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_connection_send_text_async);
    g_task_set_task_data  (task, c, send_text_call_free);
    g_task_run_in_thread  (task, send_text_thread_func);
    g_object_unref (task);
}

gboolean
pn_mesh_connection_send_text_finish (GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}
