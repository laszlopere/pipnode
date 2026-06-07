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

#include "pn-tasmota-concentrator.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* fa-database U+F1C0 -- the node is the store that accumulates every
 * device's report into one place, so a database glyph reads naturally
 * and stays distinct from the Probe's binoculars.  FontAwesome 4.7 only
 * (project convention). */
#define PN_TASMOTA_CONCENTRATOR_ICON       "\xef\x87\x80"

/* Tasmota's built-in group topic.  Replies are never published under it
 * (a group Status is silently answer-suppressed by the firmware), but
 * guard against it appearing in a contrived topic so it is never folded
 * into the snapshot as a phantom device. */
#define PN_TASMOTA_GROUP_TOPIC_ALL         "tasmotas"

/* Topic the combined snapshot is emitted under.  The inbound envelope
 * topic (stat/<device>/STATUS#) names a single device and would be
 * misleading on a message that carries the whole fleet, so it is
 * replaced with this neutral tag. */
#define PN_TASMOTA_CONCENTRATOR_TOPIC      "tasmota/devices"

/* Default emit min-interval, in seconds.  A single "Status 0" reply
 * fans out into a dozen STATUS# messages back-to-back; two seconds
 * debounces that burst into one downstream snapshot while still feeling
 * live to a Debug / table view. */
#define PN_TASMOTA_CONCENTRATOR_DEFAULT_INTERVAL  2.0

struct _PnTasmotaConcentrator
{
    PnNode parent_instance;

    /* Minimum seconds between two emitted snapshots.  <= 0 means "emit
     * on every accepted message". */
    gdouble     interval;

    /* Drop a device whose last reply is older than this many seconds
     * before building the next snapshot.  <= 0 disables the timeout, so
     * a device, once seen, stays in the snapshot for the session. */
    gdouble     timeout;

    /* Also fold tele/<device>/STATE and tele/<device>/SENSOR telemetry
     * into the per-device record, not just the stat/<device>/STATUS#
     * replies.  TRUE by default so a device's live state and sensor
     * readings ride alongside its static status sections. */
    gboolean    merge_telemetry;

    /* device name (gchar*, owned) -> DeviceRecord* (owned).  The live
     * fleet: every device whose replies have been collected and not yet
     * timed out. */
    GHashTable *devices;

    /* Monotonic time of the last emitted snapshot (microseconds from
     * g_get_monotonic_time()).  0 means "never emitted", so the first
     * accepted message always produces a snapshot. */
    gint64      last_emit;
};

/* One accumulating per-device record. */
typedef struct
{
    /* A JSON object node holding the top-level-merged report for this
     * device.  Owned; freed with the record. */
    JsonNode *merged;

    /* Monotonic time of the most recent reply folded in, for the
     * silent-device timeout. */
    gint64    last_seen;
} DeviceRecord;

