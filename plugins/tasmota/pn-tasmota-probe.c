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

#include "pn-tasmota-probe.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

/* fa-binoculars U+F1E5 -- the node watches the telemetry stream go by to
 * spot devices, so binoculars read naturally and stay distinct from the
 * question-mark the manual Status Request uses.  FontAwesome 4.7 only
 * (project convention). */
#define PN_TASMOTA_PROBE_ICON       "\xef\x87\xa5"

/* Tasmota's built-in group topic.  A device of this name is never a real
 * endpoint and a group Status yields no reply, so we never poll it even
 * though it appears in the cmnd/<name>/... shape. */
#define PN_TASMOTA_GROUP_TOPIC_ALL  "tasmotas"

/* Default minimum re-probe interval, in seconds: a known device is not
 * re-queried more often than this.  Five minutes keeps a steady stream
 * of telemetry from hammering the broker while still refreshing each
 * device's status often enough to notice an IP / firmware change. */
#define PN_TASMOTA_PROBE_DEFAULT_INTERVAL  300.0

struct _PnTasmotaProbe
{
    PnNode parent_instance;

    /* Which Status <n> section to ask each device for; the enum value is
     * the wire argument. */
    PnTasmotaStatusKind  request;

    /* Minimum seconds between two probes of the *same* device.  <= 0
     * means "probe on every message that names the device". */
    gdouble              interval;

    /* device name (gchar*, owned) -> last-probe monotonic time
     * (gint64*, owned, microseconds from g_get_monotonic_time()).  The
     * live set of every Tasmota device this node has ever seen. */
    GHashTable          *seen;
};

