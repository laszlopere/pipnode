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

/* Unit tests for the abstract PnTasmotaEnergyMeter base, exercised
 * through a concrete leaf (PnTasmotaPower).  The base adds a two-stage
 * gate in front of the inherited PnAnalogMeter JSON-path resolver:
 *
 *   - an unconfigured meter (empty "switch-name") drops every message,
 *     so a freshly-dropped meter parks the needle at zero;
 *   - only "<prefix>/<device>/SENSOR" topics pass (the last segment must
 *     be the literal "SENSOR"); a STATE/RESULT topic is dropped;
 *   - the device (second-to-last segment) must equal "switch-name";
 *   - only then does it chain up so PnAnalogMeter resolves the leaf's
 *     "key" path and reveals the needle.  A topic that passes the gate
 *     but whose payload lacks the leaf's key still leaves the needle
 *     hidden.
 *
 * The needle "revealed" latch (PnAnalogMeter has_value) is the headless
 * observable: it flips TRUE only when a finite value was extracted and
 * pushed into the meter, so it doubles as the "value drove the meter"
 * signal without a display.  The meter is a pure sink (never forwards). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-analog-meter.h"
#include "pn-tasmota-power.h"

#include <json-glib/json-glib.h>

/* A realistic Tasmota `tele/<dev>/SENSOR` payload, lodged in the data
 * bag under "payload" (so it resolves at root.data.payload.ENERGY.* the
 * way PnAnalogMeter's JSON-path reader walks it).  When @with_power is
 * FALSE the ENERGY object omits Power, modelling a gate-passing topic
 * whose payload nonetheless does not carry the leaf's channel. */
static PnMessage *
make_sensor_message (const gchar *topic, gboolean with_power)
{
    JsonObject *energy  = json_object_new ();
    JsonObject *payload = json_object_new ();
    JsonNode   *pnode   = json_node_new (JSON_NODE_OBJECT);
    PnMessage  *msg     = pn_message_new (NULL, topic);

    if (with_power)
        json_object_set_double_member (energy, "Power", 123.4);
    json_object_set_double_member (energy, "Voltage", 231.5);
    json_object_set_double_member (energy, "Current",   0.654);

    json_object_set_object_member (payload, "ENERGY", energy);
    json_node_take_object         (pnode, payload);
    pn_message_set_member         (msg, "payload", pnode);

    return msg;
}

static gboolean
needle_revealed (PnNode *node)
{
    PnAnalogMeterPaintState st;
    pn_analog_meter_get_paint_state (PN_ANALOG_METER (node), &st);
    return st.has_value;
}

/* Run a single message through a freshly-built Power meter configured
 * for @switch_name and report whether the needle was revealed. */
static gboolean
revealed_after (const gchar *switch_name, const gchar *topic,
                gboolean with_power)
{
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_POWER, NULL);
    PnMessage *msg  = make_sensor_message (topic, with_power);
    gboolean   out;

    if (switch_name != NULL)
        g_object_set (node, "switch-name", switch_name, NULL);

    pn_node_receive_message (node, msg);
    out = needle_revealed (node);

    g_object_unref (msg);
    g_object_unref (node);
    return out;
}

static void
test_switch_name_round_trip (void)
{
    PnNode *node = g_object_new (PN_TYPE_TASMOTA_POWER, NULL);
    gchar  *got  = NULL;

    /* Defaults to unconfigured (NULL/empty). */
    g_object_get (node, "switch-name", &got, NULL);
    PN_CHECK (got == NULL || *got == '\0');
    g_free (got);

    got = NULL;
    g_object_set (node, "switch-name", "sonoff37", NULL);
    g_object_get (node, "switch-name", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "sonoff37");
    g_free (got);

    g_object_unref (node);
}

static void
test_unconfigured_drops (void)
{
    /* No switch-name: every message is dropped, needle stays parked. */
    PN_CHECK_FALSE (revealed_after (NULL, "tele/sonoff37/SENSOR", TRUE));
    PN_CHECK_FALSE (revealed_after ("",   "tele/sonoff37/SENSOR", TRUE));
}

static void
test_non_sensor_dropped (void)
{
    /* Right device, wrong topic family -- only the SENSOR topic carries
     * the ENERGY sub-object, so STATE / RESULT are ignored. */
    PN_CHECK_FALSE (revealed_after ("sonoff37", "tele/sonoff37/STATE",  TRUE));
    PN_CHECK_FALSE (revealed_after ("sonoff37", "stat/sonoff37/RESULT", TRUE));
}

static void
test_wrong_device_dropped (void)
{
    /* SENSOR topic, but a different device than the one configured. */
    PN_CHECK_FALSE (revealed_after ("sonoff37", "tele/sonoff99/SENSOR", TRUE));
}

static void
test_match_drives_needle (void)
{
    /* Topic + device gate pass and the payload carries the key -- the
     * inherited resolver extracts the value and reveals the needle. */
    PN_CHECK (revealed_after ("sonoff37", "tele/sonoff37/SENSOR", TRUE));
}

static void
test_gate_pass_missing_key (void)
{
    /* Gate passes but the payload's ENERGY object lacks Power: the
     * JSON path does not resolve, so no value is pushed and the needle
     * stays hidden. */
    PN_CHECK_FALSE (revealed_after ("sonoff37", "tele/sonoff37/SENSOR", FALSE));
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = g_object_new (PN_TYPE_TASMOTA_POWER, NULL);
    PnMessage *msg   = make_sensor_message ("tele/sonoff37/SENSOR", TRUE);

    g_object_set (node, "switch-name", "sonoff37", NULL);
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-energy-meter");

    pn_test_add ("switch_name_round_trip", test_switch_name_round_trip);
    pn_test_add ("unconfigured_drops",     test_unconfigured_drops);
    pn_test_add ("non_sensor_dropped",     test_non_sensor_dropped);
    pn_test_add ("wrong_device_dropped",   test_wrong_device_dropped);
    pn_test_add ("match_drives_needle",    test_match_drives_needle);
    pn_test_add ("gate_pass_missing_key",  test_gate_pass_missing_key);
    pn_test_add ("is_a_sink",              test_is_a_sink);

    return pn_test_run ();
}
