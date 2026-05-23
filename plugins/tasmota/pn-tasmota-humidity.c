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

#include "pn-tasmota-humidity.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <json-glib/json-glib.h>

/* fa-tint U+F043 — water-drop glyph, the conventional humidity icon
 * across the dashboard / weather tooling Tasmota usually feeds. */
#define PN_TASMOTA_HUMIDITY_ICON "\xef\x81\x83"

struct _PnTasmotaHumidity
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnTasmotaHumidity, pn_tasmota_humidity, PN_TYPE_NODE)

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
pn_tasmota_humidity_receive (
        PnNode    *node,
        PnMessage *message)
{
    JsonObject *payload;
    gdouble     humidity;
    gchar      *device = NULL;
    gchar      *output;

    payload = get_payload_object (message);
    if (payload == NULL)
        return;

    if (!pn_tasmota_find_number (payload, "Humidity", &humidity))
        return;

    device = pn_tasmota_device_from_topic (pn_message_get_topic (message));

    pn_message_set_double  (message, "value",   humidity);
    pn_message_set_string  (message, "unit",    "%");
    if (device != NULL)
        pn_message_set_string (message, "device", device);
    pn_message_set_boolean (message, "success", TRUE);

    output = g_strdup_printf ("%s%s humidity %.1f%%",
                              device != NULL ? device : "",
                              device != NULL ? ":"    : "",
                              humidity);
    pn_message_set_string (message, "output", output);
    g_free (output);
    g_free (device);

    pn_node_emit_message (node, message);
}

static void
pn_tasmota_humidity_class_init (PnTasmotaHumidityClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive       = pn_tasmota_humidity_receive;

    node_class->palette_icon  = PN_TASMOTA_HUMIDITY_ICON;
    node_class->class_name    = "Tasmota Humidity";
    node_class->icon          = PN_TASMOTA_HUMIDITY_ICON;
    node_class->color         = (PnColor){ 0.30, 0.55, 0.85, 1.0 };
    node_class->category      = "Tasmota";
    node_class->has_input     = TRUE;
    node_class->has_output    = TRUE;
}

static void
pn_tasmota_humidity_init (PnTasmotaHumidity *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.30, 0.55, 0.85, 1.0 };

    pn_node_set_class_name (node, "Tasmota Humidity");
    pn_node_set_icon       (node, PN_TASMOTA_HUMIDITY_ICON);
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnTasmotaHumidity *
pn_tasmota_humidity_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_HUMIDITY, NULL);
}
