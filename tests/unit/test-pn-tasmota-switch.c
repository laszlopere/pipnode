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

/* Unit tests for PnTasmotaSwitch, the canvas-side latch that fuses the
 * relay read + write sides into one slide switch.  Two seams:
 *
 *   Outbound (user click -> pn_switch_toggle): builds a Tasmota command
 *   envelope (topic cmnd/<switch-name>/POWER, payload "ON"/"OFF",
 *   data.value 1.0/0.0) for a downstream MQTT Sink.  An unconfigured node
 *   falls back to the bare PnSwitch shape rather than publish to an empty
 *   cmnd//POWER topic.
 *
 *   Inbound (stat/<device>/POWER publishes): authoritatively sync the
 *   latch to the device's reported state WITHOUT emitting (re-emitting
 *   would loop straight back to the broker).  Gated by exact device name.
 *
 * Startup: unlike the base PnSwitch the announce-on-startup flag defaults
 * OFF -- opening a worksheet must never actuate a physical relay -- and is
 * opt-in via the enforce-on-startup property. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-switch.h"
#include "pn-switch.h"

/* Capture the last emitted message (ref'd: pn_switch_toggle unrefs the
 * built message right after emitting). */
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

/* Send a stat/POWER publish to a switch and report whether it emitted. */
static void
feed (PnNode *node, const gchar *topic, const gchar *payload)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    if (payload != NULL)
        pn_message_set_string (msg, "payload", payload);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* ---- defaults / startup ------------------------------------------- */

static void
test_default_off_and_announce_off (void)
{
    PnNode   *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);
    gboolean  enforce = TRUE;

    /* Latch starts off; the relay-actuating startup announce is OFF by
     * default so opening a worksheet never flips hardware. */
    PN_CHECK_FALSE (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_FALSE (pn_switch_get_announce_on_startup (PN_SWITCH (node)));

    g_object_get (node, "enforce-on-startup", &enforce, NULL);
    PN_CHECK_FALSE (enforce);

    g_object_unref (node);
}

static void
test_enforce_on_startup_round_trips (void)
{
    PnNode   *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);
    gboolean  enforce = FALSE;

    /* The property is the device-flavoured surface of the base switch's
     * announce flag -- setting one drives the other. */
    g_object_set (node, "enforce-on-startup", TRUE, NULL);
    g_object_get (node, "enforce-on-startup", &enforce, NULL);
    PN_CHECK (enforce);
    PN_CHECK (pn_switch_get_announce_on_startup (PN_SWITCH (node)));

    g_object_set (node, "enforce-on-startup", FALSE, NULL);
    PN_CHECK_FALSE (pn_switch_get_announce_on_startup (PN_SWITCH (node)));

    g_object_unref (node);
}

/* ---- outbound (user click) ---------------------------------------- */

static void
test_toggle_builds_command (void)
{
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    /* First click: off -> on, emits a Tasmota POWER ON command. */
    pn_switch_toggle (PN_SWITCH (node));
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPSTR (pn_message_get_topic (cap.last), ==, "cmnd/sonoff19/POWER");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "payload"), ==, "ON");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "device"), ==, "sonoff19");

    /* Second click: on -> off, POWER OFF. */
    pn_switch_toggle (PN_SWITCH (node));
    PN_CHECK_CMPINT (cap.count, ==, 2);
    PN_CHECK_FALSE  (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "payload"), ==, "OFF");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 0.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_unconfigured_toggle_base_shape (void)
{
    /* Without a switch-name the node still emits on click, but falls back
     * to the bare PnSwitch shape (no Tasmota topic / payload / device) so
     * it cannot publish to an empty cmnd//POWER. */
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);
    pn_switch_toggle (PN_SWITCH (node));

    PN_CHECK_CMPINT (cap.count, ==, 1);
    /* The base shape carries the node's default topic template, never a
     * Tasmota command path -- so a misconfigured node cannot publish to
     * an empty cmnd//POWER. */
    PN_CHECK_FALSE  (g_str_has_prefix (pn_message_get_topic (cap.last), "cmnd/"));
    PN_CHECK_FALSE  (pn_test_has (cap.last, "payload"));
    PN_CHECK_FALSE  (pn_test_has (cap.last, "device"));
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* ---- inbound (status sync, never emit) ---------------------------- */

static void
test_inbound_syncs_latch_silently (void)
{
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    /* An inbound POWER ON moves the latch but never emits (no loop back
     * to the broker). */
    feed (node, "stat/sonoff19/POWER", "ON");
    PN_CHECK (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    feed (node, "stat/sonoff19/POWER", "OFF");
    PN_CHECK_FALSE (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_inbound_rejects_other_device (void)
{
    /* Exact device match: "sonoff1" ignores "sonoff19"'s publish, latch
     * stays put. */
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_object_set (node, "switch-name", "sonoff1", NULL);
    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    feed (node, "stat/sonoff19/POWER", "ON");
    PN_CHECK_FALSE  (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_inbound_ignores_non_binary (void)
{
    /* "TOGGLE" and friends on the POWER topic leave the latch alone. */
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    feed (node, "stat/sonoff19/POWER", "TOGGLE");
    PN_CHECK_FALSE  (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_inbound_unconfigured_ignored (void)
{
    /* No switch-name: inbound dropped, latch stays off, nothing emitted. */
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    feed (node, "stat/sonoff19/POWER", "ON");
    PN_CHECK_FALSE  (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_inbound_rejects_non_power_topic (void)
{
    Capture  cap  = { 0, NULL };
    PnNode  *node = g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);

    g_object_set (node, "switch-name", "sonoff19", NULL);
    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    feed (node, "stat/sonoff19/RESULT", "ON");
    PN_CHECK_FALSE  (pn_switch_get_on (PN_SWITCH (node)));
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-switch");

    pn_test_add ("default_off_announce_off",  test_default_off_and_announce_off);
    pn_test_add ("enforce_round_trips",       test_enforce_on_startup_round_trips);
    pn_test_add ("toggle_builds_command",     test_toggle_builds_command);
    pn_test_add ("unconfigured_base_shape",   test_unconfigured_toggle_base_shape);
    pn_test_add ("inbound_syncs_silently",    test_inbound_syncs_latch_silently);
    pn_test_add ("inbound_rejects_other_dev", test_inbound_rejects_other_device);
    pn_test_add ("inbound_ignores_nonbinary", test_inbound_ignores_non_binary);
    pn_test_add ("inbound_unconfigured",      test_inbound_unconfigured_ignored);
    pn_test_add ("inbound_rejects_nonpower",  test_inbound_rejects_non_power_topic);

    return pn_test_run ();
}
