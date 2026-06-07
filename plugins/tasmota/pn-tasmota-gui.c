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
/*  Tasmota Devices dialog — GUI companion for the tasmota plugin.     */
/*                                                                     */
/*  Built as pipnode_tasmota-gui.so, the sibling of the GTK-free logic */
/*  module pipnode_tasmota.so (TODO #23 two-tier layout, TODO #46).    */
/*  The editor loads the logic .so first (registering every Tasmota    */
/*  #GType), then finds this companion next to it and calls            */
/*  pn_plugin_gui_init(), which advertises a "Tasmota Devices" entry in */
/*  the editor's Devices menu via the provider registry               */
/*  (pn-device-provider.h).  pipnode-run never loads a companion, so   */
/*  the logic runs headless while the editor still shows this dialog.  */
/*                                                                     */
/*  Activating the entry opens a small dialog (the reusable            */
/*  device-dialog shell + form helpers) with a combobox of the         */
/*  available MQTT Broker credential profiles -- the same set the MQTT */
/*  Source node offers.  When the user picks a broker and presses      */
/*  Connect, we spin up a *hidden* MQTT Source (pn_mqtt_new(), not on  */
/*  any worksheet) subscribed to "#" plus a hidden MQTT Sink           */
/*  (pn_mqtt_sink_new()) for the discovery polls, and replicate the    */
/*  PnTasmotaProbe + PnTasmotaConcentrator discovery the worksheet     */
/*  nodes do: every device first seen on any tele/stat/cmnd topic is   */
/*  polled with `cmnd/<device>/Status 0`, and its stat/<device>/STATUS */
/*  replies are top-level-merged into one per-device record.           */
/*                                                                     */
/*  Tasmota discovery is poll-based and SLOW -- each device must be    */
/*  asked and answers asynchronously, trickling in over seconds -- so  */
/*  unlike the Zigbee dialog (which gets a retained bridge/devices     */
/*  inventory all at once and calls set_device_rows once), this dialog */
/*  fills the left list PROGRESSIVELY: a row is added the instant a    */
/*  device is first seen ("Querying…") and updated in place as its     */
/*  STATUS reply enriches it (pn_device_dialog_add/update_device_row). */
/*                                                                     */
/*  Selecting a device shows a read-only info page built from the      */
/*  merged STATUS reply; this iteration carries no device-mutating     */
/*  controls (the only outward traffic is the harmless Status query).  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gmodule.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include <pn-plugin.h>
#include <pn-node-factory.h>
#include <pn-device-provider.h>
#include <pn-device-dialog.h>
#include <pn-device-form.h>
#include <pn-device-combo.h>
#include <pn-action-button.h>
#include <pn-vault.h>
#include <pn-mqtt.h>
#include <pn-mqtt-sink.h>
#include <pn-mqtt-profile.h>
#include <pn-message.h>
#include <pn-node.h>

#include "pn-tasmota-common.h"

/* The themed menu icon.  "network-wireless" reads as "talk to a wireless
 * device" and ships with every common icon theme (the Meshtastic and
 * Zigbee providers use it too); no bundled asset to keep the packaging
 * minimal-invasive. */
#define TS_ICON_NAME        "network-wireless"

#define TS_DEV_CTX_QDATA    "pn-tasmota-dev-ctx"
#define TS_DEV_DIALOG_QDATA "pn-tasmota-dev-dialog"

/* PnMqtt takes a single subscribe filter; "#" is the broad choice the
 * discovery example uses, since Tasmota spreads itself across the tele/,
 * stat/ and cmnd/ families with no single prefix. */
#define TS_SUBSCRIBE_TOPIC  "#"

/* Tasmota's group-command topic -- a broadcast pseudo-device that never
 * answers a Status poll, so skip it (mirrors PnTasmotaProbe). */
#define TS_GROUP_DEVICE     "tasmotas"

/* Re-poll a device we keep hearing from but that has not answered Status
 * yet, no more often than this (a just-booted device can miss the first
 * poll).  In microseconds, matching g_get_monotonic_time(). */
#define TS_REPOLL_USEC      ((gint64) 8 * G_USEC_PER_SEC)

/* ------------------------------------------------------------------ */
/*  Per-device record                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Top-level-merged Status sections (an object node, owned), exactly
     * as PnTasmotaConcentrator accumulates them: Status, StatusNET,
     * StatusFWR, StatusSTS, … each keyed distinctly so a shallow merge
     * stitches them into one object. */
    JsonNode *merged;
    /* TRUE once at least one STATUS reply has merged -- the row's
     * secondary line stays "Querying…" until then. */
    gboolean  resolved;
    /* Monotonic time of the last Status poll we sent this device, so we
     * re-poll a slow device at most every TS_REPOLL_USEC. */
    gint64    last_poll;
} TsDevice;

