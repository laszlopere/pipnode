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

#include "pn-tasmota-status-request.h"

#include "pn-message.h"

#include <json-glib/json-glib.h>

/* fa-question-circle U+F059 -- the node "asks" a device for its
 * configuration / info, so the question mark reads naturally and keeps
 * it visually distinct from the toggle glyph the relay write-side pair
 * uses.  FontAwesome 4.7 only (see project convention). */
#define PN_TASMOTA_STATUS_REQUEST_ICON        "\xef\x81\x99"

/* Tasmota's built-in group topic: a command published to
 * cmnd/tasmotas/<Command> is acted on by *every* Tasmota device that
 * shares the broker.  Used as the default and as the fallback for an
 * empty device field, so the node always has a safe target. */
#define PN_TASMOTA_GROUP_TOPIC_ALL            "tasmotas"

struct _PnTasmotaStatusRequest
{
    PnNode parent_instance;

    /* Target device name, or the "tasmotas" group topic to reach every
     * device.  Never the unconfigured/error state -- a Status query is
     * read-only, so broadcasting is a perfectly safe default. */
    gchar               *device;

    /* Which Status <n> section to ask for; the enum value is the wire
     * argument. */
    PnTasmotaStatusKind  request;
};

G_DEFINE_TYPE (PnTasmotaStatusRequest, pn_tasmota_status_request, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_DEVICE,
    PROP_REQUEST,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Request-kind enum type                                            */
/* ------------------------------------------------------------------ */

/* The nicks (third column) are the user-visible combobox labels and the
 * tokens serialised into saved worksheets -- keep them stable.  Each
 * carries the raw "Status <n>" form so power users can map the choice
 * straight onto Tasmota's documented command. */
#define PN_TYPE_TASMOTA_STATUS_KIND (pn_tasmota_status_kind_get_type ())

static GType
pn_tasmota_status_kind_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_TASMOTA_STATUS_ALL,              "PN_TASMOTA_STATUS_ALL",
              "Everything (Status 0)"           },
            { PN_TASMOTA_STATUS_PARAMETERS,       "PN_TASMOTA_STATUS_PARAMETERS",
              "Device parameters (Status 1)"    },
            { PN_TASMOTA_STATUS_FIRMWARE,         "PN_TASMOTA_STATUS_FIRMWARE",
              "Firmware version (Status 2)"     },
            { PN_TASMOTA_STATUS_LOGGING,          "PN_TASMOTA_STATUS_LOGGING",
              "Logging & telemetry (Status 3)"  },
            { PN_TASMOTA_STATUS_MEMORY,           "PN_TASMOTA_STATUS_MEMORY",
              "Memory & flash (Status 4)"       },
            { PN_TASMOTA_STATUS_NETWORK,          "PN_TASMOTA_STATUS_NETWORK",
              "Network (Status 5)"              },
            { PN_TASMOTA_STATUS_MQTT,             "PN_TASMOTA_STATUS_MQTT",
              "MQTT (Status 6)"                 },
            { PN_TASMOTA_STATUS_TIME,             "PN_TASMOTA_STATUS_TIME",
              "Time & location (Status 7)"      },
            { PN_TASMOTA_STATUS_CONNECTED_SENSOR, "PN_TASMOTA_STATUS_CONNECTED_SENSOR",
              "Connected sensors (Status 8)"    },
            { PN_TASMOTA_STATUS_POWER_THRESHOLDS, "PN_TASMOTA_STATUS_POWER_THRESHOLDS",
              "Power thresholds (Status 9)"     },
            { PN_TASMOTA_STATUS_SENSOR_DATA,      "PN_TASMOTA_STATUS_SENSOR_DATA",
              "Sensor data (Status 10)"         },
            { PN_TASMOTA_STATUS_STATE,            "PN_TASMOTA_STATUS_STATE",
              "Device state (Status 11)"        },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnTasmotaStatusKind", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Resolve the device name to target: the configured name, or the
 *  built-in "tasmotas" group topic (all devices) when the field is
 *  empty.  Never returns NULL. */