G_DEFINE_TYPE (PnTasmotaConcentrator, pn_tasmota_concentrator, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INTERVAL,
    PROP_TIMEOUT,
    PROP_MERGE_TELEMETRY,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Device records                                                     */
/* ------------------------------------------------------------------ */

static DeviceRecord *
device_record_new (void)
{
    DeviceRecord *rec = g_new0 (DeviceRecord, 1);
    JsonObject   *obj = json_object_new ();

    rec->merged = json_node_alloc ();
    json_node_init_object (rec->merged, obj);   /* takes its own ref */
    json_object_unref (obj);
    return rec;
}

static void
device_record_free (gpointer data)
{
    DeviceRecord *rec = data;

    json_node_free (rec->merged);
    g_free (rec);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** The last segment of @topic (the Tasmota command/section), or the
 *  whole topic when it has no slash. */
static const gchar *
topic_last_segment (const gchar *topic)
{
    const gchar *slash = strrchr (topic, '/');
    return (slash != NULL) ? slash + 1 : topic;
}

/** Decide whether @topic is one the concentrator collects: a
 *  stat/<device>/STATUS# reply always, plus tele/<device>/STATE and
 *  tele/<device>/SENSOR telemetry when merge-telemetry is on.  Anything
 *  else sharing the upstream MQTT Source is ignored. */
static gboolean
topic_is_accepted (PnTasmotaConcentrator *self, const gchar *topic)
{
    const gchar *last;

    if (topic == NULL || *topic == '\0')
        return FALSE;

    last = topic_last_segment (topic);

    if (g_str_has_prefix (topic, "stat/") &&
        g_str_has_prefix (last, "STATUS"))
        return TRUE;

    if (self->merge_telemetry &&
        g_str_has_prefix (topic, "tele/") &&
        (g_strcmp0 (last, "STATE") == 0 || g_strcmp0 (last, "SENSOR") == 0))
        return TRUE;

    return FALSE;
}

/** Pull data.payload off @message as a JSON object, or %NULL when the
 *  message carries no structured payload (raw-string MQTT publishes land
 *  via PnMqtt's text fallback and have nothing to merge). */
static JsonObject *
get_payload_object (PnMessage *message)
{
    JsonNode *node = pn_message_get_member (message, "payload");

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    return json_node_get_object (node);
}

/** Shallow-merge @src into @dst at the top level: every member of @src
 *  is deep-copied into @dst, replacing any member of the same name.
 *  Tasmota keys each Status section distinctly (Status, StatusNET,
 *  StatusSTS, ...), so a top-level merge naturally stitches the sections
 *  of one device into a single object without clobbering each other. */
static void
merge_into (JsonObject *dst, JsonObject *src)
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

/** Drop every device whose last reply is older than the timeout.  A
 *  no-op when the timeout is disabled (<= 0). */
static void
prune_silent (PnTasmotaConcentrator *self, gint64 now)
{
    GHashTableIter it;
    gpointer       key, value;
    gint64         max_age;

    if (self->timeout <= 0.0)
        return;

    max_age = (gint64) (self->timeout * (gdouble) G_USEC_PER_SEC);

    g_hash_table_iter_init (&it, self->devices);
    while (g_hash_table_iter_next (&it, &key, &value))
    {
        DeviceRecord *rec = value;
        if ((now - rec->last_seen) > max_age)
            g_hash_table_iter_remove (&it);
    }
}

/** Whether a snapshot is due to be emitted now. */
static gboolean
emit_is_due (PnTasmotaConcentrator *self, gint64 now)
{
    gint64 min_gap;

    if (self->interval <= 0.0)
        return TRUE;                   /* no debounce -> emit every time */
    if (self->last_emit == 0)
        return TRUE;                   /* never emitted -> emit promptly */

    min_gap = (gint64) (self->interval * (gdouble) G_USEC_PER_SEC);
    return (now - self->last_emit) >= min_gap;
}

/* ------------------------------------------------------------------ */
/*  Snapshot                                                           */
/* ------------------------------------------------------------------ */

/** Reshape @message in place into the combined fleet snapshot: a
 *  data.devices object mapping each known device to a deep copy of its
 *  merged record, plus the standard value / success / output contract.
 *  Member order in both the object and the summary is the devices'
 *  sorted name order, so the snapshot is stable across emits. */
static void
emit_snapshot (PnTasmotaConcentrator *self, PnMessage *message)
{
    JsonObject *devices = json_object_new ();
    JsonNode   *devnode;
    GList      *names, *iter;
    GString    *summary;
    guint       count;

    /* Build data.devices from the live records, deep-copying each so a
     * later merge into our internal state cannot mutate an already-
     * emitted snapshot. */
    {
        GHashTableIter it;
        gpointer       key, value;

        g_hash_table_iter_init (&it, self->devices);
        while (g_hash_table_iter_next (&it, &key, &value))
        {
            const gchar  *name = key;
            DeviceRecord *rec  = value;
            json_object_set_member (devices, name,
                                    json_node_copy (rec->merged));
        }
    }

    count = json_object_get_size (devices);

    /* Human-readable summary: "N devices: a, b, c" in sorted order. */
    names   = json_object_get_members (devices);   /* borrowed names */
    names   = g_list_sort (names, (GCompareFunc) g_strcmp0);
    summary = g_string_new (NULL);
    g_string_append_printf (summary, "%u device%s",
                            count, count == 1 ? "" : "s");
    for (iter = names; iter != NULL; iter = iter->next)
        g_string_append_printf (summary, "%s%s",
                                iter == names ? ": " : ", ",
                                (const gchar *) iter->data);
    g_list_free (names);

    /* Hand the devices object to the message (it takes the node). */
    devnode = json_node_alloc ();
    json_node_init_object (devnode, devices);      /* takes its own ref */
    json_object_unref (devices);
    pn_message_set_member (message, "devices", devnode);

    pn_message_set_topic   (message, PN_TASMOTA_CONCENTRATOR_TOPIC);
    pn_message_set_double  (message, "value",   (gdouble) count);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "output",  summary->str);

    g_string_free (summary, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_concentrator_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaConcentrator *self = PN_TASMOTA_CONCENTRATOR (node);
    const gchar           *topic;
    gchar                 *device;
    JsonObject            *payload;
    DeviceRecord          *rec;
    gint64                 now;

    /* Only the collectible topic families pass; everything else sharing
     * the upstream MQTT Source is ignored. */
    topic = pn_message_get_topic (message);
    if (!topic_is_accepted (self, topic))
        return;

    device = pn_tasmota_device_from_topic (topic);
    if (device == NULL)
        return;

    if (g_strcmp0 (device, PN_TASMOTA_GROUP_TOPIC_ALL) == 0)
    {
        g_free (device);
        return;
    }

    /* Nothing to accumulate without a structured payload. */
    payload = get_payload_object (message);
    if (payload == NULL)
    {
        g_free (device);
        return;
    }

    now = g_get_monotonic_time ();

    rec = g_hash_table_lookup (self->devices, device);
    if (rec == NULL)
    {
        rec = device_record_new ();
        g_hash_table_insert (self->devices, g_strdup (device), rec);
    }
    merge_into (json_node_get_object (rec->merged), payload);
    rec->last_seen = now;

    g_free (device);

    /* Forget devices that have gone silent (the freshly-updated one
     * cannot be among them: its last_seen is now). */
    prune_silent (self, now);

    if (!emit_is_due (self, now))
        return;                        /* still inside the debounce window */

    self->last_emit = now;
    emit_snapshot (self, message);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_concentrator_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaConcentrator *self = PN_TASMOTA_CONCENTRATOR (object);

    switch (prop_id)
    {
    case PROP_INTERVAL:
        g_value_set_double (value, self->interval);
        break;
    case PROP_TIMEOUT:
        g_value_set_double (value, self->timeout);
        break;
    case PROP_MERGE_TELEMETRY:
        g_value_set_boolean (value, self->merge_telemetry);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_concentrator_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaConcentrator *self = PN_TASMOTA_CONCENTRATOR (object);

    switch (prop_id)
    {
    case PROP_INTERVAL:
        self->interval = g_value_get_double (value);
        g_object_notify_by_pspec (object, props[PROP_INTERVAL]);
        break;
    case PROP_TIMEOUT:
        self->timeout = g_value_get_double (value);
        g_object_notify_by_pspec (object, props[PROP_TIMEOUT]);
        break;
    case PROP_MERGE_TELEMETRY:
        self->merge_telemetry = g_value_get_boolean (value);
        g_object_notify_by_pspec (object, props[PROP_MERGE_TELEMETRY]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_concentrator_finalize (GObject *object)
{
    PnTasmotaConcentrator *self = PN_TASMOTA_CONCENTRATOR (object);

    g_clear_pointer (&self->devices, g_hash_table_destroy);

    G_OBJECT_CLASS (pn_tasmota_concentrator_parent_class)->finalize (object);
}

static void
pn_tasmota_concentrator_class_init (PnTasmotaConcentratorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_concentrator_get_property;
    object_class->set_property = pn_tasmota_concentrator_set_property;
    object_class->finalize     = pn_tasmota_concentrator_finalize;

    node_class->receive      = pn_tasmota_concentrator_receive;

    node_class->palette_icon = PN_TASMOTA_CONCENTRATOR_ICON;
    node_class->class_name   = "Tasmota Concentrator";
    node_class->icon         = PN_TASMOTA_CONCENTRATOR_ICON;
    /* A sea-green that reads as the passive "collector" counterpart to
     * the Probe's active violet, while staying in the Tasmota family and
     * clear of grey (reserved for disabled nodes) and the mid-blue that
     * reads as grey. */
    node_class->color        = (PnColor){ 0.27, 0.62, 0.47, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_INTERVAL] = g_param_spec_double (
            "interval", "Emit interval",
            "Minimum seconds between two emitted fleet snapshots.  A "
            "single Status 0 reply arrives as a burst of STATUS# "
            "messages; this debounces the burst into one downstream "
            "update.  Set to 0 to emit a snapshot on every collected "
            "message.",
            0.0, G_MAXDOUBLE, PN_TASMOTA_CONCENTRATOR_DEFAULT_INTERVAL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEOUT] = g_param_spec_double (
            "timeout", "Silent timeout",
            "Drop a device from the snapshot when its most recent reply "
            "is older than this many seconds, so the snapshot reflects "
            "only devices still reporting.  Set to 0 to keep every "
            "device once seen for the lifetime of the node.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MERGE_TELEMETRY] = g_param_spec_boolean (
            "merge-telemetry", "Merge telemetry",
            "Also fold tele/<device>/STATE and tele/<device>/SENSOR "
            "telemetry into each device's record, alongside the "
            "stat/<device>/STATUS# replies.  Turn off to collect only "
            "the explicit Status sections.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_concentrator_init (PnTasmotaConcentrator *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  green = { 0.27, 0.62, 0.47, 1.0 };

    self->interval        = PN_TASMOTA_CONCENTRATOR_DEFAULT_INTERVAL;
    self->timeout         = 0.0;
    self->merge_telemetry = TRUE;
    self->last_emit       = 0;
    self->devices         = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free,
                                                   device_record_free);

    pn_node_set_class_name (node, "Tasmota Concentrator");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* The class-level icon/color seed the palette widget; the canvas
     * node draws from the *instance*, so set them here too (project
     * convention: set the icon in both class_init and init). */
    pn_node_set_icon  (node, PN_TASMOTA_CONCENTRATOR_ICON);
    pn_node_set_color (node, &green);
}

PnTasmotaConcentrator *
pn_tasmota_concentrator_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_CONCENTRATOR, NULL);
}