static TsDevice *
ts_device_new (void)
{
    TsDevice   *dev = g_new0 (TsDevice, 1);
    JsonObject *obj = json_object_new ();

    dev->merged = json_node_alloc ();
    json_node_init_object (dev->merged, obj);   /* takes its own ref */
    json_object_unref (obj);
    return dev;
}

static void
ts_device_free (gpointer data)
{
    TsDevice *dev = data;

    json_node_free (dev->merged);
    g_free (dev);
}

/* ------------------------------------------------------------------ */
/*  Dialog state                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Reusable dialog frame -- owned by the GtkDialog widget, borrowed
     * here.  Cleared to NULL in the ctx free path before the source is
     * torn down (the shell may be freed as a sibling qdata first). */
    PnDeviceDialog  *shell;

    /* Broker page widgets (borrowed; the static page owns them). */
    GtkComboBoxText *broker_combo;
    gulong           combo_handler;
    GtkLabel        *conn_status;
    GtkLabel        *count_label;
    GtkWidget       *connect_btn;
    gboolean         connected;       /* a publish has been seen this session */

    /* Device-info page value labels (borrowed). */
    GtkLabel        *info_name;
    GtkLabel        *info_module;
    GtkLabel        *info_ip;
    GtkLabel        *info_mac;
    GtkLabel        *info_fw;
    GtkLabel        *info_uptime;
    GtkLabel        *info_power;
    GtkLabel        *info_sensors;

    /* The hidden MQTT Source.  NULL until Connect; owned -- released with
     * g_object_unref (its dispose joins the mosquitto network thread and
     * sends a clean DISCONNECT). */
    PnMqtt          *source;
    gulong           msg_handler;     /* "message" handler id, 0 if none */
    guint64          count;           /* messages received since Connect */

    /* The hidden MQTT Sink, created/destroyed alongside the source on the
     * same broker profile; the channel the Status polls go out on.  NULL
     * until Connect; owned. */
    PnMqttSink      *sink;

    /* device name (owned gchar*) -> TsDevice*.  The live fleet. */
    GHashTable      *devices;
    guint            seen_count;      /* devices currently in the list */
    guint            resolved_count;  /* devices that answered Status */

    /* Set of device names (owned gchar*) the broker reports offline via a
     * retained `tele/<name>/LWT = Offline`.  These are never listed -- the
     * broker replays the stale will on Connect, so a device unplugged for
     * days would otherwise reappear and sit forever at "Querying…". */
    GHashTable      *offline;

    /* The list-row id (device name) currently shown on the right, or NULL
     * when nothing is selected.  Owned. */
    gchar           *selected_id;
} TsDevCtx;

/* Forward decls. */
static void ts_start_source         (TsDevCtx *ctx);
static void ts_reset_to_disconnected (TsDevCtx *ctx);
static void ts_refresh_info         (TsDevCtx *ctx);

/* ------------------------------------------------------------------ */
/*  Small JSON accessors over a device's merged record                 */
/* ------------------------------------------------------------------ */

/* Borrow the @key sub-object off @obj, or NULL when absent / not an
 * object.  Valid only while @obj lives. */
