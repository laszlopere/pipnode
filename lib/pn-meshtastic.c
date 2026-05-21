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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-meshtastic.h"
#include "pn-message.h"
#include "pn-node-dialog-helpers.h"

#include <json-glib/json-glib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  USB device table                                                   */
/*                                                                     */
/*  Mirrors the KNOWN_DEVICES table the pip-mesh bash prototype keeps  */
/*  (see /usr/bin/pip-mesh around line 88).  A new board added there   */
/*  must also be added here for it to appear in the device combo.     */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar *vendor;
    const gchar *product;
    const gchar *label;
} PnMeshKnownDevice;

static const PnMeshKnownDevice known_devices[] = {
    { "10c4", "ea60", "Heltec V3"        },
    { "239a", "0029", "Tracker"          },
    { "239a", "8027", "SenseCAP Tracker" },
    { "239a", "8029", "Tracker"          },
};

/* Maps a Meshtastic HardwareModel enum value to its symbolic name.
 * Mirrors the format_hw_model() table the pip-mesh bash prototype keeps
 * (see /usr/bin/pip-mesh around line 1073) so the settings dialog can
 * label the local device with the same string the pip-mesh CLI prints.
 * Returns %NULL for unknown values; callers fall back to the raw int. */
static const gchar *
format_hw_model (guint32 model)
{
    switch (model)
    {
    case 1:   return "TLORA_V2";
    case 2:   return "TLORA_V1";
    case 3:   return "TLORA_V2_1_1P6";
    case 4:   return "TBEAM";
    case 5:   return "HELTEC_V2_0";
    case 6:   return "TBEAM_V0P7";
    case 7:   return "T_ECHO";
    case 8:   return "TLORA_V1_1P3";
    case 9:   return "RAK4631";
    case 10:  return "HELTEC_V2_1";
    case 11:  return "HELTEC_V1";
    case 12:  return "TBEAM_S3_CORE";
    case 13:  return "RAK11200";
    case 14:  return "NANO_G1";
    case 15:  return "TLORA_V2_1_1P8";
    case 16:  return "TLORA_T3_S3";
    case 17:  return "NANO_G1_EXPLORER";
    case 18:  return "NANO_G2_ULTRA";
    case 25:  return "STATION_G1";
    case 26:  return "RAK11310";
    case 33:  return "T_ECHO_PLUS";
    case 37:  return "PORTDUINO";
    case 43:  return "HELTEC_V3";
    case 44:  return "HELTEC_WSL_V3";
    case 47:  return "RPI_PICO";
    case 48:  return "HELTEC_WIRELESS_TRACKER";
    case 49:  return "HELTEC_WIRELESS_PAPER";
    case 50:  return "T_DECK";
    case 51:  return "T_WATCH_S3";
    case 53:  return "HELTEC_HT62";
    case 65:  return "HELTEC_CAPSULE_SENSOR_V3";
    case 69:  return "HELTEC_MESH_NODE_T114";
    case 70:  return "SENSECAP_INDICATOR";
    case 71:  return "TRACKER_T1000_E";
    case 79:  return "RPI_PICO2";
    case 84:  return "WISMESH_TAP";
    case 89:  return "THINKNODE_M1";
    case 91:  return "T_ETH_ELITE";
    case 94:  return "HELTEC_MESH_POCKET";
    case 95:  return "SEEED_SOLAR_NODE";
    case 102: return "T_DECK_PRO";
    case 103: return "T_LORA_PAGER";
    case 108: return "HELTEC_MESH_SOLAR";
    case 109: return "T_ECHO_LITE";
    case 110: return "HELTEC_V4";
    case 113: return "HELTEC_WIRELESS_TRACKER_V2";
    case 114: return "T_WATCH_ULTRA";
    case 255: return "PRIVATE_HW";
    default:  return NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Frame and protobuf constants                                       */
/* ------------------------------------------------------------------ */

#define PN_MESH_FRAME_SYNC1 0x94
#define PN_MESH_FRAME_SYNC2 0xC3
#define PN_MESH_FRAME_MAX   512

/* Wire types per the protobuf spec. */
#define PN_PB_WT_VARINT  0
#define PN_PB_WT_FIXED64 1
#define PN_PB_WT_LEN     2
#define PN_PB_WT_FIXED32 5

/* Meshtastic portnum we care about (from PortNum.proto). */
#define PN_MESH_PORT_TEXT 1

/* Broadcast destination address. */
#define PN_MESH_BROADCAST 0xFFFFFFFFu

/* Handshake timeout for the short-lived list_channels helper and for
 * the worker's initial config request. */
#define PN_MESH_HANDSHAKE_TIMEOUT_MS 4000

/* Heartbeat interval for the worker's connection-keepalive frame.  The
 * Meshtastic firmware tracks "is the phone still attached" by watching
 * for ToRadio traffic and, after a silent period (the firmware-side
 * `phone_message_timeout_secs`, default ~15 min, but we have observed
 * the radio fall silent at ~1 h on real hardware too), drops the host
 * client and stops forwarding mesh packets up the serial link to save
 * power.  The official meshtastic-python and Android clients keep the
 * link alive by sending an empty `ToRadio.heartbeat` (field 7) every
 * five minutes; we mirror that here so a worksheet left running for
 * hours keeps emitting received text frames instead of going dark. */
#define PN_MESH_HEARTBEAT_INTERVAL_MS (5 * 60 * 1000)

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_MESHTASTIC_NORMAL_ICON  "\xef\x89\xba"  /* fa-commenting U+F27A */
#define PN_MESHTASTIC_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint     index;
    gchar   *name;   /* never NULL once installed */
    gint     role;
} PnMeshChannel;

/** Outbox entry: a text payload plus the channel name to send it on.
 *  The worker thread is what knows the (name -> index) mapping from
 *  the live session, so we defer frame construction until the
 *  worker pops the entry — this keeps the receive() path free of
 *  any handshake-state dependency. */
typedef struct
{
    gchar *text;
    gchar *channel_name;  /* may be empty for primary */
} PnMeshOutbox;

static void
mesh_outbox_free (gpointer data)
{
    PnMeshOutbox *o = data;
    g_free (o->text);
    g_free (o->channel_name);
    g_free (o);
}

struct _PnMeshtastic
{
    PnNode parent_instance;

    /* User-visible properties. */
    gchar *device;
    gchar *channel;

    /* Worker-thread state.  Shared fields use the mutex; the strv
     * cache uses the mutex for both publishers and readers. */
    GThread     *worker;
    GAsyncQueue *outbox;       /* PnMeshOutbox* — drained by worker */
    gint         stop;         /* g_atomic_int_set / get */
    int          wake_pipe[2]; /* {-1,-1} when the worker is not running */

    /* Cache of channel names captured during the worker's handshake
     * so the settings dialog can populate its channel combo without
     * having to re-open the (busy) device.  NULL until the handshake
     * succeeds. */
    GMutex   cache_mutex;
    gchar  **channels_cache;

    /* TRUE while the worker is opening the serial port and running
     * the initial config handshake — read by the settings dialog so
     * it can overlay a "busy" spinner and lock the form during the
     * multi-second device-init window.  Always toggled on the main
     * thread (worker side trampolines through set_busy_on_main). */
    gboolean busy;

    /* HardwareModel enum value captured from the local NodeInfo during
     * the worker's configuration handshake; 0 while no successful
     * handshake has completed yet.  Read by the settings dialog so the
     * status row can label the device with its symbolic model name
     * (e.g. "HELTEC_V3") once initialisation finishes.  Mutated only on
     * the main thread (worker side trampolines through
     * set_hw_model_on_main). */
    guint32 hw_model;

    /* Sticky record of the last device path that came back EBUSY from
     * serial_open().  The worker's EBUSY handler also resets `device`
     * to "" so the warning state and the "No Device" combo entry kick
     * back in — but the status row needs to keep displaying the
     * "Device /dev/foo is busy." line *after* that reset, otherwise
     * the user sees the selection silently snap back to "No Device"
     * with no explanation.  Cleared when the user picks a different
     * device, when the worker successfully finishes a handshake, and
     * when the worker is stopped — so a retry, a successful open, or
     * a disable all wipe the message.  Mutated only on the main thread
     * via set_last_busy_path_on_main. */
    gchar *last_busy_path;

    /* Sticky last-error string for any non-EBUSY worker failure (open
     * with ENOENT/ENXIO/EACCES, mid-stream POLLHUP, read/write errno).
     * Drives both the warning visual state and the dialog's status row
     * so a worksheet that loads with the configured device unplugged
     * shows the user *what* went wrong instead of leaving the node
     * looking healthy.  Cleared the same way last_busy_path is —
     * on a user-driven device change and on a successful handshake.
     * Mutated only on the main thread via set_last_error_on_main. */
    gchar *last_error;
};

enum
{
    PROP_0,
    PROP_DEVICE,
    PROP_CHANNEL,
    PROP_BUSY,
    PROP_HW_MODEL,
    PROP_LAST_BUSY_PATH,
    PROP_LAST_ERROR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE (PnMeshtastic, pn_meshtastic, PN_TYPE_NODE)

static void worker_stop_and_join (PnMeshtastic *self);
static void worker_start_if_ready (PnMeshtastic *self);
static void apply_visual_state   (PnMeshtastic *self, gboolean configured);

/* ------------------------------------------------------------------ */
/*  Protobuf encoder                                                   */
/* ------------------------------------------------------------------ */

static void
pb_encode_varint (GByteArray *out, guint64 value)
{
    while (value >= 0x80)
    {
        guint8 b = (value & 0x7Fu) | 0x80u;
        g_byte_array_append (out, &b, 1);
        value >>= 7;
    }
    {
        guint8 b = value & 0x7Fu;
        g_byte_array_append (out, &b, 1);
    }
}

static void
pb_encode_tag (GByteArray *out, guint field, guint wire_type)
{
    pb_encode_varint (out, ((guint64) field << 3) | wire_type);
}

static void
pb_encode_fixed32 (GByteArray *out, guint32 value)
{
    guint8 b[4];
    b[0] = (value >>  0) & 0xFFu;
    b[1] = (value >>  8) & 0xFFu;
    b[2] = (value >> 16) & 0xFFu;
    b[3] = (value >> 24) & 0xFFu;
    g_byte_array_append (out, b, 4);
}

static void
pb_encode_varint_field (GByteArray *out, guint field, guint64 value)
{
    pb_encode_tag    (out, field, PN_PB_WT_VARINT);
    pb_encode_varint (out, value);
}

static void
pb_encode_fixed32_field (GByteArray *out, guint field, guint32 value)
{
    pb_encode_tag     (out, field, PN_PB_WT_FIXED32);
    pb_encode_fixed32 (out, value);
}

static void
pb_encode_len_field (GByteArray   *out,
                     guint         field,
                     const guint8 *data,
                     gsize         len)
{
    pb_encode_tag    (out, field, PN_PB_WT_LEN);
    pb_encode_varint (out, len);
    if (len > 0)
        g_byte_array_append (out, data, len);
}

/* ------------------------------------------------------------------ */
/*  Protobuf decoder                                                   */
/*                                                                     */
/*  All decoders advance *pos and return %TRUE on success, %FALSE on   */
/*  truncation.  Callers cope with truncation by abandoning the rest  */
/*  of the message — Meshtastic frames are short and self-contained,  */
/*  a half-decoded one is just ignored.                                */
/* ------------------------------------------------------------------ */

static gboolean
pb_decode_varint (const guint8 *buf,
                  gsize         len,
                  gsize        *pos,
                  guint64      *out)
{
    guint64 v = 0;
    gint    shift = 0;
    gsize   p = *pos;

    while (p < len)
    {
        guint8 b = buf[p++];
        v |= ((guint64) (b & 0x7Fu)) << shift;
        if ((b & 0x80u) == 0)
        {
            *pos = p;
            *out = v;
            return TRUE;
        }
        shift += 7;
        if (shift > 63)
            return FALSE;
    }
    return FALSE;
}

static gboolean
pb_decode_fixed32 (const guint8 *buf,
                   gsize         len,
                   gsize        *pos,
                   guint32      *out)
{
    if (*pos + 4 > len)
        return FALSE;
    *out = ((guint32) buf[*pos    ])
         | ((guint32) buf[*pos + 1] <<  8)
         | ((guint32) buf[*pos + 2] << 16)
         | ((guint32) buf[*pos + 3] << 24);
    *pos += 4;
    return TRUE;
}

/** Skip an unknown field of @wire_type starting at *pos, advancing
 *  past its payload.  Returns %FALSE on truncation. */
static gboolean
pb_skip_field (const guint8 *buf,
               gsize         len,
               gsize        *pos,
               guint         wire_type)
{
    guint64 v;
    switch (wire_type)
    {
    case PN_PB_WT_VARINT:
        return pb_decode_varint (buf, len, pos, &v);
    case PN_PB_WT_FIXED64:
        if (*pos + 8 > len) return FALSE;
        *pos += 8;
        return TRUE;
    case PN_PB_WT_LEN:
        if (!pb_decode_varint (buf, len, pos, &v)) return FALSE;
        if (*pos + v > len) return FALSE;
        *pos += v;
        return TRUE;
    case PN_PB_WT_FIXED32:
        if (*pos + 4 > len) return FALSE;
        *pos += 4;
        return TRUE;
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Frame layer                                                        */
/* ------------------------------------------------------------------ */

/** Build a serial frame around @payload: 0x94 0xC3 BE16(len) <payload>.
 *  Returns a freshly-allocated #GBytes. */
static GBytes *
frame_wrap (const guint8 *payload, gsize len)
{
    guint8 *buf = g_malloc (len + 4);
    buf[0] = PN_MESH_FRAME_SYNC1;
    buf[1] = PN_MESH_FRAME_SYNC2;
    buf[2] = (len >> 8) & 0xFFu;
    buf[3] = (len     ) & 0xFFu;
    if (len > 0)
        memcpy (buf + 4, payload, len);
    return g_bytes_new_take (buf, len + 4);
}

/** Pull complete frames out of @rx in place.  For each frame the
 *  callback is invoked with the payload pointer and length; once the
 *  callback returns the frame's bytes are removed from the buffer.
 *  A partial trailing frame is left in place for the next call. */
typedef void (*PnFrameFn) (const guint8 *payload, gsize len, gpointer ud);

static void
frame_extract (GByteArray *rx, PnFrameFn cb, gpointer ud)
{
    gsize i = 0;
    while (i + 4 <= rx->len)
    {
        if (rx->data[i] != PN_MESH_FRAME_SYNC1)
        {
            i++;
            continue;
        }
        if (rx->data[i + 1] != PN_MESH_FRAME_SYNC2)
        {
            i++;
            continue;
        }
        guint16 plen = ((guint16) rx->data[i + 2] << 8)
                     | ((guint16) rx->data[i + 3]);
        if (plen == 0 || plen > PN_MESH_FRAME_MAX)
        {
            /* Bad length — desync.  Skip the sync byte and keep
             * scanning so we don't lose a real frame that happened
             * to follow garbage. */
            i++;
            continue;
        }
        if (i + 4 + plen > rx->len)
        {
            /* Frame still arriving; preserve it for the next read. */
            break;
        }
        cb (rx->data + i + 4, plen, ud);
        i += 4 + plen;
    }
    if (i > 0)
        g_byte_array_remove_range (rx, 0, i);
}

/* ------------------------------------------------------------------ */
/*  Outbound message construction                                      */
/* ------------------------------------------------------------------ */

/** Build a ToRadio { packet=MeshPacket { to=BCAST, channel=ch,
 *  decoded=Data { portnum=TEXT, payload=text } } } and wrap it in a
 *  serial frame.  Mirrors build_send_text_frame in pip-mesh. */
static GBytes *
build_text_frame (const gchar *text, guint channel_index)
{
    GByteArray *data_msg = g_byte_array_new ();
    GByteArray *mesh_pkt = g_byte_array_new ();
    GByteArray *to_radio = g_byte_array_new ();
    GBytes     *frame;

    /* Data { portnum=1, payload=<utf8 text> } */
    pb_encode_varint_field (data_msg, 1, PN_MESH_PORT_TEXT);
    pb_encode_len_field    (data_msg, 2,
                            (const guint8 *) text, strlen (text));

    /* MeshPacket { to=BCAST, channel=ch, decoded=Data } */
    pb_encode_fixed32_field (mesh_pkt, 2, PN_MESH_BROADCAST);
    pb_encode_varint_field  (mesh_pkt, 3, channel_index);
    pb_encode_len_field     (mesh_pkt, 4, data_msg->data, data_msg->len);

    /* ToRadio { packet=MeshPacket } */
    pb_encode_len_field (to_radio, 1, mesh_pkt->data, mesh_pkt->len);

    frame = frame_wrap (to_radio->data, to_radio->len);

    g_byte_array_free (data_msg, TRUE);
    g_byte_array_free (mesh_pkt, TRUE);
    g_byte_array_free (to_radio, TRUE);
    return frame;
}

/** Build a bare ToRadio { want_config_id=1 } frame to kick the device
 *  into streaming its configuration. */
static GBytes *
build_want_config_frame (void)
{
    GByteArray *to_radio = g_byte_array_new ();
    GBytes     *frame;

    /* ToRadio.want_config_id = field 3 varint = 1 */
    pb_encode_varint_field (to_radio, 3, 1);

    frame = frame_wrap (to_radio->data, to_radio->len);
    g_byte_array_free (to_radio, TRUE);
    return frame;
}

/** Build a ToRadio { heartbeat = Heartbeat{} } keepalive frame.  The
 *  Heartbeat message has no fields, so the encoded form is a single
 *  length-delimited record carrying an empty payload (field 7, wire
 *  type 2, length 0). */
static GBytes *
build_heartbeat_frame (void)
{
    GByteArray *to_radio = g_byte_array_new ();
    GBytes     *frame;

    /* ToRadio.heartbeat = field 7, length-delimited, zero bytes. */
    pb_encode_len_field (to_radio, 7, NULL, 0);

    frame = frame_wrap (to_radio->data, to_radio->len);
    g_byte_array_free (to_radio, TRUE);
    return frame;
}

/* ------------------------------------------------------------------ */
/*  FromRadio parsing                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Handshake collectors. */
    guint32     my_node_num;
    guint32     my_hw_model;    /* HardwareModel enum, 0 = unknown */
    GHashTable *node_names;     /* uint32 -> gchar* (heap) */
    GArray     *channels;       /* PnMeshChannel, name owned */
    gboolean    config_complete;
} PnMeshSession;

static void
mesh_session_init (PnMeshSession *s)
{
    s->my_node_num     = 0;
    s->my_hw_model     = 0;
    s->node_names      = g_hash_table_new_full (
            g_direct_hash, g_direct_equal, NULL, g_free);
    s->channels        = g_array_new (FALSE, FALSE, sizeof (PnMeshChannel));
    s->config_complete = FALSE;
}

static void
channel_clear (PnMeshChannel *c)
{
    g_clear_pointer (&c->name, g_free);
}

static void
mesh_session_clear (PnMeshSession *s)
{
    if (s->channels != NULL)
    {
        for (guint i = 0; i < s->channels->len; i++)
            channel_clear (&g_array_index (s->channels, PnMeshChannel, i));
        g_array_free (s->channels, TRUE);
        s->channels = NULL;
    }
    g_clear_pointer (&s->node_names, g_hash_table_unref);
}

static void
parse_user (const guint8 *buf,
            gsize         len,
            gchar       **out_long,
            guint32      *out_hw_model)
{
    gsize p = 0;
    *out_long     = NULL;
    *out_hw_model = 0;
    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);
        if (field == 2 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            if (p + v > len) return;
            g_free (*out_long);
            *out_long = g_strndup ((const gchar *) (buf + p), v);
            p += v;
        }
        else if (field == 5 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            *out_hw_model = (guint32) v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) return;
        }
    }
}