G_DEFINE_TYPE (PnTasmotaProbe, pn_tasmota_probe, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_REQUEST,
    PROP_INTERVAL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Parse the device name out of @topic, but only for the three Tasmota
 *  topic families (tele/, stat/, cmnd/) the node is meant to watch.
 *  Anything else sharing the upstream MQTT Source is ignored.  Returns
 *  a freshly allocated name, or %NULL when the topic is not a Tasmota
 *  device topic. */
static gchar *
probe_device_from_topic (const gchar *topic)
{
    if (topic == NULL)
        return NULL;

    if (!g_str_has_prefix (topic, "tele/") &&
        !g_str_has_prefix (topic, "stat/") &&
        !g_str_has_prefix (topic, "cmnd/"))
        return NULL;

    return pn_tasmota_device_from_topic (topic);
}

/** Decide whether @device is due for a probe right now and, when it is,
 *  record the probe time.  A device never seen before, or one last
 *  probed at least `interval` seconds ago (or any time when interval is
 *  <= 0), is due.  Updates the live set as a side effect. */
static gboolean
probe_is_due (PnTasmotaProbe *self, const gchar *device, gint64 now)
{
    gint64 *last = g_hash_table_lookup (self->seen, device);
    gboolean due;

    if (last == NULL)
    {
        due = TRUE;                       /* never seen -> probe promptly */
    }
    else if (self->interval <= 0.0)
    {
        due = TRUE;                       /* no rate limit -> always probe */
    }
    else
    {
        gint64 min_gap = (gint64) (self->interval * (gdouble) G_USEC_PER_SEC);
        due = (now - *last) >= min_gap;
    }

    if (!due)
        return FALSE;

    if (last != NULL)
        *last = now;                      /* refresh existing entry */
    else
    {
        gint64 *stamp = g_new (gint64, 1);
        *stamp = now;
        g_hash_table_insert (self->seen, g_strdup (device), stamp);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_probe_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaProbe *self = PN_TASMOTA_PROBE (node);
    gchar          *device;
    gchar          *topic;
    gchar          *payload;
    gchar          *output;

    /* Watch the stream go by: pull the device name out of the inbound
     * topic.  Non-Tasmota traffic (and the group topic) is silently
     * ignored -- this node emits probes, never forwards telemetry. */
    device = probe_device_from_topic (pn_message_get_topic (message));
    if (device == NULL)
        return;

    if (g_strcmp0 (device, PN_TASMOTA_GROUP_TOPIC_ALL) == 0)
    {
        g_free (device);
        return;
    }

    if (!probe_is_due (self, device, g_get_monotonic_time ()))
    {
        /* Known device, still inside its re-probe interval: drop. */
        g_free (device);
        return;
    }

    /* Reshape the (already-cloned, per-branch) inbound message in place
     * into a cmnd/<device>/Status <n> command the downstream MQTT Sink
     * publishes -- exactly the shape PnTasmotaStatusRequest produces,
     * but addressed at the device we just discovered. */
    topic = g_strdup_printf ("cmnd/%s/Status", device);
    pn_message_set_topic (message, topic);
    g_free (topic);

    payload = g_strdup_printf ("%d", (gint) self->request);
    pn_message_set_string  (message, "payload", payload);
    g_free (payload);

    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  device);

    output = g_strdup_printf ("%s: probe Status %d",
                              device, (gint) self->request);
    pn_message_set_string (message, "output", output);
    g_free (output);

    g_free (device);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_probe_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaProbe *self = PN_TASMOTA_PROBE (object);

    switch (prop_id)
    {
    case PROP_REQUEST:
        g_value_set_enum (value, self->request);
        break;
    case PROP_INTERVAL:
        g_value_set_double (value, self->interval);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_probe_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaProbe *self = PN_TASMOTA_PROBE (object);

    switch (prop_id)
    {
    case PROP_REQUEST:
        self->request = g_value_get_enum (value);
        g_object_notify_by_pspec (object, props[PROP_REQUEST]);
        break;
    case PROP_INTERVAL:
        self->interval = g_value_get_double (value);
        g_object_notify_by_pspec (object, props[PROP_INTERVAL]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_probe_finalize (GObject *object)
{
    PnTasmotaProbe *self = PN_TASMOTA_PROBE (object);

    g_clear_pointer (&self->seen, g_hash_table_destroy);

    G_OBJECT_CLASS (pn_tasmota_probe_parent_class)->finalize (object);
}

static void
pn_tasmota_probe_class_init (PnTasmotaProbeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_probe_get_property;
    object_class->set_property = pn_tasmota_probe_set_property;
    object_class->finalize     = pn_tasmota_probe_finalize;

    node_class->receive      = pn_tasmota_probe_receive;

    node_class->palette_icon = PN_TASMOTA_PROBE_ICON;
    node_class->class_name   = "Tasmota Probe";
    node_class->icon         = PN_TASMOTA_PROBE_ICON;
    /* A violet that reads as the active "discovery" node, distinct from
     * the manual Status Request's teal and the relay command's steel
     * blue while still sitting in the Tasmota family.  Kept clear of
     * grey (which the worksheet reserves for disabled nodes) and of the
     * mid-blue that reads as grey. */
    node_class->color        = (PnColor){ 0.55, 0.40, 0.78, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_REQUEST] = g_param_spec_enum (
            "request", "Request",
            "Which Tasmota Status section each discovered device is asked "
            "for.  \"Everything (Status 0)\" returns the full report; the "
            "other choices request one specific section (network, MQTT, "
            "firmware, sensors, ...).  The replies arrive on "
            "stat/<device>/STATUS# and are gathered by the Tasmota "
            "Concentrator.",
            PN_TYPE_TASMOTA_STATUS_KIND, PN_TASMOTA_STATUS_ALL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INTERVAL] = g_param_spec_double (
            "interval", "Re-probe interval",
            "Minimum seconds between two probes of the same device.  A "
            "newly-seen device is probed promptly; a device already known "
            "is never re-probed more often than this.  Set to 0 to probe "
            "on every message that names the device.",
            0.0, G_MAXDOUBLE, PN_TASMOTA_PROBE_DEFAULT_INTERVAL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_probe_init (PnTasmotaProbe *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.55, 0.40, 0.78, 1.0 };

    self->request  = PN_TASMOTA_STATUS_ALL;
    self->interval = PN_TASMOTA_PROBE_DEFAULT_INTERVAL;
    self->seen     = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);

    pn_node_set_class_name (node, "Tasmota Probe");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* The class-level icon/color seed the palette widget; the canvas
     * node draws from the *instance*, so set them here too (project
     * convention: set the icon in both class_init and init). */
    pn_node_set_icon  (node, PN_TASMOTA_PROBE_ICON);
    pn_node_set_color (node, &violet);
}

PnTasmotaProbe *
pn_tasmota_probe_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_PROBE, NULL);
}
