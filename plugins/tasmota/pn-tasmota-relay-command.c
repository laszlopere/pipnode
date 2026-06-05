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

#include "pn-tasmota-relay-command.h"

#include "pn-message.h"

#include <json-glib/json-glib.h>

/* fa-toggle-on U+F205 -- same glyph as PnTasmotaRelayStatus so the
 * read-side / write-side pair reads as siblings.  Flipped to the
 * warning ❗ glyph (U+2757) while the switch-name field is empty so
 * an unconfigured node visibly refuses to operate -- consistent with
 * PnInject and PnTasmotaRelayStatus, and especially load-bearing here
 * because operating the wrong physical relay is a real-world cost
 * the warning state is meant to prevent. */
#define PN_TASMOTA_RELAY_COMMAND_ICON         "\xef\x88\x85"

struct _PnTasmotaRelayCommand
{
    PnNode parent_instance;

    /* Mandatory.  Empty / NULL marks the node as needing setup and
     * rejects every message -- there is no safe default we could pick
     * on the user's behalf since silently defaulting would risk
     * flipping the wrong physical relay. */
    gchar *switch_name;
};

G_DEFINE_TYPE (PnTasmotaRelayCommand, pn_tasmota_relay_command, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SWITCH_NAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (
        PnTasmotaRelayCommand *self,
        gboolean               configured)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  steel = { 0.42, 0.55, 0.72, 1.0 };

    /* Keep the healthy steel identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &steel);
    pn_node_set_icon      (node, PN_TASMOTA_RELAY_COMMAND_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Value parsing                                                      */
/* ------------------------------------------------------------------ */

/** Pull a numeric `data.value` off @data.  Accepts JSON int and
 *  double; reports FALSE for missing / non-numeric so the receiver
 *  can ignore the message without synthesising a 0 that would silently
 *  command the relay off. */
static gboolean
read_value_member (
        JsonObject *data,
        gdouble    *out_value)
{
    JsonNode *n;
    GType     t;

    if (data == NULL || !json_object_has_member (data, "value"))
        return FALSE;

    n = json_object_get_member (data, "value");
    if (!JSON_NODE_HOLDS_VALUE (n))
        return FALSE;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        *out_value = json_node_get_double (n);
    else if (t == G_TYPE_INT64)
        *out_value = (gdouble) json_node_get_int (n);
    else
        return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_relay_command_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaRelayCommand *self = PN_TASMOTA_RELAY_COMMAND (node);
    gdouble                value;
    gboolean               on;
    gchar                 *topic;
    gchar                 *output;

    /* Unconfigured nodes drop every message -- the whole point of the
     * mandatory switch-name field is that we must never command a
     * physical relay without an explicit per-instance target. */
    if (self->switch_name == NULL || *self->switch_name == '\0')
        return;

    /* Messages without a usable numeric value leave the relay alone --
     * same rule PnTasmotaRelayStatus uses on the read side, so the
     * pair stays internally consistent. */
    if (!read_value_member (pn_message_get_data (message), &value))
        return;

    on = (value > 0.5);

    /* Rewrite the envelope topic so a downstream PnMqttSink publishes
     * straight to the Tasmota command topic without an intervening
     * PnRewrite step.  The plain `cmnd/<dev>/POWER` shape matches what
     * Tasmota itself documents and reads cleanly on the Debug pane. */
    topic = g_strdup_printf ("cmnd/%s/POWER", self->switch_name);
    pn_message_set_topic (message, topic);
    g_free (topic);

    /* Reshape the data bag so PnMqttSink's default payload path
     * (publish data.payload as raw bytes when it is a string) puts
     * exactly the "ON" / "OFF" literal Tasmota expects on the wire.
     * The original numeric value rides along on data.value so any
     * downstream branch (an LED indicator wired off the same point)
     * can still mirror the on/off state without parsing the payload
     * string. */
    pn_message_set_string  (message, "payload", on ? "ON" : "OFF");
    pn_message_set_double  (message, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "device",  self->switch_name);

    output = g_strdup_printf ("%s: command power %s",
                              self->switch_name, on ? "on" : "off");
    pn_message_set_string (message, "output", output);
    g_free (output);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
tasmota_relay_command_set_name (
        PnTasmotaRelayCommand *self,
        const gchar           *name)
{
    /* Normalise NULL / "" so both forms produce the same unconfigured
     * state, but round-trip whatever the caller passed so the dialog
     * sees a stable value on notify::switch-name. */
    gchar    *replacement = (name != NULL) ? g_strdup (name) : NULL;
    gboolean  configured  = (name != NULL && *name != '\0');

    g_free (self->switch_name);
    self->switch_name = replacement;

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SWITCH_NAME]);
}

static void
pn_tasmota_relay_command_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaRelayCommand *self = PN_TASMOTA_RELAY_COMMAND (object);

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
pn_tasmota_relay_command_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaRelayCommand *self = PN_TASMOTA_RELAY_COMMAND (object);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        tasmota_relay_command_set_name (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_relay_command_finalize (GObject *object)
{
    PnTasmotaRelayCommand *self = PN_TASMOTA_RELAY_COMMAND (object);

    g_clear_pointer (&self->switch_name, g_free);

    G_OBJECT_CLASS (pn_tasmota_relay_command_parent_class)->finalize (object);
}

static void
pn_tasmota_relay_command_class_init (PnTasmotaRelayCommandClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_relay_command_get_property;
    object_class->set_property = pn_tasmota_relay_command_set_property;
    object_class->finalize     = pn_tasmota_relay_command_finalize;

    node_class->receive      = pn_tasmota_relay_command_receive;

    node_class->palette_icon = PN_TASMOTA_RELAY_COMMAND_ICON;
    node_class->class_name   = "Tasmota Relay Command";
    node_class->icon         = PN_TASMOTA_RELAY_COMMAND_ICON;
    node_class->color        = (PnColor){ 0.42, 0.55, 0.72, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_SWITCH_NAME] = g_param_spec_string (
            "switch-name", "Switch name",
            "Tasmota device name to command.  Stamped into the "
            "outgoing envelope topic as cmnd/<switch-name>/POWER so "
            "a downstream MQTT Sink publishes straight to the right "
            "relay.  Empty marks the node as needing configuration "
            "and rejects every message -- mandatory by design, since "
            "silently defaulting would risk flipping the wrong "
            "physical relay.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_relay_command_init (PnTasmotaRelayCommand *self)
{
    PnNode *node = PN_NODE (self);

    self->switch_name = NULL;

    pn_node_set_class_name (node, "Tasmota Relay Command");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped
     * node visibly refuses to operate until the user fills in the
     * switch-name field. */
    apply_visual_state (self, FALSE);
}

PnTasmotaRelayCommand *
pn_tasmota_relay_command_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_RELAY_COMMAND, NULL);
}
