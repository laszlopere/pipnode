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

#include "pn-tasmota-temperature.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <json-glib/json-glib.h>

/* fa-thermometer-half U+F2C9 — readable on the green/yellow node body
 * the palette draws for filter-shaped nodes. */
#define PN_TASMOTA_TEMPERATURE_ICON "\xef\x8b\x89"

struct _PnTasmotaTemperature
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnTasmotaTemperature, pn_tasmota_temperature, PN_TYPE_NODE)

/** Pull data.payload off @message as a JSON object, or %NULL when
 *  the message does not carry a structured payload (raw-string MQTT
 *  publishes land here too via PnMqtt's text fallback). */
static JsonObject *
get_payload_object (PnMessage *message)
{
    JsonNode *node;

    node = pn_message_get_member (message, "payload");
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    return json_node_get_object (node);
}

static void
pn_tasmota_temperature_receive (
        PnNode    *node,
        PnMessage *message)
{
    JsonObject  *payload;
    gdouble      temperature;
    const gchar *unit;
    gchar       *device = NULL;
    gchar       *output;

    payload = get_payload_object (message);
    if (payload == NULL)
        return;

    if (!pn_tasmota_find_number (payload, "Temperature", &temperature))
        return;

    /* Tasmota's TempUnit lives at the top level of the payload
     * alongside the per-sensor objects ("AM2301", "DHT22", ...).
     * Default to "C" when the firmware omits it — that matches the
     * Tasmota build default. */
    unit = pn_tasmota_find_top_string (payload, "TempUnit");
    if (unit == NULL || *unit == '\0')
        unit = "C";

    device = pn_tasmota_device_from_topic (pn_message_get_topic (message));

    pn_message_set_double  (message, "value",   temperature);
    pn_message_set_string  (message, "unit",    unit);
    if (device != NULL)
        pn_message_set_string (message, "device", device);
    pn_message_set_boolean (message, "success", TRUE);

    output = g_strdup_printf ("%s%s temperature %.1f °%s",
                              device != NULL ? device : "",
                              device != NULL ? ":"    : "",
                              temperature, unit);
    pn_message_set_string (message, "output", output);
    g_free (output);
    g_free (device);

    pn_node_emit_message (node, message);
}

static void
pn_tasmota_temperature_class_init (PnTasmotaTemperatureClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive       = pn_tasmota_temperature_receive;

    node_class->palette_icon  = PN_TASMOTA_TEMPERATURE_ICON;
    node_class->class_name    = "Tasmota Temperature";
    node_class->icon          = PN_TASMOTA_TEMPERATURE_ICON;
    node_class->color         = (PnColor){ 0.85, 0.45, 0.30, 1.0 };
    node_class->category      = "Tasmota";
    node_class->has_input     = TRUE;
    node_class->has_output    = TRUE;
}

static void
pn_tasmota_temperature_init (PnTasmotaTemperature *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  red  = { 0.85, 0.45, 0.30, 1.0 };

    pn_node_set_class_name (node, "Tasmota Temperature");
    pn_node_set_icon       (node, PN_TASMOTA_TEMPERATURE_ICON);
    pn_node_set_color      (node, &red);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnTasmotaTemperature *
pn_tasmota_temperature_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_TEMPERATURE, NULL);
}