static void
parse_node_info (PnMeshSession *s, const guint8 *buf, gsize len)
{
    gsize    p = 0;
    guint32  num = 0;
    guint32  hw_model = 0;
    gchar   *long_name = NULL;

    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            goto done;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 1 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            num = (guint32) v;
        }
        else if (field == 2 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            if (p + v > len) goto done;
            parse_user (buf + p, v, &long_name, &hw_model);
            p += v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) goto done;
        }
    }

done:
    /* If this NodeInfo describes the local device (num matches the
     * my_node_num that arrived earlier in the same handshake), capture
     * the hardware-model enum so the settings dialog can label the
     * device with its symbolic model name once the spinner clears. */
    if (num != 0 && num == s->my_node_num && hw_model != 0)
        s->my_hw_model = hw_model;

    if (num != 0 && long_name != NULL)
    {
        g_hash_table_insert (s->node_names,
                             GUINT_TO_POINTER (num),
                             long_name);  /* hash takes ownership */
    }
    else
    {
        g_free (long_name);
    }
}

static void
parse_channel_settings (const guint8 *buf, gsize len, gchar **out_name)
{
    gsize p = 0;
    *out_name = NULL;
    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);
        if (field == 3 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            if (p + v > len) return;
            g_free (*out_name);
            *out_name = g_strndup ((const gchar *) (buf + p), v);
            p += v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) return;
        }
    }
}