static JsonObject *
ts_section (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

/* Borrow string member @key off @obj at the top level, or NULL when
 * absent / not a string. */
static const gchar *
ts_string (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

/* Tasmota's friendly name: Status.FriendlyName is an array of relay
 * labels; take the first.  Older firmware sends a bare string.  Returns a
 * pointer borrowed from @status, or NULL. */
static const gchar *
ts_friendly_name (JsonObject *status)
{
    JsonNode  *n;
    JsonArray *a;

    if (status == NULL || !json_object_has_member (status, "FriendlyName"))
        return NULL;
    n = json_object_get_member (status, "FriendlyName");
    if (JSON_NODE_HOLDS_ARRAY (n))
    {
        a = json_node_get_array (n);
        if (json_array_get_length (a) > 0)
            return json_array_get_string_element (a, 0);
        return NULL;
    }
    if (JSON_NODE_HOLDS_VALUE (n) &&
        json_node_get_value_type (n) == G_TYPE_STRING)
        return json_node_get_string (n);
    return NULL;
}

/* The relay/power state from StatusSTS: "POWER" for a single-relay
 * device, "POWER1".."POWER8" for a multi-relay one.  Returns a newly
 * allocated summary ("ON", "OFF", or "ON / OFF / …") or NULL when the
 * device reports no relay. */
static gchar *
ts_power_state (JsonObject *sts)
{
    GString     *out;
    const gchar *single;
    guint        i;
    gboolean     any = FALSE;

    if (sts == NULL)
        return NULL;

    single = ts_string (sts, "POWER");
    if (single != NULL)
        return g_strdup (single);

    out = g_string_new (NULL);
    for (i = 1; i <= 8; i++)
    {
        gchar       *key = g_strdup_printf ("POWER%u", i);
        const gchar *val = ts_string (sts, key);
        g_free (key);
        if (val == NULL)
            continue;
        g_string_append_printf (out, "%s%s", any ? " / " : "", val);
        any = TRUE;
    }
    if (!any)
    {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}

/* A short summary of the sensors a device exposes: the object-typed
 * members of StatusSNS (each is a per-sensor reading block, e.g.
 * "AM2301", "ENERGY"), comma-joined.  NULL when none. */
static gchar *
ts_sensor_summary (JsonObject *sns)
{
    GString *out;
    GList   *members, *iter;
    gboolean any = FALSE;

    if (sns == NULL)
        return NULL;

    out     = g_string_new (NULL);
    members = json_object_get_members (sns);
    for (iter = members; iter != NULL; iter = iter->next)
    {
        const gchar *name = iter->data;
        JsonNode    *n    = json_object_get_member (sns, name);

        if (!JSON_NODE_HOLDS_OBJECT (n))
            continue;                       /* skip Time, TempUnit, … */
        g_string_append_printf (out, "%s%s", any ? ", " : "", name);
        any = TRUE;
    }
    g_list_free (members);

    if (!any)
    {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Device-list rows                                                   */
/* ------------------------------------------------------------------ */

/* The bold first line for a device: its friendly name, falling back to
 * the topic device id.  Newly allocated. */
static gchar *
ts_device_primary (TsDevice *dev, const gchar *name)
{
    JsonObject  *status = ts_section (json_node_get_object (dev->merged), "Status");
    const gchar *fname  = ts_friendly_name (status);

    return g_strdup ((fname != NULL && *fname != '\0') ? fname : name);
}

/* The dim second line: "<ip> \xc2\xb7 <firmware>" once resolved, or
 * "Querying…" while we are still waiting for the first STATUS reply.
 * Newly allocated. */
static gchar *
ts_device_secondary (TsDevice *dev)
{
    JsonObject  *merged;
    const gchar *ip;
    const gchar *fw;

    if (!dev->resolved)
        return g_strdup ("Querying\xe2\x80\xa6");

    merged = json_node_get_object (dev->merged);
    ip = ts_string (ts_section (merged, "StatusNET"), "IPAddress");
    fw = ts_string (ts_section (merged, "StatusFWR"), "Version");

    if (ip != NULL && fw != NULL)
        return g_strdup_printf ("%s \xc2\xb7 %s", ip, fw);
    if (ip != NULL)
        return g_strdup (ip);
    if (fw != NULL)
        return g_strdup (fw);
    return g_strdup ("Online");
}

/* Build a one-off PnDeviceRow for @name from its record (caller frees). */
static PnDeviceRow *
ts_row_for (const gchar *name, TsDevice *dev)
{
    gchar       *primary   = ts_device_primary (dev, name);
    gchar       *secondary = ts_device_secondary (dev);
    PnDeviceRow *row       = pn_device_row_new (name, primary, secondary, NULL);

    g_free (primary);
    g_free (secondary);
    return row;
}

/* ------------------------------------------------------------------ */
/*  JSON merge (mirrors PnTasmotaConcentrator's top-level stitch)      */
/* ------------------------------------------------------------------ */

static void
ts_merge_into (JsonObject *dst, JsonObject *src)
{
    GList *members = json_object_get_members (src);
    GList *iter;

    for (iter = members; iter != NULL; iter = iter->next)
    {
        const gchar *name  = iter->data;
        JsonNode    *child = json_object_get_member (src, name);

        json_object_set_member (dst, name, json_node_copy (child));
    }
    g_list_free (members);
}

/* Pull data.payload off @message as a JSON object, or NULL when it
 * carries no structured payload (raw-string publishes have nothing to
 * merge). */
static JsonObject *
ts_payload_object (PnMessage *message)
{
    JsonNode *node = pn_message_get_member (message, "payload");

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    return json_node_get_object (node);
}

/* Pull data.payload off @message as a plain string, or NULL when it is
 * absent or not a string scalar.  A raw publish like the LWT's "Online" /
 * "Offline" lands here (JSON payloads go through ts_payload_object). */
static const gchar *
ts_payload_string (PnMessage *message)
{
    JsonNode *node = pn_message_get_member (message, "payload");

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    if (json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (node);
}

/* ------------------------------------------------------------------ */
/*  Connection state readout                                           */
/* ------------------------------------------------------------------ */

static void
ts_set_connected (TsDevCtx *ctx, gboolean connected)
{
    ctx->connected = connected;

    if (ctx->conn_status != NULL)
        gtk_label_set_markup (
            ctx->conn_status,
            connected
                ? "<span foreground='#2ecc71'>\xe2\x97\x8f</span> Connected"
                : "<span foreground='#888888'>\xe2\x97\x8f</span> Not connected");

    if (ctx->connect_btn != NULL)
        gtk_button_set_label (GTK_BUTTON (ctx->connect_btn),
                              connected ? "Disconnect" : "Connect");
}

/* One-line running progress in the status bar. */
static void
ts_update_status (TsDevCtx *ctx)
{
    if (ctx->shell == NULL)
        return;

    if (ctx->seen_count == 0)
    {
        pn_device_dialog_set_status (
            ctx->shell, "Listening for Tasmota devices\xe2\x80\xa6");
        return;
    }
    pn_device_dialog_set_statusf (
        ctx->shell, "%u device%s, %u resolved\xe2\x80\xa6",
        ctx->seen_count, ctx->seen_count == 1 ? "" : "s",
        ctx->resolved_count);
}

/* ------------------------------------------------------------------ */
/*  Discovery                                                          */
/* ------------------------------------------------------------------ */

/* Publish `cmnd/<name>/Status 0` through the hidden sink: the same probe
 * PnTasmotaProbe issues, asking the device to report every Status
 * section.  The sink publishes each inbound message's envelope topic with
 * data.payload verbatim, so we just set the two. */
static void
ts_poll_device (TsDevCtx *ctx, const gchar *name)
{
    gchar     *topic;
    PnMessage *msg;

    if (ctx->sink == NULL)
        return;

    topic = g_strdup_printf ("cmnd/%s/Status", name);
    msg   = pn_message_new (NULL, topic);
    pn_message_set_string (msg, "payload", "0");   /* Status 0 -> everything */
    pn_node_receive_message (PN_NODE (ctx->sink), msg);
    g_object_unref (msg);
    g_free (topic);
}

/* Merge a stat/<name>/STATUS# reply into @dev and refresh its row (and
 * the info page if it is the selected device). */
static void
ts_ingest_status (TsDevCtx    *ctx,
                  const gchar *name,
                  TsDevice    *dev,
                  PnMessage   *message)
{
    JsonObject  *payload = ts_payload_object (message);
    PnDeviceRow *row;

    if (payload == NULL)
        return;

    ts_merge_into (json_node_get_object (dev->merged), payload);

    if (!dev->resolved)
    {
        dev->resolved = TRUE;
        ctx->resolved_count++;
    }

    row = ts_row_for (name, dev);
    pn_device_dialog_update_device_row (ctx->shell, row);
    pn_device_row_free (row);

    if (g_strcmp0 (name, ctx->selected_id) == 0)
        ts_refresh_info (ctx);

    ts_update_status (ctx);
}

/* Forget a device entirely: drop its row, its record, and any selection or
 * info page pointing at it.  Used when the broker reports it offline.  A
 * no-op for a name that was never seeded. */
static void
ts_forget_device (TsDevCtx *ctx, const gchar *name)
{
    TsDevice *dev = g_hash_table_lookup (ctx->devices, name);

    if (dev == NULL)
        return;

    if (ctx->shell != NULL)
        pn_device_dialog_remove_device_row (ctx->shell, name);

    if (dev->resolved && ctx->resolved_count > 0)
        ctx->resolved_count--;
    if (ctx->seen_count > 0)
        ctx->seen_count--;

    if (g_strcmp0 (name, ctx->selected_id) == 0)
    {
        g_clear_pointer (&ctx->selected_id, g_free);
        ts_refresh_info (ctx);                  /* blank the info page */
    }

    g_hash_table_remove (ctx->devices, name);   /* frees the TsDevice */
    ts_update_status (ctx);
}

/* The hidden source's "message" signal: every PUBLISH the broker delivers
 * lands here on the main thread. */
static void
ts_on_message (PnMqtt *source, PnMessage *message, gpointer user_data)
{
    TsDevCtx    *ctx = user_data;
    const gchar *topic;
    const gchar *last;
    gchar       *name;
    TsDevice    *dev;
    gint64       now;

    (void) source;

    ctx->count++;
    {
        gchar *text = g_strdup_printf ("%" G_GUINT64_FORMAT, ctx->count);
        pn_device_form_set_value (ctx->count_label, text);
        g_free (text);
    }
    if (ctx->count == 1)
        ts_set_connected (ctx, TRUE);

    topic = pn_message_get_topic (message);
    name  = pn_tasmota_device_from_topic (topic);
    if (name == NULL)
        return;                                 /* not a Tasmota topic */
    if (g_strcmp0 (name, TS_GROUP_DEVICE) == 0)
    {
        g_free (name);                          /* group broadcast, skip */
        return;
    }

    /* The broker retains each device's last will (`tele/<name>/LWT`) and
     * replays it to every new subscriber.  So on Connect a device that has
     * been unplugged for days re-announces itself -- as "Offline".  Honour
     * that verdict: an offline device is never listed, and one that drops
     * while the dialog is open is removed.  We key off the LWT payload, not
     * message age -- a retained message carries no publish timestamp, and an
     * offline device typically has nothing but this LWT retained to date it
     * by, so an age threshold could not catch it. */
    if (g_str_has_suffix (topic, "/LWT"))
    {
        const gchar *state = ts_payload_string (message);

        if (state != NULL && g_ascii_strcasecmp (state, "Offline") == 0)
        {
            g_hash_table_add (ctx->offline, g_strdup (name));
            ts_forget_device (ctx, name);
            g_free (name);
            return;
        }
        if (state != NULL && g_ascii_strcasecmp (state, "Online") == 0)
            g_hash_table_remove (ctx->offline, name);   /* back from the dead */
    }

    /* A device the broker has flagged offline: ignore everything else it
     * retained (a stale row would only sit forever at "Querying…"). */
    if (g_hash_table_contains (ctx->offline, name))
    {
        g_free (name);
        return;
    }

    now = g_get_monotonic_time ();
    dev = g_hash_table_lookup (ctx->devices, name);

    if (dev == NULL)
    {
        /* First sighting: show the device IMMEDIATELY (the progressive
         * fill the slow poll makes worthwhile), then ask it for Status. */
        PnDeviceRow *row;

        dev = ts_device_new ();
        dev->last_poll = now;
        g_hash_table_insert (ctx->devices, g_strdup (name), dev);
        ctx->seen_count++;

        row = ts_row_for (name, dev);           /* "Querying…" */
        pn_device_dialog_add_device_row (ctx->shell, row);
        pn_device_row_free (row);

        ts_poll_device (ctx, name);
        ts_update_status (ctx);
    }
    else if (!dev->resolved && (now - dev->last_poll) > TS_REPOLL_USEC)
    {
        /* Heard from again but still silent on Status -- re-poke it (a
         * device that booted mid-scan can miss the first poll). */
        dev->last_poll = now;
        ts_poll_device (ctx, name);
    }

    /* Merge any STATUS reply (stat/<name>/STATUS...). */
    last = strrchr (topic, '/');
    last = (last != NULL) ? last + 1 : topic;
    if (g_str_has_prefix (topic, "stat/") && g_str_has_prefix (last, "STATUS"))
        ts_ingest_status (ctx, name, dev, message);

    g_free (name);
}

/* ------------------------------------------------------------------ */
/*  Right-hand info page                                               */
/* ------------------------------------------------------------------ */

/* Repaint the device-info page from the selected device's merged record,
 * or blank it to placeholders when nothing (or nothing resolved) is
 * selected. */
static void
ts_refresh_info (TsDevCtx *ctx)
{
    TsDevice    *dev   = NULL;
    JsonObject  *merged = NULL;
    JsonObject  *status, *net, *fwr, *sts, *prm;
    const gchar *fname;
    gchar       *power, *sensors;

    if (ctx->selected_id != NULL)
        dev = g_hash_table_lookup (ctx->devices, ctx->selected_id);

    if (dev == NULL || !dev->resolved)
    {
        /* Either nothing selected, or selected-but-still-querying. */
        const gchar *waiting =
            (dev != NULL) ? "Querying\xe2\x80\xa6" : NULL;

        pn_device_form_set_value (ctx->info_name,
                                  ctx->selected_id);   /* show the id meanwhile */
        pn_device_form_set_value (ctx->info_module,  waiting);
        pn_device_form_set_value (ctx->info_ip,      waiting);
        pn_device_form_set_value (ctx->info_mac,     waiting);
        pn_device_form_set_value (ctx->info_fw,      waiting);
        pn_device_form_set_value (ctx->info_uptime,  waiting);
        pn_device_form_set_value (ctx->info_power,   waiting);
        pn_device_form_set_value (ctx->info_sensors, waiting);
        return;
    }

    merged = json_node_get_object (dev->merged);
    status = ts_section (merged, "Status");
    net    = ts_section (merged, "StatusNET");
    fwr    = ts_section (merged, "StatusFWR");
    sts    = ts_section (merged, "StatusSTS");
    prm    = ts_section (merged, "StatusPRM");

    fname = ts_friendly_name (status);
    pn_device_form_set_value (ctx->info_name,
                              (fname != NULL) ? fname : ctx->selected_id);
    pn_device_form_set_value (ctx->info_module, ts_string (status, "DeviceName"));
    pn_device_form_set_value (ctx->info_ip,     ts_string (net, "IPAddress"));
    pn_device_form_set_value (ctx->info_mac,    ts_string (net, "Mac"));
    pn_device_form_set_value (ctx->info_fw,     ts_string (fwr, "Version"));

    {
        const gchar *uptime = ts_string (sts, "Uptime");
        if (uptime == NULL)
            uptime = ts_string (prm, "Uptime");
        pn_device_form_set_value (ctx->info_uptime, uptime);
    }

    power = ts_power_state (sts);
    pn_device_form_set_value (ctx->info_power, power);
    g_free (power);

    sensors = ts_sensor_summary (ts_section (merged, "StatusSNS"));
    pn_device_form_set_value (ctx->info_sensors, sensors);
    g_free (sensors);
}

/* PnDeviceDialogDetailFunc: the merged STATUS record for @device_id as a
 * pretty-printed JSON string (newly allocated), or NULL when the device is
 * unknown.  Exposed over the host's Devices D-Bus interface
 * (GetDeviceDetail) so an automation client can read the same data the
 * info pane shows. */
static gchar *
ts_device_detail (const gchar *device_id, gpointer user_data)
{
    TsDevCtx     *ctx = user_data;
    TsDevice     *dev;
    JsonGenerator *gen;
    gchar        *out;

    dev = g_hash_table_lookup (ctx->devices, device_id);
    if (dev == NULL)
        return NULL;

    gen = json_generator_new ();
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_root (gen, dev->merged);
    out = json_generator_to_data (gen, NULL);
    g_object_unref (gen);
    return out;
}

/* PnDeviceSelectedFunc: a row was clicked / activated. */
static void
ts_on_device_selected (const PnDeviceRow *row, gpointer user_data)
{
    TsDevCtx *ctx = user_data;

    g_free (ctx->selected_id);
    ctx->selected_id = g_strdup (row->id);
    ts_refresh_info (ctx);
    pn_device_dialog_set_current_page (ctx->shell, 1);   /* the Device tab */
}

/* ------------------------------------------------------------------ */
/*  Hidden source lifecycle                                            */
/* ------------------------------------------------------------------ */

static void
ts_drop_source (TsDevCtx *ctx)
{
    if (ctx->source != NULL)
    {
        if (ctx->msg_handler != 0)
        {
            g_signal_handler_disconnect (ctx->source, ctx->msg_handler);
            ctx->msg_handler = 0;
        }
        /* unref -> dispose joins the mosquitto network thread and sends a
         * clean DISCONNECT, so no callback can fire after this returns. */
        g_clear_object (&ctx->source);
    }
    g_clear_object (&ctx->sink);
}

/* PnDeviceDialogCloseFunc: the user is closing the dialog.  Drop the live
 * MQTT session now -- ts_drop_source joins the network thread -- so the
 * widget teardown that follows cannot race an in-flight message delivery.
 * (ts_dev_ctx_free also calls ts_drop_source; it is idempotent.) */
static void
ts_on_close (PnDeviceDialog *shell, gpointer user_data)
{
    TsDevCtx *ctx = user_data;

    (void) shell;
    ts_drop_source (ctx);
}

/* Tear the dialog back to its disconnected state: drop the live session,
 * forget the fleet, blank the list and counters, repaint Status. */
static void
ts_reset_to_disconnected (TsDevCtx *ctx)
{
    ts_drop_source (ctx);

    ctx->count          = 0;
    ctx->seen_count     = 0;
    ctx->resolved_count = 0;
    pn_device_form_set_value (ctx->count_label, "0");

    g_hash_table_remove_all (ctx->devices);
    g_hash_table_remove_all (ctx->offline);
    g_clear_pointer (&ctx->selected_id, g_free);
    pn_device_dialog_set_device_rows (ctx->shell, NULL);   /* clears the list */
    ts_refresh_info (ctx);

    ts_set_connected (ctx, FALSE);
}

static void
ts_start_source (TsDevCtx *ctx)
{
    const gchar *id;
    gchar       *label;

    /* Drop any prior session and clear the fleet before reconnecting. */
    ts_reset_to_disconnected (ctx);

    id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (ctx->broker_combo));
    if (id == NULL)
        id = "";                                /* "" -> primary broker */
    label = gtk_combo_box_text_get_active_text (ctx->broker_combo);

    /* Hidden source: subscribe to everything and watch for Tasmota
     * devices.  Setting the properties schedules the connect on the next
     * main-loop idle (PnMqtt debounces it). */
    ctx->source = pn_mqtt_new ();
    g_object_set (ctx->source,
                  "broker-profile",  id,
                  "subscribe-topic", TS_SUBSCRIBE_TOPIC,
                  NULL);
    ctx->msg_handler = g_signal_connect (ctx->source, "message",
                                         G_CALLBACK (ts_on_message), ctx);

    /* Hidden sink on the same broker, for the Status polls.  retain FALSE:
     * a cmnd/ request must not be retained or the broker would replay it. */
    ctx->sink = pn_mqtt_sink_new ();
    g_object_set (ctx->sink,
                  "broker-profile", id,
                  "retain",         FALSE,
                  NULL);

    pn_device_dialog_set_statusf (
        ctx->shell, "Connecting via %s, discovering Tasmota devices\xe2\x80\xa6",
        (label != NULL && *label != '\0') ? label : "the default broker");
    g_free (label);
}

/* ------------------------------------------------------------------ */
/*  Broker page callbacks                                              */
/* ------------------------------------------------------------------ */

static void
ts_on_apply_clicked (GtkButton *button, gpointer user_data)
{
    TsDevCtx *ctx = user_data;

    (void) button;

    if (ctx->connected || ctx->source != NULL)
    {
        ts_reset_to_disconnected (ctx);
        pn_device_dialog_set_status (ctx->shell, "Disconnected.");
    }
    else
        ts_start_source (ctx);
}

/* The user picked a different broker: any live session belongs to the old
 * profile, so drop it.  A no-op while none is running (including the
 * programmatic set_active during population). */
static void
ts_on_broker_changed (GtkComboBox *combo, gpointer user_data)
{
    TsDevCtx *ctx = user_data;

    (void) combo;

    if (ctx->source != NULL)
    {
        ts_reset_to_disconnected (ctx);
        pn_device_dialog_set_status (
            ctx->shell, "Broker changed \xe2\x80\x94 press Connect to discover.");
    }
}

/* PnDeviceScanFunc: the right-click Reload (and the D-Bus Scan method).
 * Re-poll the live broker only; never initiate a connection -- a broker
 * session is an outward action the user must authorise with Connect, so
 * Reload before that just nudges, it does not connect. */
static void
ts_on_scan (gpointer user_data)
{
    TsDevCtx *ctx = user_data;

    if (ctx->source != NULL)
        ts_start_source (ctx);
    else
        pn_device_dialog_set_status (
            ctx->shell, "Pick a broker and press Connect to discover devices.");
}

/* ------------------------------------------------------------------ */
/*  Broker combo population                                            */
/* ------------------------------------------------------------------ */

static void
ts_populate_brokers (TsDevCtx *ctx)
{
    PnVault   *vault = pn_vault_get_default ();
    PnProfile *primary;
    GList     *profiles, *l;
    gchar     *def_label;

    primary = pn_vault_get_default_profile (vault, PN_PROFILE_TYPE_MQTT_BROKER);
    def_label = (primary != NULL)
        ? g_strdup_printf ("Default (%s)", pn_profile_get_name (primary))
        : g_strdup ("Default (none set)");
    gtk_combo_box_text_append (ctx->broker_combo, "", def_label);
    g_free (def_label);

    profiles = pn_vault_list_profiles (vault, PN_PROFILE_TYPE_MQTT_BROKER);
    for (l = profiles; l != NULL; l = l->next)
    {
        PnProfile *p = l->data;
        gtk_combo_box_text_append (ctx->broker_combo,
                                   pn_profile_get_id (p),
                                   pn_profile_get_name (p));
    }
    g_list_free (profiles);   /* profiles are borrowed from the vault */

    gtk_combo_box_set_active (GTK_COMBO_BOX (ctx->broker_combo), 0);
}

/* ------------------------------------------------------------------ */
/*  Dialog construction                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *
ts_build_dialog (GtkWindow *parent, TsDevCtx *ctx)
{
    GtkWidget *dialog;
    GtkWidget *inner;
    GtkWidget *tab;
    GtkWidget *grid;
    GtkWidget *combo;
    GtkWidget *connect;
    GtkWidget *cell;
    gint       row = 0;

    ctx->devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, ts_device_free);
    ctx->offline = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);

    ctx->shell = pn_device_dialog_new (parent, "Tasmota Devices",
                                       PN_DEVICE_DIALOG_WITH_DEVICE_LIST);
    dialog = pn_device_dialog_get_dialog (ctx->shell);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 880, 580);

    /* ---- Broker page ---- */
    tab  = pn_device_form_new_tab (&inner);
    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);

    cell  = pn_device_form_attach_control_row (GTK_GRID (grid), row++, "Broker");
    combo = pn_device_combo_new ();
    ctx->broker_combo = GTK_COMBO_BOX_TEXT (combo);
    gtk_widget_set_tooltip_text (combo,
            "MQTT Broker credential profile the hidden Tasmota discovery "
            "source connects through.  \"Default\" follows the primary "
            "mqtt-broker profile.");
    gtk_box_pack_start (GTK_BOX (cell), combo, TRUE, TRUE, 0);
    ctx->combo_handler = g_signal_connect (combo, "changed",
                                           G_CALLBACK (ts_on_broker_changed),
                                           ctx);

    ctx->conn_status =
        pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Status");
    ctx->count_label =
        pn_device_form_attach_label_row (GTK_GRID (grid), row++,
                                         "Messages received");
    pn_device_form_set_value (ctx->count_label, "0");

    connect = pn_action_button_new ("Connect", PN_ACTION_BUTTON_NORMAL);
    ctx->connect_btn = connect;
    gtk_widget_set_halign (connect, GTK_ALIGN_END);
    gtk_widget_set_margin_top (connect, 8);
    g_signal_connect (connect, "clicked",
                      G_CALLBACK (ts_on_apply_clicked), ctx);
    gtk_grid_attach (GTK_GRID (grid), connect, 1, row++, 1, 1);

    pn_device_form_add_section (
        inner, "MQTT Broker",
        "Tasmota devices report through an MQTT broker.  Choose which "
        "broker to connect to and press Connect; \"Default\" uses the one "
        "your system already set up.  Discovery is gradual \xe2\x80\x94 each "
        "device is asked for its status and answers in its own time, so the "
        "list on the left fills in as replies arrive.",
        grid);

    pn_device_dialog_append_page (ctx->shell, tab, "Broker");

    /* ---- Device-info page ---- */
    tab  = pn_device_form_new_tab (&inner);
    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    row  = 0;

    ctx->info_name    = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Friendly name");
    ctx->info_module  = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Device name");
    ctx->info_ip      = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "IP address");
    ctx->info_mac     = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "MAC");
    ctx->info_fw      = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Firmware");
    ctx->info_uptime  = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Uptime");
    ctx->info_power   = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Relay state");
    ctx->info_sensors = pn_device_form_attach_label_row (GTK_GRID (grid), row++, "Sensors");

    pn_device_form_add_section (
        inner, "Device",
        "Details collected from the selected device's Status reply.  Pick a "
        "device in the list on the left; values appear once it answers.",
        grid);

    pn_device_dialog_append_page (ctx->shell, tab, "Device");

    /* Left-hand list: no auto-scan (devices arrive over MQTT once a broker
     * is connected); Reload reconnects, a click opens the info page. */
    pn_device_dialog_set_list_hints (
        ctx->shell,
        "network-wireless-disabled", "No devices yet.",
        "Pick a broker and press Connect.",
        "dialog-question", "No Tasmota devices seen.",
        "Are they powered on and reporting to this broker?");
    pn_device_dialog_set_scan_callback (ctx->shell, ts_on_scan, ctx);
    pn_device_dialog_set_selected_callback (ctx->shell,
                                            ts_on_device_selected, ctx);
    /* Stop the MQTT source/sink BEFORE the dialog's widgets are torn down:
     * the network thread is joined here so it cannot post a delivered
     * message to the main loop mid-destroy and deadlock the close. */
    pn_device_dialog_set_close_callback (ctx->shell, ts_on_close, ctx);
    /* Expose each device's merged Status reply over the host's Devices
     * D-Bus interface (GetDeviceDetail). */
    pn_device_dialog_set_detail_callback (ctx->shell, ts_device_detail, ctx);

    ts_populate_brokers (ctx);

    gtk_widget_show_all (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));

    ts_set_connected (ctx, FALSE);
    pn_device_dialog_set_status (ctx->shell,
                                 "Pick an MQTT broker and press Connect.");
    return dialog;
}

