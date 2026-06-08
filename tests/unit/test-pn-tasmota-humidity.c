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

/* Unit tests for PnTasmotaHumidity.  Unlike the energy meters this node
 * subclasses PnNode directly and is a forwarder, not a sink: it scans the
 * data.payload object for a "Humidity" reading (walking into the per-
 * sensor child object Tasmota wraps it in, e.g. "AM2301"), stamps the
 * envelope with data.value, data.unit = "%", data.device (from the topic)
 * and data.success = TRUE, then emits the (mutated) message downstream.
 * A message with no structured payload, or a payload that carries no
 * Humidity, is dropped silently (no emit, no mutation). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-humidity.h"

#include <json-glib/json-glib.h>

/* A realistic DHT/AM2301 SENSOR payload nested under data.payload.  The
 * humidity reading lives inside the per-sensor child object, exercising
 * pn_tasmota_find_number()'s one-level descent.  @with_humidity omits the
 * Humidity member to model a sensor that only reports temperature. */
static PnMessage *
make_dht_message (const gchar *topic, gboolean with_humidity)
{
    JsonObject *sensor  = json_object_new ();
    JsonObject *payload = json_object_new ();
    JsonNode   *pnode   = json_node_new (JSON_NODE_OBJECT);
    PnMessage  *msg     = pn_message_new (NULL, topic);

    json_object_set_double_member (sensor, "Temperature", 21.7);
    if (with_humidity)
        json_object_set_double_member (sensor, "Humidity", 48.3);
    json_object_set_double_member (sensor, "DewPoint", 10.2);

    json_object_set_object_member (payload, "AM2301", sensor);
    json_object_set_string_member (payload, "TempUnit", "C");
    json_node_take_object         (pnode, payload);
    pn_message_set_member         (msg, "payload", pnode);

    return msg;
}

static void
test_reads_humidity (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_humidity_new ());
    PnMessage *msg   = make_dht_message ("tele/sonoff37/SENSOR", TRUE);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);                 /* forwards, not a sink */
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 48.3, 1e-6);
    PN_CHECK_CMPSTR (pn_test_str (msg, "unit"),   ==, "%");
    PN_CHECK_CMPSTR (pn_test_str (msg, "device"), ==, "sonoff37");
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK        (pn_test_str (msg, "output") != NULL);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_payload_dropped (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_humidity_new ());
    PnMessage *msg   = pn_message_new (NULL, "tele/sonoff37/SENSOR");

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* No data.payload object at all -- nothing to read, nothing emitted. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE  (pn_test_has (msg, "value"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_humidity_dropped (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_humidity_new ());
    PnMessage *msg   = make_dht_message ("tele/sonoff37/SENSOR", FALSE);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Payload present but carries no Humidity member. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE  (pn_test_has (msg, "value"));

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-humidity");

    pn_test_add ("reads_humidity",      test_reads_humidity);
    pn_test_add ("no_payload_dropped",  test_no_payload_dropped);
    pn_test_add ("no_humidity_dropped", test_no_humidity_dropped);

    return pn_test_run ();
}