static void
parse_channel (PnMeshSession *s, const guint8 *buf, gsize len)
{
    gsize          p = 0;
    PnMeshChannel  ch = { 0, NULL, 0 };

    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            goto done;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 1 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            ch.index = (gint) v;
        }
        else if (field == 2 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            if (p + v > len) goto done;
            parse_channel_settings (buf + p, v, &ch.name);
            p += v;
        }
        else if (field == 3 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            ch.role = (gint) v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) goto done;
        }
    }

done:
    /* role 0 = DISABLED, drop those.  Otherwise keep entries even if
     * the name is NULL (an empty string slot still occupies its
     * channel index). */
    if (ch.role == 0 && ch.name == NULL)
        return;
    if (ch.name == NULL)
        ch.name = g_strdup ("");
    g_array_append_val (s->channels, ch);
}

static void
parse_my_node_info (PnMeshSession *s, const guint8 *buf, gsize len)
{
    gsize p = 0;
    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);
        if (field == 1 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            s->my_node_num = (guint32) v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) return;
        }
    }
}

/* Decoded fields lifted from a TEXT_MESSAGE_APP MeshPacket. */
typedef struct
{
    gboolean has_text;
    guint32  from;
    guint32  to;
    guint32  channel;
    guint32  packet_id;
    guint32  rx_time;
    guint32  hop_limit;
    guint32  hop_start;
    gchar   *text;     /* heap, NUL-terminated */
} PnMeshDecodedText;

static void
parse_data (const guint8 *buf, gsize len, PnMeshDecodedText *out)
{
    gsize    p = 0;
    guint32  portnum = 0;
    gchar   *payload = NULL;

    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (buf, len, &p, &tag))
            goto done;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 1 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            portnum = (guint32) v;
        }
        else if (field == 2 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) goto done;
            if (p + v > len) goto done;
            g_free (payload);
            payload = g_strndup ((const gchar *) (buf + p), v);
            p += v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) goto done;
        }
    }

done:
    if (portnum == PN_MESH_PORT_TEXT && payload != NULL && *payload != '\0')
    {
        out->has_text = TRUE;
        out->text     = payload;
    }
    else
    {
        g_free (payload);
    }
}

/** Parse one MeshPacket payload, populating @out with text-message
 *  fields if the inner Data is TEXT_MESSAGE_APP. */
