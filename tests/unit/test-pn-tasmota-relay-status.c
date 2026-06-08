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

/* Unit tests for PnTasmotaRelayStatus, the read-side of the relay pair.
 * It listens for a device's relay-state publishes (stat/<device>/POWER and
 * the numbered POWER1..POWER8 variants), accepts ONLY the bare "ON"/"OFF"
 * payloads Tasmota emits, and decodes them onto data.value (1.0/0.0) for
 * downstream LEDs / graphs.  The mandatory switch-name field gates inbound
 * by exact device name (extracted, not substring-matched, so "sonoff1"
 * never accepts "sonoff19"); foreign topics, the wrong topic section,
 * non-binary payloads and a missing payload are all dropped silently. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-relay-status.h"

/* Capture the last emitted message (ref'd so it survives the dispatcher
 * unref) alongside a count. */
typedef struct {
    guint      count;
    PnMessage *last;
} Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;
    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

/* Feed one stat publish to a configured node; return the capture. */
static void
feed (Capture *cap, const gchar *switch_name,
      const gchar *topic, const gchar *payload)
{
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_RELAY_STATUS, NULL);
    PnMessage *msg  = pn_message_new (NULL, topic);

    if (switch_name != NULL)
        g_object_set (node, "switch-name", switch_name, NULL);
    if (payload != NULL)
        pn_message_set_string (msg, "payload", payload);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    pn_node_receive_message (node, msg);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_unconfigured_drops (void)
{
    Capture cap = { 0, NULL };
    feed (&cap, NULL, "stat/sonoff19/POWER", "ON");
    PN_CHECK_CMPINT (cap.count, ==, 0);
    g_clear_object (&cap.last);
}

static void
test_accepts_on (void)
{
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff19", "stat/sonoff19/POWER", "ON");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "device"), ==, "sonoff19");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "sonoff19: power on");

    g_clear_object (&cap.last);
}

static void
test_accepts_off (void)
{
    Capture cap = { 0, NULL };
    /* Case-insensitive match: Tasmota sends "OFF", but "off" decodes too. */
    feed (&cap, "sonoff19", "stat/sonoff19/POWER", "off");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 0.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "sonoff19: power off");

    g_clear_object (&cap.last);
}

static void
test_numbered_power_accepted (void)
{
    /* Multi-relay devices publish POWER1..POWER8; the last-segment prefix
     * check covers them. */
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff19", "stat/sonoff19/POWER2", "ON");
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);
    g_clear_object (&cap.last);
}

static void
test_rejects_other_device (void)
{
    /* Exact device match: "sonoff1" must not accept "sonoff19"'s publish. */
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff1", "stat/sonoff19/POWER", "ON");
    PN_CHECK_CMPINT (cap.count, ==, 0);
    g_clear_object (&cap.last);
}

static void
test_rejects_non_power_topic (void)
{
    /* Right device, wrong section: RESULT / STATE are not relay state. */
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff19", "stat/sonoff19/RESULT", "ON");
    PN_CHECK_CMPINT (cap.count, ==, 0);
    g_clear_object (&cap.last);
}

static void
test_rejects_non_binary_payload (void)
{
    /* "TOGGLE" / "Blink" / "PulseTime" land on the same POWER topic but
     * are not binary states -- they leave the latch alone. */
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff19", "stat/sonoff19/POWER", "TOGGLE");
    PN_CHECK_CMPINT (cap.count, ==, 0);
    g_clear_object (&cap.last);
}

static void
test_missing_payload_drops (void)
{
    Capture cap = { 0, NULL };
    feed (&cap, "sonoff19", "stat/sonoff19/POWER", NULL);
    PN_CHECK_CMPINT (cap.count, ==, 0);
    g_clear_object (&cap.last);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-relay-status");

    pn_test_add ("unconfigured_drops",      test_unconfigured_drops);
    pn_test_add ("accepts_on",              test_accepts_on);
    pn_test_add ("accepts_off",             test_accepts_off);
    pn_test_add ("numbered_power_accepted", test_numbered_power_accepted);
    pn_test_add ("rejects_other_device",    test_rejects_other_device);
    pn_test_add ("rejects_non_power_topic", test_rejects_non_power_topic);
    pn_test_add ("rejects_non_binary",      test_rejects_non_binary_payload);
    pn_test_add ("missing_payload_drops",   test_missing_payload_drops);

    return pn_test_run ();
}