/* ------------------------------------------------------------------ */
/*  Lifetime                                                           */
/* ------------------------------------------------------------------ */

static void
ts_dev_ctx_free (gpointer data)
{
    TsDevCtx *ctx = data;

    /* The shell is owned by the dialog widget and may be torn down as a
     * sibling qdata before or after us; drop our borrowed pointer first so
     * nothing below touches it. */
    ctx->shell = NULL;

    ts_drop_source (ctx);
    g_clear_pointer (&ctx->devices, g_hash_table_unref);
    g_clear_pointer (&ctx->offline, g_hash_table_unref);
    g_clear_pointer (&ctx->selected_id, g_free);
    g_free (ctx);
}

static void
ts_on_dialog_destroyed (gpointer parent, GObject *was_dialog)
{
    (void) was_dialog;
    g_object_set_data (G_OBJECT (parent), TS_DEV_DIALOG_QDATA, NULL);
}

/* PnDeviceProviderPresentFunc: open the Tasmota devices dialog, or raise
 * the one already open for @parent. */
static void
ts_dialog_present (GtkWindow *parent, gpointer user_data)
{
    GtkWidget *dialog;
    TsDevCtx  *ctx;

    (void) user_data;
    g_return_if_fail (parent == NULL || GTK_IS_WINDOW (parent));

    if (parent != NULL)
    {
        dialog = g_object_get_data (G_OBJECT (parent), TS_DEV_DIALOG_QDATA);
        if (dialog != NULL)
        {
            gtk_window_present (GTK_WINDOW (dialog));
            return;
        }
    }

    ctx    = g_new0 (TsDevCtx, 1);
    dialog = ts_build_dialog (parent, ctx);

    /* Ctx dies with the dialog. */
    g_object_set_data_full (G_OBJECT (dialog), TS_DEV_CTX_QDATA,
                            ctx, ts_dev_ctx_free);

    if (parent != NULL)
    {
        g_object_set_data (G_OBJECT (parent), TS_DEV_DIALOG_QDATA, dialog);
        g_object_weak_ref (G_OBJECT (dialog),
                           (GWeakNotify) ts_on_dialog_destroyed, parent);
    }

    gtk_window_present (GTK_WINDOW (dialog));
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_gui_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-tasmota-gui",
        .version     = "0.1.0",
        .description = "Tasmota Devices dialog (GUI companion).",
    };

    (void) factory;   /* no per-type vfuncs to install; the menu is all */

    pn_device_provider_register ("tasmota", "Tasmota Devices", TS_ICON_NAME,
                                 ts_dialog_present, NULL, NULL);
    return &info;
}
