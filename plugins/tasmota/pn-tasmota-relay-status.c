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

#include "pn-tasmota-relay-status.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* fa-toggle-on U+F205 -- same glyph family as the host's PnSwitch so
 * the two read as siblings (a Tasmota relay is the network-side
 * counterpart of the canvas-side slide switch).  Flipped to the
 * warning ❗ glyph (U+2757) while the switch-name filter is empty so
 * an unconfigured node reads as "needs setup" at a glance, mirroring
 * the convention PnInject established. */
#define PN_TASMOTA_RELAY_STATUS_ICON         "\xef\x88\x85"
#define PN_TASMOTA_RELAY_STATUS_WARNING_ICON "\xe2\x9d\x97"

struct _PnTasmotaRelayStatus
{
    PnNode parent_instance;

    /* Tasmota device name the node filters on.  Only messages whose
     * topic carries this exact name in the second-to-last segment
     * (Tasmota's <prefix>/<device>/<command> topology) are processed;
     * everything else is dropped silently.  An empty / NULL value
     * marks the node as unconfigured and rejects every message --
     * mirrors PnInject's "configuration required" convention so the
     * red body reads as a prompt to fill the field in. */
    gchar *switch_name;
};

G_DEFINE_TYPE (PnTasmotaRelayStatus, pn_tasmota_relay_status, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SWITCH_NAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip between the configured (steel-blue + toggle glyph) and
 *  unconfigured (red + ❗ glyph) appearance depending on whether the
 *  switch-name filter has been set.  Same shape as PnInject's
 *  apply_visual_state so both nodes carry the same visual cue for
 *  "this needs a value before it can do anything". */
static void
apply_visual_state (
        PnTasmotaRelayStatus *self,
        gboolean              configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        PnColor steel = { 0.42, 0.55, 0.72, 1.0 };
        pn_node_set_color (node, &steel);
        pn_node_set_icon  (node, PN_TASMOTA_RELAY_STATUS_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_TASMOTA_RELAY_STATUS_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Topic + payload parsing                                            */
/* ------------------------------------------------------------------ */

/** Tasmota relay state lands on `stat/<device>/POWER` for the single-
 *  relay default plus `POWER1`..`POWER8` for multi-relay devices.
 *  Matching "starts with POWER" on the last topic segment covers
 *  every variant without listing the eight numbered codes one by
 *  one. */
static gboolean
topic_is_tasmota_power (const gchar *topic)
{
    const gchar *last_slash;
    const gchar *last_segment;

    if (topic == NULL || *topic == '\0')
        return FALSE;

    last_slash   = strrchr (topic, '/');
    last_segment = (last_slash != NULL) ? last_slash + 1 : topic;

    return g_str_has_prefix (last_segment, "POWER");
}

/** Pull a string `data.payload` member off @message.  Tasmota POWER
 *  publishes carry the bare strings "ON" / "OFF" (not a JSON object
 *  the way SENSOR / STATE do), so PnMqtt forwards them through the
 *  text-payload fallback as a string member rather than the structured
 *  object the temperature / humidity filters expect.  Returns a
 *  borrowed pointer; %NULL when the message has no payload or it is
 *  not a JSON string. */
static const gchar *
get_payload_string (PnMessage *message)
{
    JsonNode *node;

    node = pn_message_get_member (message, "payload");
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    if (json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (node);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_relay_status_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaRelayStatus *self = PN_TASMOTA_RELAY_STATUS (node);
    const gchar          *payload;
    const gchar          *topic;
    gboolean              on;
    gchar                *device = NULL;
    gchar                *output;

    /* Unconfigured nodes drop every message -- there is no sensible
     * default device name we could pick on the user's behalf. */
    if (self->switch_name == NULL || *self->switch_name == '\0')
        return;

    topic = pn_message_get_topic (message);
    if (!topic_is_tasmota_power (topic))
        return;

    /* Per-device filter: only messages from the configured
     * <prefix>/<switch-name>/POWER* topic pass.  Comparing extracted
     * device names (rather than substring-matching the raw topic)
     * means "sonoff1" does not accidentally accept publishes from
     * "sonoff19". */
    device = pn_tasmota_device_from_topic (topic);
    if (device == NULL || g_strcmp0 (device, self->switch_name) != 0)
    {
        g_free (device);
        return;
    }

    payload = get_payload_string (message);
    if (payload == NULL)
    {
        g_free (device);
        return;
    }

    /* Tasmota encodes the relay state as the literal strings "ON" /
     * "OFF"; anything else (the "TOGGLE" / "PulseTime" / "Blink"
     * responses Tasmota emits on the same topic in other contexts)
     * leaves the latch alone -- silently dropping a non-binary state
     * is friendlier than synthesising a 0 that downstream filters
     * would then treat as a real off-transition. */
    if (g_ascii_strcasecmp (payload, "ON") == 0)
        on = TRUE;
    else if (g_ascii_strcasecmp (payload, "OFF") == 0)
        on = FALSE;
    else
    {
        g_free (device);
        return;
    }

    pn_message_set_double  (message, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  device);

    output = g_strdup_printf ("%s: power %s", device, on ? "on" : "off");
    pn_message_set_string (message, "output", output);
    g_free (output);
    g_free (device);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
tasmota_relay_status_set_name (
        PnTasmotaRelayStatus *self,
        const gchar          *name)
{
    /* Normalise NULL / "" so both forms produce the same unconfigured
     * state, but round-trip whatever the caller passed so the dialog
     * sees a stable value on the notify::switch-name signal. */
    gchar    *replacement = (name != NULL) ? g_strdup (name) : NULL;
    gboolean  configured  = (name != NULL && *name != '\0');

    g_free (self->switch_name);
    self->switch_name = replacement;

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SWITCH_NAME]);
}

static void
pn_tasmota_relay_status_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaRelayStatus *self = PN_TASMOTA_RELAY_STATUS (object);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        g_value_set_string (value, self->switch_name);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_relay_status_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaRelayStatus *self = PN_TASMOTA_RELAY_STATUS (object);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        tasmota_relay_status_set_name (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_relay_status_finalize (GObject *object)
{
    PnTasmotaRelayStatus *self = PN_TASMOTA_RELAY_STATUS (object);

    g_clear_pointer (&self->switch_name, g_free);

    G_OBJECT_CLASS (pn_tasmota_relay_status_parent_class)->finalize (object);
}

static void
pn_tasmota_relay_status_class_init (PnTasmotaRelayStatusClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_relay_status_get_property;
    object_class->set_property = pn_tasmota_relay_status_set_property;
    object_class->finalize     = pn_tasmota_relay_status_finalize;

    node_class->receive      = pn_tasmota_relay_status_receive;

    /* Pin the toggle glyph for the palette so the entry reads the same
     * regardless of whether any live instance has been configured. */
    node_class->palette_icon = PN_TASMOTA_RELAY_STATUS_ICON;
    node_class->class_name   = "Tasmota Relay Status";
    node_class->icon         = PN_TASMOTA_RELAY_STATUS_ICON;
    node_class->color        = (PnColor){ 0.42, 0.55, 0.72, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_SWITCH_NAME] = g_param_spec_string (
            "switch-name", "Switch name",
            "Tasmota device name the filter listens for.  Matched "
            "against the second-to-last segment of each incoming "
            "topic (the canonical Tasmota <prefix>/<device>/<command> "
            "topology), so 'sonoff19' accepts messages whose topic is "
            "'stat/sonoff19/POWER' (or any POWER<N> variant) and "
            "drops everything else.  Empty marks the node as needing "
            "configuration and rejects every message.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_relay_status_init (PnTasmotaRelayStatus *self)
{
    PnNode *node = PN_NODE (self);

    self->switch_name = NULL;

    pn_node_set_class_name (node, "Tasmota Relay Status");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped
     * node visibly nags the user to fill the switch-name field. */
    apply_visual_state (self, FALSE);
}

PnTasmotaRelayStatus *
pn_tasmota_relay_status_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_RELAY_STATUS, NULL);
}