static void
parse_mesh_packet (const guint8 *buf, gsize len, PnMeshDecodedText *out)
{
    gsize p = 0;
    memset (out, 0, sizeof *out);

    while (p < len)
    {
        guint64 tag, v;
        guint32 v32;
        if (!pb_decode_varint (buf, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 1 && wt == PN_PB_WT_FIXED32)
        {
            if (!pb_decode_fixed32 (buf, len, &p, &v32)) return;
            out->from = v32;
        }
        else if (field == 2 && wt == PN_PB_WT_FIXED32)
        {
            if (!pb_decode_fixed32 (buf, len, &p, &v32)) return;
            out->to = v32;
        }
        else if (field == 3 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            out->channel = (guint32) v;
        }
        else if (field == 4 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            if (p + v > len) return;
            parse_data (buf + p, v, out);
            p += v;
        }
        else if (field == 6 && wt == PN_PB_WT_FIXED32)
        {
            if (!pb_decode_fixed32 (buf, len, &p, &v32)) return;
            out->packet_id = v32;
        }
        else if (field == 7 && wt == PN_PB_WT_FIXED32)
        {
            if (!pb_decode_fixed32 (buf, len, &p, &v32)) return;
            out->rx_time = v32;
        }
        else if (field == 10 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            out->hop_limit = (guint32) v;
        }
        else if (field == 13 && wt == PN_PB_WT_VARINT)
        {
            if (!pb_decode_varint (buf, len, &p, &v)) return;
            out->hop_start = (guint32) v;
        }
        else
        {
            if (!pb_skip_field (buf, len, &p, wt)) return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Serial port helpers                                                */
/* ------------------------------------------------------------------ */

/** Open @path with raw 115200 8N1, return the fd or -1 on error. */
static int
serial_open (const gchar *path)
{
    int fd = open (path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    /* Refuse the device if another process — or another node in this
     * one — already holds it.  flock(2) is advisory but honoured by
     * every modern serial peer (gpsd, chrony, minicom, cu, pyserial,
     * the Meshtastic CLI); TIOCEXCL hardens against tools that don't
     * flock at all (cat, echo, the legacy bash prototype) by making
     * future non-root open()s on the same tty return EBUSY.  Both are
     * released by the existing close(fd) paths. */
    if (flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        int saved = (errno == EWOULDBLOCK) ? EBUSY : errno;
        close (fd);
        errno = saved;
        return -1;
    }
    ioctl (fd, TIOCEXCL);

    struct termios t;
    if (tcgetattr (fd, &t) != 0)
    {
        close (fd);
        return -1;
    }
    cfmakeraw (&t);
    cfsetispeed (&t, B115200);
    cfsetospeed (&t, B115200);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_cflag &= ~CSIZE;
    t.c_cflag |= CS8;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr (fd, TCSANOW, &t) != 0)
    {
        close (fd);
        return -1;
    }
    /* Discard any junk left in the kernel buffer from a previous
     * session — typically the tail end of a config stream from the
     * last process to hold the port. */
    tcflush (fd, TCIOFLUSH);
    return fd;
}

/** Blocking write that retries on EAGAIN/EINTR until either all bytes
 *  are sent, an error occurs, or @deadline_ms (monotonic-wallclock
 *  milliseconds since some epoch; 0 = no timeout) elapses.  Returns
 *  the number of bytes actually written, or -1 on hard error. */
static gssize
serial_write_all (int fd, const guint8 *buf, gsize len, gint64 deadline_us)
{
    gsize written = 0;
    while (written < len)
    {
        ssize_t n = write (fd, buf + written, len - written);
        if (n > 0)
        {
            written += n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
        {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            int           timeout_ms = -1;
            if (deadline_us > 0)
            {
                gint64 now = g_get_monotonic_time ();
                if (now >= deadline_us)
                    return written;
                timeout_ms = (int) ((deadline_us - now) / 1000);
            }
            poll (&pfd, 1, timeout_ms);
            continue;
        }
        return -1;
    }
    return (gssize) written;
}

/* ------------------------------------------------------------------ */
/*  USB device enumeration                                             */
/* ------------------------------------------------------------------ */

/** Read a 4-byte hex sysfs attribute (e.g. /sys/.../idVendor) and
 *  return the trimmed lowercase value, or %NULL on read error. */
static gchar *
sysfs_read_id (const gchar *path)
{
    gchar  *contents = NULL;
    gsize   len      = 0;
    if (!g_file_get_contents (path, &contents, &len, NULL))
        return NULL;
    g_strstrip (contents);
    return contents;  /* already lowercase from kernel */
}

/** Walk @parent looking for a tty subdirectory whose entries name a
 *  device under /dev (e.g. ttyUSB0).  One level of nesting is
 *  searched (USB serial devices on this kernel typically expose
 *  <intf>/tty/ttyXXX, but a couple expose <intf>/<sub>/tty/ttyXXX).
 *  Returns "/dev/ttyXXX" or %NULL. */
static gchar *
find_tty_under (const gchar *parent)
{
    GDir *d = g_dir_open (parent, 0, NULL);
    if (d == NULL)
        return NULL;

    gchar *result = NULL;
    const gchar *name;
    while (result == NULL && (name = g_dir_read_name (d)) != NULL)
    {
        gchar *child = g_build_filename (parent, name, NULL);
        gchar *tty   = g_build_filename (child, "tty", NULL);
        GDir  *td    = g_dir_open (tty, 0, NULL);
        if (td != NULL)
        {
            const gchar *t;
            while ((t = g_dir_read_name (td)) != NULL)
            {
                gchar *candidate = g_build_filename ("/dev", t, NULL);
                if (g_file_test (candidate, G_FILE_TEST_EXISTS))
                {
                    result = candidate;
                    break;
                }
                g_free (candidate);
            }
            g_dir_close (td);
        }
        g_free (tty);
        if (result == NULL)
        {
            /* One nested level for devices like SenseCAP which expose
             * intf/sub/tty/ttyXXX. */
            GDir *sd = g_dir_open (child, 0, NULL);
            if (sd != NULL)
            {
                const gchar *sub;
                while (result == NULL && (sub = g_dir_read_name (sd)) != NULL)
                {
                    gchar *sub_path = g_build_filename (child, sub, NULL);
                    if (g_file_test (sub_path, G_FILE_TEST_IS_DIR))
                        result = find_tty_under (sub_path);
                    g_free (sub_path);
                }
                g_dir_close (sd);
            }
        }
        g_free (child);
    }
    g_dir_close (d);
    return result;
}

static gint
strv_sort_cmp (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

gchar **
pn_meshtastic_list_devices (void)
{
    GPtrArray *paths = g_ptr_array_new ();
    GDir      *root  = g_dir_open ("/sys/bus/usb/devices", 0, NULL);

    if (root != NULL)
    {
        const gchar *name;
        while ((name = g_dir_read_name (root)) != NULL)
        {
            gchar *base    = g_build_filename ("/sys/bus/usb/devices", name, NULL);
            gchar *vid_p   = g_build_filename (base, "idVendor",  NULL);
            gchar *pid_p   = g_build_filename (base, "idProduct", NULL);
            gchar *vid     = sysfs_read_id (vid_p);
            gchar *pid     = sysfs_read_id (pid_p);

            if (vid != NULL && pid != NULL)
            {
                gboolean match = FALSE;
                for (gsize i = 0; i < G_N_ELEMENTS (known_devices); i++)
                {
                    if (g_strcmp0 (vid, known_devices[i].vendor)  == 0 &&
                        g_strcmp0 (pid, known_devices[i].product) == 0)
                    {
                        match = TRUE;
                        break;
                    }
                }
                if (match)
                {
                    gchar *tty = find_tty_under (base);
                    if (tty != NULL)
                        g_ptr_array_add (paths, tty);
                }
            }
            g_free (vid);
            g_free (pid);
            g_free (vid_p);
            g_free (pid_p);
            g_free (base);
        }
        g_dir_close (root);
    }

    g_ptr_array_sort (paths, strv_sort_cmp);
    g_ptr_array_add  (paths, NULL);
    return (gchar **) g_ptr_array_free (paths, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Handshake                                                          */
/*                                                                     */
/*  Sends the want_config_id frame and reads from @fd until either     */
/*  config_complete_id arrives or @deadline_us passes.  Caller keeps  */
/*  ownership of @session — it ends up populated with whatever the    */
/*  device managed to send before the timeout.  Returns %TRUE if the  */
/*  config_complete sentinel was seen, %FALSE on timeout or read       */
/*  error (the partial session is still usable).                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnMeshSession *session;
} PnHandshakeCtx;

static void
handshake_on_frame (const guint8 *payload, gsize len, gpointer ud)
{
    PnHandshakeCtx *ctx = ud;
    PnMeshSession  *s   = ctx->session;
    gsize           p   = 0;

    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (payload, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 3 && wt == PN_PB_WT_LEN)              /* my_info */
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            if (p + v > len) return;
            parse_my_node_info (s, payload + p, v);
            p += v;
        }
        else if (field == 4 && wt == PN_PB_WT_LEN)         /* node_info */
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            if (p + v > len) return;
            parse_node_info (s, payload + p, v);
            p += v;
        }
        else if (field == 7 && wt == PN_PB_WT_VARINT)      /* config_complete */
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            s->config_complete = TRUE;
        }
        else if (field == 10 && wt == PN_PB_WT_LEN)        /* channel */
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            if (p + v > len) return;
            parse_channel (s, payload + p, v);
            p += v;
        }
        else
        {
            if (!pb_skip_field (payload, len, &p, wt)) return;
        }
    }
}

static gboolean
run_handshake (int fd, PnMeshSession *session, gint timeout_ms)
{
    /* Send the want_config_id kick. */
    GBytes *kick = build_want_config_frame ();
    gsize   klen;
    const guint8 *kdata = g_bytes_get_data (kick, &klen);
    gint64  send_deadline = g_get_monotonic_time () + 1 * G_TIME_SPAN_SECOND;
    if (serial_write_all (fd, kdata, klen, send_deadline) != (gssize) klen)
    {
        g_bytes_unref (kick);
        return FALSE;
    }
    g_bytes_unref (kick);

    /* Read until config_complete or timeout. */
    GByteArray *rx = g_byte_array_new ();
    PnHandshakeCtx ctx = { session };
    gint64 deadline = g_get_monotonic_time ()
                    + (gint64) timeout_ms * 1000;

    while (!session->config_complete)
    {
        gint64 now = g_get_monotonic_time ();
        if (now >= deadline)
            break;

        struct pollfd pfd = { fd, POLLIN, 0 };
        int           pms = (int) ((deadline - now) / 1000);
        if (pms < 1) pms = 1;

        int pr = poll (&pfd, 1, pms);
        if (pr <= 0)
            continue;

        guint8 buf[512];
        ssize_t n = read (fd, buf, sizeof buf);
        if (n > 0)
        {
            g_byte_array_append (rx, buf, n);
            frame_extract (rx, handshake_on_frame, &ctx);
        }
        else if (n == 0)
        {
            break;  /* EOF — port closed by other end */
        }
        else if (errno != EAGAIN && errno != EINTR)
        {
            break;
        }
    }
    g_byte_array_free (rx, TRUE);
    return session->config_complete;
}

/* ------------------------------------------------------------------ */
/*  pn_meshtastic_list_channels (public, dialog-side)                  */
/* ------------------------------------------------------------------ */

static gint
channel_index_cmp (gconstpointer a, gconstpointer b)
{
    const PnMeshChannel *ca = a;
    const PnMeshChannel *cb = b;
    return ca->index - cb->index;
}

gchar **
pn_meshtastic_list_channels (const gchar *device_path)
{
    if (device_path == NULL || *device_path == '\0')
        return NULL;

    int fd = serial_open (device_path);
    if (fd < 0)
        return NULL;

    PnMeshSession session;
    mesh_session_init (&session);
    run_handshake (fd, &session, PN_MESH_HANDSHAKE_TIMEOUT_MS);
    close (fd);

    if (session.channels == NULL || session.channels->len == 0)
    {
        mesh_session_clear (&session);
        return NULL;
    }

    g_array_sort (session.channels, channel_index_cmp);

    GPtrArray *names = g_ptr_array_new ();
    for (guint i = 0; i < session.channels->len; i++)
    {
        const PnMeshChannel *c = &g_array_index (session.channels,
                                                 PnMeshChannel, i);
        if (c->name != NULL && *c->name != '\0')
            g_ptr_array_add (names, g_strdup (c->name));
    }
    g_ptr_array_add (names, NULL);

    mesh_session_clear (&session);
    return (gchar **) g_ptr_array_free (names, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Worker thread                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnNode    *node;     /* strong ref */
    PnMessage *message;  /* strong ref */
} PnEmitClosure;

static gboolean
emit_on_main_trampoline (gpointer data)
{
    PnEmitClosure *c = data;
    pn_node_emit_message (c->node, c->message);
    return G_SOURCE_REMOVE;
}

static void
emit_closure_free (gpointer data)
{
    PnEmitClosure *c = data;
    g_object_unref (c->node);
    g_object_unref (c->message);
    g_free (c);
}

static void
emit_message_on_main (PnMeshtastic *self, PnMessage *message)
{
    PnEmitClosure *c = g_new0 (PnEmitClosure, 1);
    c->node    = g_object_ref (PN_NODE (self));
    c->message = message;  /* transfer */
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                emit_on_main_trampoline,
                                c,
                                emit_closure_free);
}

typedef struct
{
    PnMeshtastic *self;     /* strong ref */
    gchar        *path;     /* device path that failed */
} PnClearDeviceClosure;

static gboolean
clear_device_trampoline (gpointer data)
{
    PnClearDeviceClosure *c = data;
    /* Only clear if the user hasn't already moved on to a different
     * device — otherwise we'd clobber a fresh selection that the
     * property setter just installed while this idle was waiting to
     * run.  Going through g_object_set fires the PROP_DEVICE setter,
     * which flips the visual state to red ❗ and stops the
     * (now-exiting) worker via worker_restart. */
    if (g_strcmp0 (c->self->device, c->path) == 0)
        g_object_set (c->self, "device", "", NULL);
    return G_SOURCE_REMOVE;
}

static void
clear_device_closure_free (gpointer data)
{
    PnClearDeviceClosure *c = data;
    g_object_unref (c->self);
    g_free (c->path);
    g_free (c);
}

/** Schedule a main-thread reset of the `device` property to "" so the
 *  node returns to the unconfigured / warning state.  Called from the
 *  worker thread when the user-picked path turns out to be busy. */
static void
clear_device_on_main (PnMeshtastic *self)
{
    PnClearDeviceClosure *c = g_new0 (PnClearDeviceClosure, 1);
    c->self = g_object_ref (self);
    c->path = g_strdup (self->device != NULL ? self->device : "");
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                clear_device_trampoline,
                                c,
                                clear_device_closure_free);
}

typedef struct
{
    PnMeshtastic *self;     /* strong ref */
    gboolean      value;
} PnBusyClosure;

static gboolean
set_busy_trampoline (gpointer data)
{
    PnBusyClosure *c = data;
    if (c->self->busy != c->value)
    {
        c->self->busy = c->value;
        g_object_notify_by_pspec (G_OBJECT (c->self), props[PROP_BUSY]);
    }
    return G_SOURCE_REMOVE;
}

static void
busy_closure_free (gpointer data)
{
    PnBusyClosure *c = data;
    g_object_unref (c->self);
    g_free (c);
}

/** Toggle the `busy` notification flag from any thread.  When called
 *  on the main thread the change runs synchronously inline (g_main_
 *  context_invoke's documented short-circuit); from the worker
 *  thread it dispatches through the default main context so the
 *  dialog's notify handler always runs in the GTK thread. */
static void
set_busy_on_main (PnMeshtastic *self, gboolean value)
{
    PnBusyClosure *c = g_new0 (PnBusyClosure, 1);
    c->self  = g_object_ref (self);
    c->value = value;
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                set_busy_trampoline,
                                c,
                                busy_closure_free);
}

