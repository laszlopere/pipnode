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

/* Unit tests for PnTasmotaReactivePower.  This leaf reads the reactive
 * power Q off data/payload/ENERGY/ReactivePower, scales the dial 0..5000
 * (shared with the real/apparent meters), labels it "var" and -- a
 * quantity with no meaningful DC sense -- suppresses the current-type
 * glyph (mode None).  The gate behaviour inherited from
 * PnTasmotaEnergyMeter is covered by the energy-meter test; here we check
 * the leaf's identity and that its own key path resolves and reveals the
 * needle, and that it is a sink. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-analog-meter.h"
#include "pn-tasmota-reactive-power.h"

#include <json-glib/json-glib.h>

static PnMessage *
make_sensor_message (const gchar *topic)
{
    JsonObject *energy  = json_object_new ();
    JsonObject *payload = json_object_new ();
    JsonNode   *pnode   = json_node_new (JSON_NODE_OBJECT);
    PnMessage  *msg     = pn_message_new (NULL, topic);

    json_object_set_double_member (energy, "Power",         123.4);
    json_object_set_double_member (energy, "ApparentPower", 150.0);
    json_object_set_double_member (energy, "ReactivePower",  85.2);
    json_object_set_double_member (energy, "Factor",          0.82);
    json_object_set_double_member (energy, "Voltage",       231.5);
    json_object_set_double_member (energy, "Current",         0.654);

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

static void
test_meter_identity (void)
{
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_REACTIVE_POWER, NULL);
    gchar   *key  = NULL;
    gchar   *unit = NULL;
    gdouble  minv = -1.0;
    gdouble  maxv = -1.0;
    gint     mode = -1;

    g_object_get (node,
                  "key",       &key,
                  "unit",      &unit,
                  "min-value", &minv,
                  "max-value", &maxv,
                  "mode",      &mode,
                  NULL);

    PN_CHECK_CMPSTR (key,  ==, "data/payload/ENERGY/ReactivePower");
    PN_CHECK_CMPSTR (unit, ==, "var");
    PN_CHECK_NEAR   (minv,    0.0, 1e-9);
    PN_CHECK_NEAR   (maxv, 5000.0, 1e-9);
    PN_CHECK_CMPINT (mode, ==, PN_ANALOG_METER_MODE_NONE);

    g_free (key);
    g_free (unit);
    g_object_unref (node);
}

static void
test_reads_reactive_channel (void)
{
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_REACTIVE_POWER, NULL);
    PnMessage *msg  = make_sensor_message ("tele/sonoff37/SENSOR");

    g_object_set (node, "switch-name", "sonoff37", NULL);

    PN_CHECK_FALSE (needle_revealed (node));   /* parked until first value */
    pn_node_receive_message (node, msg);
    PN_CHECK (needle_revealed (node));         /* ENERGY/ReactivePower */

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = g_object_new (PN_TYPE_TASMOTA_REACTIVE_POWER, NULL);
    PnMessage *msg   = make_sensor_message ("tele/sonoff37/SENSOR");

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
    pn_test_init (&argc, &argv, "pn-tasmota-reactive-power");

    pn_test_add ("meter_identity",         test_meter_identity);
    pn_test_add ("reads_reactive_channel", test_reads_reactive_channel);
    pn_test_add ("is_a_sink",              test_is_a_sink);

    return pn_test_run ();
}
