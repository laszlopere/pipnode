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

/* Unit tests for PnTasmotaTemperature.  Like the humidity node it
 * subclasses PnNode directly and forwards rather than sinks: it scans
 * data.payload for a "Temperature" reading (descending into the per-
 * sensor child object), reads the unit from the top-level "TempUnit"
 * (defaulting to "C" when the firmware omits it), and stamps the envelope
 * with data.value, data.unit, data.device and data.success before
 * emitting.  A message with no structured payload, or one carrying no
 * Temperature, is dropped silently. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-temperature.h"

#include <json-glib/json-glib.h>

/* A realistic AM2301 SENSOR payload nested under data.payload.  The
 * temperature lives in the per-sensor child object; TempUnit sits at the
 * top level.  Pass @temp_unit = NULL to omit TempUnit (firmware default),
 * or @with_temp = FALSE to model a sensor reporting no temperature. */
static PnMessage *
make_dht_message (const gchar *topic, gboolean with_temp,
                  const gchar *temp_unit)
{
    JsonObject *sensor  = json_object_new ();
    JsonObject *payload = json_object_new ();
    JsonNode   *pnode   = json_node_new (JSON_NODE_OBJECT);
    PnMessage  *msg     = pn_message_new (NULL, topic);

    if (with_temp)
        json_object_set_double_member (sensor, "Temperature", 21.7);
    json_object_set_double_member (sensor, "Humidity", 48.3);

    json_object_set_object_member (payload, "AM2301", sensor);
    if (temp_unit != NULL)
        json_object_set_string_member (payload, "TempUnit", temp_unit);
    json_node_take_object         (pnode, payload);
    pn_message_set_member         (msg, "payload", pnode);

    return msg;
}

static void
test_reads_temperature (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_temperature_new ());
    PnMessage *msg   = make_dht_message ("tele/sonoff37/SENSOR", TRUE, "C");

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);                 /* forwards, not a sink */
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 21.7, 1e-6);
    PN_CHECK_CMPSTR (pn_test_str (msg, "unit"),   ==, "C");
    PN_CHECK_CMPSTR (pn_test_str (msg, "device"), ==, "sonoff37");
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK        (pn_test_str (msg, "output") != NULL);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_unit_defaults_to_c (void)
{
    PnNode    *node = PN_NODE (pn_tasmota_temperature_new ());
    PnMessage *msg  = make_dht_message ("tele/sonoff37/SENSOR", TRUE, NULL);

    /* No TempUnit in the payload -- the node falls back to "C". */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPSTR (pn_test_str (msg, "unit"), ==, "C");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_unit_honours_fahrenheit (void)
{
    PnNode    *node = PN_NODE (pn_tasmota_temperature_new ());
    PnMessage *msg  = make_dht_message ("tele/sonoff37/SENSOR", TRUE, "F");

    /* A Fahrenheit build's TempUnit is carried through verbatim. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPSTR (pn_test_str (msg, "unit"), ==, "F");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_payload_dropped (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_temperature_new ());
    PnMessage *msg   = pn_message_new (NULL, "tele/sonoff37/SENSOR");

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE  (pn_test_has (msg, "value"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_temperature_dropped (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_tasmota_temperature_new ());
    PnMessage *msg   = make_dht_message ("tele/sonoff37/SENSOR", FALSE, "C");

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Payload present but carries no Temperature member. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE  (pn_test_has (msg, "value"));

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-temperature");

    pn_test_add ("reads_temperature",      test_reads_temperature);
    pn_test_add ("unit_defaults_to_c",     test_unit_defaults_to_c);
    pn_test_add ("unit_honours_fahrenheit", test_unit_honours_fahrenheit);
    pn_test_add ("no_payload_dropped",     test_no_payload_dropped);
    pn_test_add ("no_temperature_dropped", test_no_temperature_dropped);

    return pn_test_run ();
}