typedef struct
{
    PnMeshtastic *self;     /* strong ref */
    guint32       value;
} PnHwModelClosure;

static gboolean
set_hw_model_trampoline (gpointer data)
{
    PnHwModelClosure *c = data;
    if (c->self->hw_model != c->value)
    {
        c->self->hw_model = c->value;
        g_object_notify_by_pspec (G_OBJECT (c->self), props[PROP_HW_MODEL]);
    }
    return G_SOURCE_REMOVE;
}

static void
hw_model_closure_free (gpointer data)
{
    PnHwModelClosure *c = data;
    g_object_unref (c->self);
    g_free (c);
}

/** Push a freshly-handshaken HardwareModel enum value onto @self from
 *  any thread.  Mirrors the set_busy_on_main shape so the property
 *  notify always lands on the GTK main thread, and is idempotent: a
 *  call with the same value as the current property is a no-op. */
static void
set_hw_model_on_main (PnMeshtastic *self, guint32 value)
{
    PnHwModelClosure *c = g_new0 (PnHwModelClosure, 1);
    c->self  = g_object_ref (self);
    c->value = value;
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                set_hw_model_trampoline,
                                c,
                                hw_model_closure_free);
}

typedef struct
{
    PnMeshtastic *self;     /* strong ref */
    gchar        *value;    /* heap, may be NULL to clear */
} PnLastBusyPathClosure;

static gboolean
set_last_busy_path_trampoline (gpointer data)
{
    PnLastBusyPathClosure *c = data;
    if (g_strcmp0 (c->self->last_busy_path, c->value) != 0)
    {
        g_free (c->self->last_busy_path);
        c->self->last_busy_path = c->value != NULL ? g_strdup (c->value) : NULL;
        g_object_notify_by_pspec (G_OBJECT (c->self),
                                  props[PROP_LAST_BUSY_PATH]);
    }
    return G_SOURCE_REMOVE;
}

static void
last_busy_path_closure_free (gpointer data)
{
    PnLastBusyPathClosure *c = data;
    g_object_unref (c->self);
    g_free (c->value);
    g_free (c);
}

/** Record (or clear when @path is %NULL) the last device path that
 *  refused to open with EBUSY, so the settings dialog's status row
 *  can keep displaying the busy banner after the worker's EBUSY
 *  handler resets `device` to "".  Safe to call from any thread —
 *  notify always lands on the GTK main thread. */
static void
set_last_busy_path_on_main (PnMeshtastic *self, const gchar *path)
{
    PnLastBusyPathClosure *c = g_new0 (PnLastBusyPathClosure, 1);
    c->self  = g_object_ref (self);
    c->value = path != NULL ? g_strdup (path) : NULL;
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                set_last_busy_path_trampoline,
                                c,
                                last_busy_path_closure_free);
}

typedef struct
{
    PnMeshtastic *self;     /* strong ref */
    gchar        *value;    /* heap, may be NULL to clear */
} PnLastErrorClosure;

static gboolean
set_last_error_trampoline (gpointer data)
{
    PnLastErrorClosure *c = data;
    if (g_strcmp0 (c->self->last_error, c->value) != 0)
    {
        g_free (c->self->last_error);
        c->self->last_error = c->value != NULL ? g_strdup (c->value) : NULL;
        /* The visual state depends on the error flag, so refresh it
         * synchronously here before the property notify fires — that
         * way the node body has switched to red ❗ by the time the
         * status-row handler reads the new state. */
        apply_visual_state (c->self, *c->self->device != '\0');
        g_object_notify_by_pspec (G_OBJECT (c->self),
                                  props[PROP_LAST_ERROR]);
    }
    return G_SOURCE_REMOVE;
}

static void
last_error_closure_free (gpointer data)
{
    PnLastErrorClosure *c = data;
    g_object_unref (c->self);
    g_free (c->value);
    g_free (c);
}

/** Record (or clear when @reason is %NULL) the last worker-emitted
 *  error string so the settings dialog's status row can surface it
 *  and the node body can flip to the warning visual.  Safe to call
 *  from any thread — both the state mutation and the notify land on
 *  the GTK main thread. */
static void
set_last_error_on_main (PnMeshtastic *self, const gchar *reason)
{
    PnLastErrorClosure *c = g_new0 (PnLastErrorClosure, 1);
    c->self  = g_object_ref (self);
    c->value = reason != NULL ? g_strdup (reason) : NULL;
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                set_last_error_trampoline,
                                c,
                                last_error_closure_free);
}

/** Look up a node-num's display name in @session, or fall back to a
 *  formatted hex id string ("!a1b2c3d4"). */
static gchar *
session_format_sender (PnMeshSession *session, guint32 node_num,
                       gchar **out_id)
{
    *out_id = g_strdup_printf ("!%08x", node_num);
    const gchar *cached = NULL;
    if (session->node_names != NULL)
        cached = g_hash_table_lookup (session->node_names,
                                      GUINT_TO_POINTER (node_num));
    return cached != NULL ? g_strdup (cached) : g_strdup (*out_id);
}

/** Look up a channel name by index in @session. */
static const gchar *
session_channel_name (PnMeshSession *session, guint32 index)
{
    if (session->channels == NULL)
        return NULL;
    for (guint i = 0; i < session->channels->len; i++)
    {
        const PnMeshChannel *c = &g_array_index (session->channels,
                                                 PnMeshChannel, i);
        if ((guint32) c->index == index)
            return c->name;
    }
    return NULL;
}

/** Publish the freshly-handshaken channel name list onto self so the
 *  dialog can render the channel combobox without re-opening the
 *  port. */
static void
publish_channel_cache (PnMeshtastic *self, PnMeshSession *session)
{
    GPtrArray *names = g_ptr_array_new ();
    if (session->channels != NULL)
    {
        g_array_sort (session->channels, channel_index_cmp);
        for (guint i = 0; i < session->channels->len; i++)
        {
            const PnMeshChannel *c = &g_array_index (session->channels,
                                                     PnMeshChannel, i);
            if (c->name != NULL && *c->name != '\0')
                g_ptr_array_add (names, g_strdup (c->name));
        }
    }
    g_ptr_array_add (names, NULL);

    g_mutex_lock (&self->cache_mutex);
    g_strfreev (self->channels_cache);
    self->channels_cache = (gchar **) g_ptr_array_free (names, FALSE);
    g_mutex_unlock (&self->cache_mutex);
}

static void
emit_error_message (PnMeshtastic *self, const gchar *reason)
{
    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
    pn_message_set_boolean (msg, "success", FALSE);
    pn_message_set_string  (msg, "error",   reason);
    pn_message_set_string  (msg, "device",
                            self->device != NULL ? self->device : "");
    emit_message_on_main (self, msg);
    /* Mirror the outbound message into the sticky last_error slot so
     * the warning visual + dialog status row catch up.  EBUSY is
     * cosmetically routed through last_busy_path (which takes display
     * priority in update_status_label) and through a device-clear
     * that re-uses apply_visual_state's `configured == FALSE` branch,
     * so a duplicate "Device or resource busy" line in the status row
     * is suppressed there; the visual state side is harmless either
     * way. */
    set_last_error_on_main (self, reason);
}

typedef struct
{
    PnMeshtastic  *self;
    PnMeshSession *session;
} PnReadCtx;

static void
worker_on_frame (const guint8 *payload, gsize len, gpointer ud)
{
    PnReadCtx     *ctx = ud;
    PnMeshtastic  *self = ctx->self;
    PnMeshSession *session = ctx->session;

    /* Top level of FromRadio: pull either field 2 (mesh_packet) or
     * field 4 (node_info — keeps the name cache live as new nodes
     * announce themselves on air). */
    gsize p = 0;
    while (p < len)
    {
        guint64 tag, v;
        if (!pb_decode_varint (payload, len, &p, &tag))
            return;
        guint field = (guint) (tag >> 3);
        guint wt    = (guint) (tag & 0x7u);

        if (field == 2 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            if (p + v > len) return;
            PnMeshDecodedText d;
            parse_mesh_packet (payload + p, v, &d);
            p += v;
            if (!d.has_text)
                continue;

            /* Build the outbound PnMessage. */
            PnMessage *msg = pn_message_new (PN_NODE (self), NULL);
            pn_message_set_boolean (msg, "success", TRUE);
            pn_message_set_string  (msg, "output",  d.text);

            gchar *from_id = NULL;
            gchar *from_long = session_format_sender (session, d.from, &from_id);
            pn_message_set_string (msg, "from_node_id",   from_id);
            pn_message_set_string (msg, "from_long_name", from_long);
            g_free (from_id);
            g_free (from_long);

            pn_message_set_int (msg, "channel_index", (gint) d.channel);
            const gchar *cname = session_channel_name (session, d.channel);
            if (cname != NULL)
                pn_message_set_string (msg, "channel_name", cname);

            pn_message_set_int (msg, "packet_id", (gint) d.packet_id);
            pn_message_set_int (msg, "rx_time",   (gint) d.rx_time);
            pn_message_set_int (msg, "hop_limit", (gint) d.hop_limit);
            pn_message_set_int (msg, "hop_start", (gint) d.hop_start);
            pn_message_set_boolean (msg, "broadcast",
                                    d.to == PN_MESH_BROADCAST);

            emit_message_on_main (self, msg);
            g_free (d.text);
        }
        else if (field == 4 && wt == PN_PB_WT_LEN)
        {
            if (!pb_decode_varint (payload, len, &p, &v)) return;
            if (p + v > len) return;
            parse_node_info (session, payload + p, v);
            p += v;
        }
        else
        {
            if (!pb_skip_field (payload, len, &p, wt)) return;
        }
    }
}