static const gchar *
resolve_target (PnTasmotaStatusRequest *self)
{
    if (self->device != NULL && *self->device != '\0')
        return self->device;
    return PN_TASMOTA_GROUP_TOPIC_ALL;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_status_request_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaStatusRequest *self   = PN_TASMOTA_STATUS_REQUEST (node);
    const gchar            *target = resolve_target (self);
    gchar                  *topic;
    gchar                  *payload;
    gchar                  *output;

    /* Every inbound message is a bare trigger -- the value carried in
     * the data bag is irrelevant, so a Button, Knob, timer or any other
     * source can drive the query.  We reshape the message in place (the
     * way PnTasmotaRelayCommand does) and forward it to a PnMqttSink. */

    /* Rewrite the envelope topic to the Tasmota command path so the
     * downstream sink publishes straight to cmnd/<target>/Status with
     * no intervening Rewrite node. */
    topic = g_strdup_printf ("cmnd/%s/Status", target);
    pn_message_set_topic (message, topic);
    g_free (topic);

    /* The Status argument rides on data.payload as a plain string so
     * PnMqttSink's default payload path puts exactly "<n>" on the wire,
     * which is what Tasmota expects (e.g. "Status 0"). */
    payload = g_strdup_printf ("%d", (gint) self->request);
    pn_message_set_string  (message, "payload", payload);
    g_free (payload);

    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  target);

    output = g_strdup_printf ("%s: request Status %d",
                              target, (gint) self->request);
    pn_message_set_string (message, "output", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
tasmota_status_request_set_device (
        PnTasmotaStatusRequest *self,
        const gchar            *device)
{
    /* Round-trip whatever the caller passed (including "") so the dialog
     * sees a stable value on notify; resolve_target() turns empty into
     * the group topic at send time. */
    g_free (self->device);
    self->device = (device != NULL) ? g_strdup (device) : NULL;

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DEVICE]);
}

static void
pn_tasmota_status_request_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaStatusRequest *self = PN_TASMOTA_STATUS_REQUEST (object);

    switch (prop_id)
    {
    case PROP_DEVICE:
        g_value_set_string (value, self->device);
        break;
    case PROP_REQUEST:
        g_value_set_enum (value, self->request);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_status_request_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaStatusRequest *self = PN_TASMOTA_STATUS_REQUEST (object);

    switch (prop_id)
    {
    case PROP_DEVICE:
        tasmota_status_request_set_device (self, g_value_get_string (value));
        break;
    case PROP_REQUEST:
        self->request = g_value_get_enum (value);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_REQUEST]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_status_request_finalize (GObject *object)
{
    PnTasmotaStatusRequest *self = PN_TASMOTA_STATUS_REQUEST (object);

    g_clear_pointer (&self->device, g_free);

    G_OBJECT_CLASS (pn_tasmota_status_request_parent_class)->finalize (object);
}

static void
pn_tasmota_status_request_class_init (PnTasmotaStatusRequestClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_status_request_get_property;
    object_class->set_property = pn_tasmota_status_request_set_property;
    object_class->finalize     = pn_tasmota_status_request_finalize;

    node_class->receive      = pn_tasmota_status_request_receive;

    node_class->palette_icon = PN_TASMOTA_STATUS_REQUEST_ICON;
    node_class->class_name   = "Tasmota Status Request";
    node_class->icon         = PN_TASMOTA_STATUS_REQUEST_ICON;
    /* A saturated teal to read as a read/query node, distinct from the
     * steel-blue write-side relay command while still sitting in the
     * Tasmota family.  Kept well clear of grey, which the worksheet
     * reserves for disabled nodes. */
    node_class->color        = (PnColor){ 0.12, 0.63, 0.58, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Tasmota device to query.  Stamped into the outgoing "
            "envelope topic as cmnd/<device>/Status so a downstream "
            "MQTT Sink publishes straight to the right device.  Leave "
            "as (or set to) \"tasmotas\" -- Tasmota's built-in group "
            "topic -- to ask every device that hears the broker at "
            "once; an empty field is treated the same way.  A Status "
            "query is read-only, so broadcasting is safe.",
            PN_TASMOTA_GROUP_TOPIC_ALL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_REQUEST] = g_param_spec_enum (
            "request", "Request",
            "Which Tasmota Status section to ask for.  \"Everything "
            "(Status 0)\" returns the full report; the other choices "
            "request one specific section (network, MQTT, firmware, "
            "sensors, ...).  The reply arrives back through an MQTT "
            "Source subscribed to stat/<device>/STATUS#.",
            PN_TYPE_TASMOTA_STATUS_KIND, PN_TASMOTA_STATUS_ALL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_status_request_init (PnTasmotaStatusRequest *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.12, 0.63, 0.58, 1.0 };

    self->device  = g_strdup (PN_TASMOTA_GROUP_TOPIC_ALL);
    self->request = PN_TASMOTA_STATUS_ALL;

    pn_node_set_class_name (node, "Tasmota Status Request");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* The class-level icon/color seed the palette widget; the canvas
     * node draws from the *instance*, so set them here too (project
     * convention: set the icon in both class_init and init). */
    pn_node_set_icon  (node, PN_TASMOTA_STATUS_REQUEST_ICON);
    pn_node_set_color (node, &teal);
}

PnTasmotaStatusRequest *
pn_tasmota_status_request_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
}
