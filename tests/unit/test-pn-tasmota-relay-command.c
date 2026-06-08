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

/* Unit tests for PnTasmotaRelayCommand, the write-side of the relay pair.
 * It is a translator, not a topic filter: any inbound trigger carrying a
 * numeric data.value is reshaped in place into a Tasmota command envelope
 * (topic cmnd/<switch-name>/POWER, payload "ON"/"OFF") and forwarded for a
 * downstream MQTT Sink to publish.  The mandatory switch-name field gates
 * everything -- an unconfigured node drops every message rather than risk
 * flipping the wrong physical relay -- and a message without a usable
 * numeric value leaves the relay alone.  The on/off split is the midpoint
 * rule value > 0.5, and the original value rides along on data.value. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-relay-command.h"

/* Build a node + a message carrying data.value, receive it, and report
 * the emit count.  The node rewrites @msg in place, so the caller can
 * inspect @msg after the call for the command shape. */
static guint
run_value (const gchar *switch_name, PnMessage *msg, gdouble value)
{
    guint   emits = 0;
    PnNode *node  = g_object_new (PN_TYPE_TASMOTA_RELAY_COMMAND, NULL);

    if (switch_name != NULL)
        g_object_set (node, "switch-name", switch_name, NULL);

    pn_message_set_double (msg, "value", value);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    pn_node_receive_message (node, msg);

    g_object_unref (node);
    return emits;
}

static void
test_unconfigured_drops (void)
{
    /* No switch-name: every message is dropped, even a valid value. */
    PnMessage *msg = pn_message_new (NULL, "trigger");
    PN_CHECK_CMPINT (run_value (NULL, msg, 1.0), ==, 0);
    g_object_unref (msg);
}

static void
test_no_value_drops (void)
{
    /* Configured, but the trigger carries no numeric value: the relay is
     * left alone rather than synthesising a 0 that would command it off. */
    guint      emits = 0;
    PnNode    *node  = g_object_new (PN_TYPE_TASMOTA_RELAY_COMMAND, NULL);
    PnMessage *msg   = pn_message_new (NULL, "trigger");

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_builds_power_on (void)
{
    PnMessage *msg = pn_message_new (NULL, "trigger");

    PN_CHECK_CMPINT (run_value ("sonoff19", msg, 1.0), ==, 1);

    /* Envelope rewritten to the Tasmota command path; payload is the
     * literal "ON" Tasmota expects on the wire; the numeric mirror and
     * the device / success bag members ride along. */
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/sonoff19/POWER");
    PN_CHECK_CMPSTR (pn_test_str (msg, "payload"), ==, "ON");
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 1.0, 1e-9);
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK_CMPSTR (pn_test_str (msg, "device"), ==, "sonoff19");
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "sonoff19: command power on");

    g_object_unref (msg);
}

static void
test_builds_power_off (void)
{
    PnMessage *msg = pn_message_new (NULL, "trigger");

    PN_CHECK_CMPINT (run_value ("sonoff19", msg, 0.0), ==, 1);

    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/sonoff19/POWER");
    PN_CHECK_CMPSTR (pn_test_str (msg, "payload"), ==, "OFF");
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 0.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "sonoff19: command power off");

    g_object_unref (msg);
}

static void
test_midpoint_threshold (void)
{
    /* The on/off split is value > 0.5 (the project's boolean midpoint),
     * not value > 0.0: exactly 0.5 is still off, 0.6 is on. */
    PnMessage *a = pn_message_new (NULL, "trigger");
    PnMessage *b = pn_message_new (NULL, "trigger");

    run_value ("sonoff19", a, 0.5);
    PN_CHECK_CMPSTR (pn_test_str (a, "payload"), ==, "OFF");

    run_value ("sonoff19", b, 0.6);
    PN_CHECK_CMPSTR (pn_test_str (b, "payload"), ==, "ON");

    g_object_unref (a);
    g_object_unref (b);
}

static void
test_empty_name_drops (void)
{
    /* Empty string is the same unconfigured state as NULL. */
    PnMessage *msg = pn_message_new (NULL, "trigger");
    PN_CHECK_CMPINT (run_value ("", msg, 1.0), ==, 0);
    g_object_unref (msg);
}

static void
test_int_value_accepted (void)
{
    /* data.value may arrive as a JSON integer (e.g. from a Knob) as well
     * as a double; both drive the relay. */
    guint      emits = 0;
    PnNode    *node  = g_object_new (PN_TYPE_TASMOTA_RELAY_COMMAND, NULL);
    PnMessage *msg   = pn_message_new (NULL, "trigger");

    g_object_set (node, "switch-name", "sonoff19", NULL);
    pn_message_set_int (msg, "value", 1);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "payload"), ==, "ON");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_name_round_trips (void)
{
    gchar  *name = NULL;
    PnNode *node = g_object_new (PN_TYPE_TASMOTA_RELAY_COMMAND, NULL);

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_object_get (node, "switch-name", &name, NULL);
    PN_CHECK_CMPSTR (name, ==, "sonoff19");
    g_free (name);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-relay-command");

    pn_test_add ("unconfigured_drops",   test_unconfigured_drops);
    pn_test_add ("no_value_drops",       test_no_value_drops);
    pn_test_add ("builds_power_on",      test_builds_power_on);
    pn_test_add ("builds_power_off",     test_builds_power_off);
    pn_test_add ("midpoint_threshold",   test_midpoint_threshold);
    pn_test_add ("empty_name_drops",     test_empty_name_drops);
    pn_test_add ("int_value_accepted",   test_int_value_accepted);
    pn_test_add ("name_round_trips",     test_name_round_trips);

    return pn_test_run ();
}