static gpointer
worker_main (gpointer data)
{
    PnMeshtastic *self = data;

    int fd = serial_open (self->device);
    if (fd < 0)
    {
        int err = errno;
        emit_error_message (self, g_strerror (err));
        /* If the device is busy (held by another node in this process
         * or by an unrelated process / tool), clear the selection so
         * the node lands back in the warning state and the dialog's
         * combobox drops to "No Device" — the user has to actively
         * pick a different device or wait for the conflict to clear
         * and re-pick the same one.  Other open failures (ENOENT,
         * EACCES, …) are usually transient or fixable in place, so we
         * preserve the user's selection for those.  Capture the
         * busy path on the node *before* the clear so the dialog's
         * status row keeps showing "Device /dev/foo is busy." after
         * the auto-revert lands. */
        if (err == EBUSY)
        {
            set_last_busy_path_on_main (self, self->device);
            clear_device_on_main (self);
        }
        set_busy_on_main (self, FALSE);
        return NULL;
    }

    PnMeshSession session;
    mesh_session_init (&session);

    /* The handshake is best-effort: even if config_complete never
     * arrives we keep the partial session and run the read loop with
     * whatever channel list we did get.  A device that comes online
     * mid-handshake will still send text messages we can decode. */
    run_handshake (fd, &session, PN_MESH_HANDSHAKE_TIMEOUT_MS);
    publish_channel_cache (self, &session);
    /* Push the local NodeInfo's hw_model captured during the handshake
     * to the node's read-only `hw-model` property so the settings
     * dialog's status row can label the device with its symbolic model
     * name.  Stays at 0 if the handshake didn't see a NodeInfo for
     * my_node_num. */
    set_hw_model_on_main (self, session.my_hw_model);
    /* The port opened cleanly, so any prior EBUSY banner from this
     * node is now stale — clear it so the status row switches from
     * "Device X is busy." to the ready line.  Same logic applies to
     * any prior non-EBUSY error stashed in last_error: a fresh
     * handshake supersedes it, so the warning visual flips back to
     * teal and the dialog drops the error line. */
    set_last_busy_path_on_main (self, NULL);
    set_last_error_on_main     (self, NULL);
    /* Initialisation done — drop the dialog spinner.  From here on
     * the worker just shuttles frames between the kernel buffer and
     * the GAsyncQueue, which is not user-visible work. */
    set_busy_on_main (self, FALSE);

    GByteArray *rx  = g_byte_array_new ();
    PnReadCtx   ctx = { self, &session };

    /* Deadline (monotonic, microseconds) for the next outbound heartbeat
     * frame.  Reset whenever we send any ToRadio traffic — the firmware
     * doesn't care whether the keepalive is a Heartbeat or a real
     * payload, only that *some* host->radio frame arrived recently. */
    gint64 next_heartbeat_us = g_get_monotonic_time ()
                             + (gint64) PN_MESH_HEARTBEAT_INTERVAL_MS * 1000;

    while (!g_atomic_int_get (&self->stop))
    {
        struct pollfd pfds[2];
        pfds[0].fd = fd;
        pfds[0].events  = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = self->wake_pipe[0];
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        gint64 now_us       = g_get_monotonic_time ();
        gint64 remaining_us = next_heartbeat_us - now_us;
        int    timeout_ms   = (remaining_us <= 0)
                            ? 0
                            : (int) (remaining_us / 1000) + 1;

        int pr = poll (pfds, 2, timeout_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            emit_error_message (self, g_strerror (errno));
            break;
        }

        if (pr == 0 || g_get_monotonic_time () >= next_heartbeat_us)
        {
            GBytes       *frame = build_heartbeat_frame ();
            gsize         hlen;
            const guint8 *hbuf  = g_bytes_get_data (frame, &hlen);
            gint64        deadline = g_get_monotonic_time ()
                                   + 2 * G_TIME_SPAN_SECOND;
            if (serial_write_all (fd, hbuf, hlen, deadline) < 0)
            {
                emit_error_message (self, g_strerror (errno));
                g_bytes_unref (frame);
                break;
            }
            g_bytes_unref (frame);
            next_heartbeat_us = g_get_monotonic_time ()
                              + (gint64) PN_MESH_HEARTBEAT_INTERVAL_MS * 1000;
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            emit_error_message (self, "serial port hung up");
            break;
        }

        if (pfds[0].revents & POLLIN)
        {
            guint8 buf[512];
            ssize_t n = read (fd, buf, sizeof buf);
            if (n > 0)
            {
                g_byte_array_append (rx, buf, n);
                frame_extract (rx, worker_on_frame, &ctx);
            }
            else if (n == 0)
            {
                emit_error_message (self, "device disconnected");
                break;
            }
            else if (errno != EAGAIN && errno != EINTR)
            {
                emit_error_message (self, g_strerror (errno));
                break;
            }
        }

        if (pfds[1].revents & POLLIN)
        {
            char drain[64];
            while (read (self->wake_pipe[0], drain, sizeof drain) > 0)
                ;

            if (g_atomic_int_get (&self->stop))
                break;

            PnMeshOutbox *out;
            while ((out = g_async_queue_try_pop (self->outbox)) != NULL)
            {
                guint ch_index = 0;
                if (out->channel_name != NULL && *out->channel_name != '\0')
                {
                    /* Resolve against the live session so a non-
                     * contiguous channel layout (gaps in the index
                     * range) still hits the right slot. */
                    if (session.channels != NULL)
                    {
                        for (guint i = 0; i < session.channels->len; i++)
                        {
                            const PnMeshChannel *c = &g_array_index (
                                    session.channels, PnMeshChannel, i);
                            if (c->name != NULL &&
                                g_strcmp0 (c->name, out->channel_name) == 0)
                            {
                                ch_index = (guint) c->index;
                                break;
                            }
                        }
                    }
                }

                GBytes       *frame = build_text_frame (out->text, ch_index);
                gsize         olen;
                const guint8 *obuf = g_bytes_get_data (frame, &olen);
                gint64        deadline = g_get_monotonic_time ()
                                       + 2 * G_TIME_SPAN_SECOND;
                if (serial_write_all (fd, obuf, olen, deadline) < 0)
                    emit_error_message (self, g_strerror (errno));
                g_bytes_unref (frame);
                mesh_outbox_free (out);
                /* The outbound text frame doubles as a keepalive — push
                 * the next heartbeat deadline forward so we don't waste
                 * a redundant frame right after a user-initiated send. */
                next_heartbeat_us = g_get_monotonic_time ()
                                  + (gint64) PN_MESH_HEARTBEAT_INTERVAL_MS * 1000;
            }
        }
    }

    g_byte_array_free (rx, TRUE);
    mesh_session_clear (&session);
    close (fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Worker lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
worker_start_if_ready (PnMeshtastic *self)
{
    if (self->worker != NULL)
        return;
    if (pn_node_get_disabled (PN_NODE (self)))
        return;
    if (self->device == NULL || *self->device == '\0')
        return;
    /* channel may be empty — that resolves to the primary channel. */

    if (pipe (self->wake_pipe) != 0)
    {
        emit_error_message (self, g_strerror (errno));
        self->wake_pipe[0] = -1;
        self->wake_pipe[1] = -1;
        return;
    }
    /* Make both ends non-blocking — the worker's read drain loop
     * relies on that, and the writer side never wants to block on a
     * full pipe (we only push one byte per outbox push). */
    fcntl (self->wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl (self->wake_pipe[1], F_SETFL, O_NONBLOCK);

    g_atomic_int_set (&self->stop, 0);
    /* Mark busy *before* spawning so a dialog already on screen
     * sees the spinner immediately, without racing the worker for
     * the first set_busy_on_main delivery. */
    set_busy_on_main (self, TRUE);
    self->worker = g_thread_new ("pn-meshtastic", worker_main, self);
}

static void
worker_stop_and_join (PnMeshtastic *self)
{
    if (self->worker == NULL)
        return;

    g_atomic_int_set (&self->stop, 1);
    if (self->wake_pipe[1] >= 0)
    {
        char x = 'x';
        ssize_t r = write (self->wake_pipe[1], &x, 1);
        (void) r;
    }
    g_thread_join (self->worker);
    self->worker = NULL;

    /* Cleared synchronously here too: covers the "user disabled the
     * node while the handshake was still running" case where the
     * worker exits before its own done-handshake set_busy(FALSE) had
     * a chance to dispatch. */
    set_busy_on_main (self, FALSE);

    if (self->wake_pipe[0] >= 0) { close (self->wake_pipe[0]); self->wake_pipe[0] = -1; }
    if (self->wake_pipe[1] >= 0) { close (self->wake_pipe[1]); self->wake_pipe[1] = -1; }

    /* Drain leftover outbox entries (the worker exited before
     * sending them). */
    PnMeshOutbox *b;
    while ((b = g_async_queue_try_pop (self->outbox)) != NULL)
        mesh_outbox_free (b);

    /* Clear the channel cache: stale once the worker is gone. */
    g_mutex_lock (&self->cache_mutex);
    g_clear_pointer (&self->channels_cache, g_strfreev);
    g_mutex_unlock (&self->cache_mutex);

    /* Clear the cached hw_model for the same reason: the next worker
     * spin-up runs a fresh handshake against the (possibly different)
     * device, so a stale value would mis-label the status row. */
    set_hw_model_on_main (self, 0);
}

static void
worker_restart (PnMeshtastic *self)
{
    worker_stop_and_join (self);
    worker_start_if_ready (self);
}

/* ------------------------------------------------------------------ */
/*  receive() vfunc                                                    */
/* ------------------------------------------------------------------ */

static void
pn_meshtastic_receive (PnNode *node, PnMessage *message)
{
    PnMeshtastic *self = PN_MESHTASTIC (node);
    JsonObject   *data = pn_message_get_data (message);

    if (data == NULL || !json_object_has_member (data, "output"))
        return;

    JsonNode *n = json_object_get_member (data, "output");
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return;

    const gchar *text = json_node_get_string (n);
    if (text == NULL || *text == '\0')
        return;

    if (self->worker == NULL)
        return;  /* not connected — silently drop */

    PnMeshOutbox *out = g_new0 (PnMeshOutbox, 1);
    out->text         = g_strdup (text);
    out->channel_name = g_strdup (self->channel != NULL ? self->channel : "");
    g_async_queue_push (self->outbox, out);

    if (self->wake_pipe[1] >= 0)
    {
        char x = 'x';
        ssize_t r = write (self->wake_pipe[1], &x, 1);
        (void) r;
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_meshtastic_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
    PnMeshtastic *self = PN_MESHTASTIC (object);
    switch (prop_id)
    {
    case PROP_DEVICE:
        g_value_set_string (value, self->device);
        break;
    case PROP_CHANNEL:
        g_value_set_string (value, self->channel);
        break;
    case PROP_BUSY:
        g_value_set_boolean (value, self->busy);
        break;
    case PROP_HW_MODEL:
        g_value_set_uint (value, self->hw_model);
        break;
    case PROP_LAST_BUSY_PATH:
        g_value_set_string (value, self->last_busy_path);
        break;
    case PROP_LAST_ERROR:
        g_value_set_string (value, self->last_error);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/** Flip the node body between the configured (teal tower) and
 *  unconfigured (red ❗) appearance based on whether @configured —
 *  i.e. whether the user has picked a device path — *and* whether
 *  the worker has reported a sticky error.  A configured node whose
 *  serial port refused to open (ENOENT after the user unplugged the
 *  USB cable, ENXIO after a kernel-side hangup) keeps `device` set
 *  so a reconnect picks the same port back up automatically, but
 *  the body still needs to read as a warning until the user fixes
 *  the underlying problem.  An empty channel is a valid
 *  configuration (the worker falls back to primary index 0), so
 *  only the device path drives the ready half of this. */
static void
apply_visual_state (
        PnMeshtastic *self,
        gboolean      configured)
{
    PnNode *node = PN_NODE (self);

    if (configured && self->last_error == NULL)
    {
        GdkRGBA teal = { 0.30, 0.66, 0.55, 1.0 };
        pn_node_set_color (node, &teal);
        pn_node_set_icon  (node, PN_MESHTASTIC_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_MESHTASTIC_WARNING_ICON);
    }
}

static void
pn_meshtastic_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
    PnMeshtastic *self = PN_MESHTASTIC (object);
    const gchar  *new_value;
    switch (prop_id)
    {
    case PROP_DEVICE:
        new_value = g_value_get_string (value);
        if (g_strcmp0 (self->device, new_value) != 0)
        {
            /* A real user pick (non-empty new path) wipes the sticky
             * busy banner from any earlier EBUSY attempt — so the
             * status row resets while the new attempt runs.  The
             * EBUSY auto-clear path comes through here too, with
             * new_value="", and intentionally leaves the banner alone
             * so it stays on screen after the combo snaps back to
             * "No Device".  The non-EBUSY last_error banner is reset
             * symmetrically so the new device gets a clean slate. */
            if (new_value != NULL && *new_value != '\0')
            {
                set_last_busy_path_on_main (self, NULL);
                set_last_error_on_main     (self, NULL);
            }
            g_free (self->device);
            self->device = g_strdup (new_value != NULL ? new_value : "");
            apply_visual_state (self, *self->device != '\0');
            g_object_notify_by_pspec (object, props[PROP_DEVICE]);
            worker_restart (self);
        }
        break;
    case PROP_CHANNEL:
        new_value = g_value_get_string (value);
        if (g_strcmp0 (self->channel, new_value) != 0)
        {
            g_free (self->channel);
            self->channel = g_strdup (new_value != NULL ? new_value : "");
            g_object_notify_by_pspec (object, props[PROP_CHANNEL]);
            worker_restart (self);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
on_disabled_notify (GObject    *obj,
                    GParamSpec *pspec G_GNUC_UNUSED,
                    gpointer    ud   G_GNUC_UNUSED)
{
    PnMeshtastic *self = PN_MESHTASTIC (obj);
    if (pn_node_get_disabled (PN_NODE (self)))
        worker_stop_and_join (self);
    else
        worker_start_if_ready (self);
}

static void
pn_meshtastic_dispose (GObject *object)
{
    PnMeshtastic *self = PN_MESHTASTIC (object);
    worker_stop_and_join (self);
    G_OBJECT_CLASS (pn_meshtastic_parent_class)->dispose (object);
}

static void
pn_meshtastic_finalize (GObject *object)
{
    PnMeshtastic *self = PN_MESHTASTIC (object);

    g_clear_pointer (&self->device,         g_free);
    g_clear_pointer (&self->channel,        g_free);
    g_clear_pointer (&self->channels_cache, g_strfreev);
    g_clear_pointer (&self->last_busy_path, g_free);
    g_clear_pointer (&self->last_error,     g_free);
    g_mutex_clear (&self->cache_mutex);

    if (self->outbox != NULL)
    {
        PnMeshOutbox *b;
        while ((b = g_async_queue_try_pop (self->outbox)) != NULL)
            mesh_outbox_free (b);
        g_async_queue_unref (self->outbox);
    }

    G_OBJECT_CLASS (pn_meshtastic_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_property_editor override        */
/*                                                                     */
/*  Two of #PnMeshtastic's three properties (`device`, `channel`)      */
/*  need a richer editor than the introspection-driven default would   */
/*  give them: the device path is a serial-port path that should pick  */
/*  from the live USB bus enumeration, and the channel name is a      */
/*  label whose valid set depends on which device the user just       */
/*  picked AND on the worker handshake having published the channel   */
/*  cache.  Pre-2.0 the host's pn-node-dialog.c carried these as       */
/*  PN_IS_MESHTASTIC special-case branches; the per-class vfunc lets   */
/*  the same code live next to the rest of #PnMeshtastic so a future   */
/*  board-table edit / handshake protocol bump touches one file       */
/*  instead of two and an out-of-tree node type with the same         */
/*  device-then-channel shape can model itself after this without      */
/*  patching libpipnode.                                               */
/* ------------------------------------------------------------------ */

static void
meshtastic_channel_repopulate (GObject         *target,
                               GtkComboBoxText *combo)
{
    PnMeshtastic         *node    = PN_MESHTASTIC (target);
    gchar                *device  = NULL;
    gchar                *current = NULL;
    gchar               **fallback = NULL;
    const gchar * const  *cached;
    const gchar * const  *list;
    gboolean              listed  = FALSE;

    g_object_get (target, "device", &device,  NULL);
    g_object_get (target, "channel", &current, NULL);

    /* Snapshot the user's current selection so a remove_all() does
     * not lose it before the new list arrives. */
    gtk_combo_box_text_remove_all (combo);

    cached = pn_meshtastic_get_channel_cache (node);
    if (cached != NULL && cached[0] != NULL)
    {
        list = cached;
    }
    else if (device != NULL && *device != '\0')
    {
        fallback = pn_meshtastic_list_channels (device);
        list     = (const gchar * const *) fallback;
    }
    else
    {
        list = NULL;
    }

    if (list != NULL)
    {
        for (gsize i = 0; list[i] != NULL; i++)
        {
            gtk_combo_box_text_append (combo, list[i], list[i]);
            if (g_strcmp0 (list[i], current) == 0)
                listed = TRUE;
        }
    }

    if (!listed && current != NULL && *current != '\0')
        gtk_combo_box_text_append (combo, current, current);

    /* Reapply the bound value: gtk_combo_box_text_remove_all() drops
     * the active-id, but the property still holds the user's choice
     * so push it back into the combo so the visual stays in sync. */
    if (current != NULL && *current != '\0')
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), current);

    g_strfreev (fallback);
    g_free (device);
    g_free (current);
}

static void
on_meshtastic_device_notify (GObject    *target,
                             GParamSpec *pspec G_GNUC_UNUSED,
                             gpointer    user_data)
{
    meshtastic_channel_repopulate (target, GTK_COMBO_BOX_TEXT (user_data));
}

static GtkWidget *
pn_meshtastic_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                                     GParamSpec  *pspec,
                                     GObject     *target,
                                     GtkWindow   *parent    G_GNUC_UNUSED)
{
    const gchar  *name     = pspec->name;
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);

    /* Device path: combo populated from pn_meshtastic_list_devices()
     * (a sysfs walk for known vendor/product IDs).  The currently-
     * configured value is added to the list verbatim if it isn't
     * auto-detected so a previously-configured device whose USB cable
     * is currently unplugged still appears as the selection. */
    if (g_strcmp0 (name, "device") == 0)
    {
        GtkWidget   *combo   = gtk_combo_box_text_new ();
        gchar      **devices = pn_meshtastic_list_devices ();
        gchar       *current = NULL;
        gboolean     listed  = FALSE;

        g_object_get (target, name, &current, NULL);

        /* Sentinel "No Device" entry whose id is the empty string,
         * so the active-id binding selects it both for a fresh
         * unconfigured node and for a node whose worker just
         * cleared the device after an EBUSY conflict. */
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "", "No Device");

        for (gchar **p = devices; p != NULL && *p != NULL; p++)
        {
            gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                       *p, *p);
            if (g_strcmp0 (*p, current) == 0)
                listed = TRUE;
        }
        if (!listed && current != NULL && *current != '\0')
            gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                       current, current);

        g_strfreev (devices);
        g_free (current);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    /* Channel name: combo populated from the worker-side cache when
     * the node is connected, falling back to a fresh handshake
     * against the configured device path when it isn't.  Repopulates
     * in place when the device combo changes so the user sees the
     * list update without having to reopen the dialog. */
    if (g_strcmp0 (name, "channel") == 0)
    {
        GtkWidget *combo = gtk_combo_box_text_new ();

        meshtastic_channel_repopulate (target,
                                       GTK_COMBO_BOX_TEXT (combo));

        /* Refresh the channel list whenever the user picks a
         * different device, AND whenever the worker's busy flag
         * transitions — the latter is the moment the worker just
         * finished publishing the channel cache after a fresh
         * handshake, so this is what makes the combo go from
         * empty (or stale) to fully populated as soon as the
         * "Initializing device…" spinner clears.  Tied to the
         * combo's lifetime via g_signal_connect_object so the
         * dialog can close without leaking the handlers. */
        g_signal_connect_object (target, "notify::device",
                                 G_CALLBACK (on_meshtastic_device_notify),
                                 combo, 0);
        g_signal_connect_object (target, "notify::busy",
                                 G_CALLBACK (on_meshtastic_device_notify),
                                 combo, 0);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    /* Any other property — currently none, but kept open for future
     * additions — falls back to the host default. */
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: status row                                        */
/*                                                                     */
/*  Layered on top of build_class_tab below.  The dialog's auto-       */
/*  generated tab would show only the two writable properties (device, */
/*  channel) — but the worker also publishes runtime state (busy,      */
/*  hw_model) that the user wants surfaced inline so the act of        */
/*  picking a device is visibly a two-step transaction: a red "Device  */
/*  /dev/ttyUSB0 is busy." while the worker handshakes, then a calmer  */
/*  "HELTEC_V3 is ready." once the model name comes back from the      */
/*  local NodeInfo.  The label markup is recomputed on every relevant  */
/*  notify, so the row also clears itself when the user picks "No      */
/*  Device" or when a worker stop wipes the cached hw_model.           */
/* ------------------------------------------------------------------ */

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    PnMeshtastic *self  = PN_MESHTASTIC (obj);
    GtkLabel     *label = GTK_LABEL (user_data);
    gchar        *markup;

    /* The sticky banner takes priority over both `busy` and the
     * (possibly just-cleared) `device` field — the worker sets it
     * right before it asks the main thread to revert the selection
     * to "No Device" after an EBUSY, and clears it itself once the
     * user picks a different device or a retry succeeds.  Without
     * this branch the "Device X is busy." line would vanish at the
     * exact moment it's most useful (the auto-revert moment), which
     * is what the user reported. */
    if (self->last_busy_path != NULL && *self->last_busy_path != '\0')
    {
        gchar *escaped = g_markup_escape_text (self->last_busy_path, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s is busy.</span>",
                escaped);
        g_free (escaped);
    }
    else if (self->last_error != NULL && *self->last_error != '\0')
    {
        /* Non-EBUSY failure: surface the errno-derived reason verbatim
         * (e.g. "No such file or directory" when the user unplugged the
         * cable before loading the worksheet) alongside the device path
         * so the user knows which port the message is about. */
        const gchar *path = (self->device != NULL && *self->device != '\0')
                            ? self->device
                            : "(none)";
        gchar *epath = g_markup_escape_text (path, -1);
        gchar *erez  = g_markup_escape_text (self->last_error, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s: %s</span>",
                epath, erez);
        g_free (epath);
        g_free (erez);
    }
    else if (self->busy)
    {
        const gchar *path = (self->device != NULL && *self->device != '\0')
                            ? self->device
                            : "(none)";
        gchar *escaped = g_markup_escape_text (path, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s is busy.</span>",
                escaped);
        g_free (escaped);
    }
    else if (self->hw_model != 0)
    {
        const gchar *name = format_hw_model (self->hw_model);
        if (name != NULL)
        {
            markup = g_strdup_printf ("%s is ready.", name);
        }
        else
        {
            /* Unknown enum value — fall back to the integer so a new
             * board added on the device side before the format table
             * here is updated still produces a meaningful line. */
            markup = g_strdup_printf ("hw_model=%u is ready.",
                                      (guint) self->hw_model);
        }
    }
    else
    {
        markup = g_strdup ("");
    }

    gtk_label_set_markup (label, markup);
    g_free (markup);
}

static GtkWidget *
pn_meshtastic_build_class_tab (PnNode    *self,
                               GtkWindow *parent)
{
    GObject   *target = G_OBJECT (self);
    GtkWidget *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget *device_editor;
    GtkWidget *channel_editor;
    GtkWidget *status_label;

    /* Recreate the two writable rows the introspection-driven default
     * tab would have built — routed through our own build_property_editor
     * so device and channel keep their custom combos.  Keeping the
     * order (device first, channel second) matches what users have
     * been seeing since the property tab landed. */
    device_editor = pn_meshtastic_build_property_editor (
            self, props[PROP_DEVICE], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (props[PROP_DEVICE]),
                               device_editor);

    channel_editor = pn_meshtastic_build_property_editor (
            self, props[PROP_CHANNEL], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (props[PROP_CHANNEL]),
                               channel_editor);

    status_label = gtk_label_new (NULL);
    gtk_label_set_xalign     (GTK_LABEL (status_label), 0.0);
    gtk_label_set_use_markup (GTK_LABEL (status_label), TRUE);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2, "Status", status_label);

    /* Seed once and then keep in sync with every relevant runtime
     * change.  g_signal_connect_object ties the handler lifetime to
     * the label widget so the dialog can close without leaking the
     * connections. */
    update_status_label (target, NULL, status_label);
    g_signal_connect_object (target, "notify::busy",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::device",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::hw-model",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::last-busy-path",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::last-error",
                             G_CALLBACK (update_status_label),
                             status_label, 0);

    return grid;
}

static void
pn_meshtastic_class_init (PnMeshtasticClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_meshtastic_get_property;
    object_class->set_property = pn_meshtastic_set_property;
    object_class->dispose      = pn_meshtastic_dispose;
    object_class->finalize     = pn_meshtastic_finalize;
    node_class->receive        = pn_meshtastic_receive;

    node_class->build_property_editor = pn_meshtastic_build_property_editor;
    node_class->build_class_tab       = pn_meshtastic_build_class_tab;

    node_class->class_name     = "Meshtastic";
    node_class->icon           = "\xef\x89\xba";  /* fa-commenting U+F27A */
    node_class->color          = (GdkRGBA){ 0.30, 0.66, 0.55, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Path of the USB-attached Meshtastic device (e.g. /dev/ttyUSB0)",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_CHANNEL] = g_param_spec_string (
            "channel", "Channel",
            "Name of the Meshtastic channel to send and receive on",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Read-only at the property level so the introspection-driven
     * JSON serialiser (lib/pn-flow.c::should_serialize_property)
     * skips it — busy is transient runtime state, not part of the
     * saved flow.  Internal mutation goes through set_busy_on_main(),
     * which writes the field and fires notify directly. */
    props[PROP_BUSY] = g_param_spec_boolean (
            "busy", "Busy",
            "TRUE while the worker is opening and configuring the device",
            FALSE,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /* Same shape as PROP_BUSY: read-only runtime state, populated by
     * the worker once its handshake captures the local NodeInfo's
     * hw_model.  format_hw_model() turns the enum into the symbolic
     * name the settings dialog renders in the status row. */
    props[PROP_HW_MODEL] = g_param_spec_uint (
            "hw-model", "Hardware Model",
            "HardwareModel enum captured from the handshake (0 = unknown)",
            0, G_MAXUINT32, 0,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /* Sticky last-EBUSY device path; cleared when the user picks a
     * different device or when a fresh open succeeds.  Read-only so
     * the JSON serialiser skips it — like busy and hw-model, it's
     * runtime state not part of the saved flow. */
    props[PROP_LAST_BUSY_PATH] = g_param_spec_string (
            "last-busy-path", "Last Busy Path",
            "Device path that last failed to open with EBUSY, or empty",
            NULL,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /* Sticky non-EBUSY worker error.  Read-only so the JSON serialiser
     * skips it — runtime state only.  NULL when the worker hasn't
     * reported a failure or when a successful handshake / device
     * change has cleared the prior error. */
    props[PROP_LAST_ERROR] = g_param_spec_string (
            "last-error", "Last Error",
            "Last sticky worker error string (open/read/write failure), or empty",
            NULL,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_meshtastic_init (PnMeshtastic *self)
{
    PnNode *node = PN_NODE (self);

    self->device         = g_strdup ("");
    self->channel        = g_strdup ("");
    self->worker         = NULL;
    self->outbox         = g_async_queue_new_full (mesh_outbox_free);
    self->wake_pipe[0]   = -1;
    self->wake_pipe[1]   = -1;
    self->channels_cache = NULL;
    self->hw_model       = 0;
    self->last_busy_path = NULL;
    self->last_error     = NULL;
    g_mutex_init (&self->cache_mutex);

    pn_node_set_class_name (node, "Meshtastic");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Fresh node has no device picked yet — start in the warning
     * state.  PROP_DEVICE's setter flips us back to teal once the
     * user (or a save-file load) supplies a non-empty path. */
    apply_visual_state (self, FALSE);

    /* React to the disabled toggle by stopping/starting the worker. */
    g_signal_connect (self, "notify::disabled",
                      G_CALLBACK (on_disabled_notify), NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMeshtastic *
pn_meshtastic_new (void)
{
    return g_object_new (PN_TYPE_MESHTASTIC, NULL);
}

const gchar * const *
pn_meshtastic_get_channel_cache (PnMeshtastic *self)
{
    g_return_val_if_fail (PN_IS_MESHTASTIC (self), NULL);
    /* The cache is rewritten under the mutex, but the strv buffer it
     * points to is allocated once and never mutated in place — so a
     * caller reading the pointer under the mutex sees a coherent
     * snapshot.  The dialog code is expected to call this on the GTK
     * main thread which is also where worker_stop_and_join() runs,
     * so the snapshot stays valid for the dialog's lifetime. */
    const gchar * const *out;
    g_mutex_lock (&self->cache_mutex);
    out = (const gchar * const *) self->channels_cache;
    g_mutex_unlock (&self->cache_mutex);
    return out;
}
