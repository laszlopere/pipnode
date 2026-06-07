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

/* Unit tests for PnTasmotaProbe: the auto-discovering poller.  It learns
 * device names from inbound tele/stat/cmnd topics, emits a
 * cmnd/<device>/Status <n> command per discovered device, probes a
 * newly-seen device promptly, and rate-limits re-probes of a known
 * device by a configurable interval.  It emits only probes, never
 * forwards telemetry, and ignores non-Tasmota and group-topic traffic. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-probe.h"

/* Build a probe node with the given re-probe interval (seconds), wiring
 * an emit counter to its "message" signal. */
static PnNode *
make_node (gdouble interval, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_TASMOTA_PROBE,
                                 "interval", interval, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Feed one inbound message carrying @topic to @node and drop our ref;
 * the node reshapes a clone-equivalent in place, so the caller inspects
 * via the emit count and the captured-on-emit copy elsewhere. */
static void
feed (PnNode *node, const gchar *topic)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* A message capture: refs the most recently emitted message so a test
 * can assert on the reshaped probe command. */
typedef struct { guint count; PnMessage *last; } Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;
    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

static PnNode *
make_capture_node (gdouble interval, Capture *cap)
{
    PnNode *node = g_object_new (PN_TYPE_TASMOTA_PROBE,
                                 "interval", interval, NULL);
    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

/* A newly-seen device, named via a tele/ topic, is probed promptly and
 * the emitted command has the Tasmota cmnd/<device>/Status shape. */
static void
test_new_device_probed (void)
{
    Capture cap;
    PnNode *node = make_capture_node (300.0, &cap);

    feed (node, "tele/sonoff19/SENSOR");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (cap.last), ==,
                     "cmnd/sonoff19/Status");
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "payload"), ==, "0");
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "device"),  ==, "sonoff19");
    PN_CHECK        (pn_test_bool (cap.last, "success"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* The request property selects the Status section; the payload is the
 * stringified section number. */
static void
test_request_section_sets_payload (void)
{
    Capture cap;
    PnNode *node = make_capture_node (300.0, &cap);

    /* PN_TASMOTA_STATUS_NETWORK == 5 */
    g_object_set (node, "request", PN_TASMOTA_STATUS_NETWORK, NULL);
    feed (node, "tele/sonoff19/STATE");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (cap.last), ==,
                     "cmnd/sonoff19/Status");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "payload"), ==, "5");

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* A known device is not re-probed while inside its interval: with a long
 * interval the second message from the same device is dropped. */
static void
test_known_device_rate_limited (void)
{
    guint   emits;
    PnNode *node = make_node (3600.0, &emits);

    feed (node, "tele/sonoff19/SENSOR");   /* first sight -> probe */
    feed (node, "tele/sonoff19/SENSOR");   /* within interval -> dropped */
    feed (node, "stat/sonoff19/RESULT");   /* still within interval -> dropped */

    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

/* interval == 0 disables rate limiting: every message naming the device
 * re-probes it. */
static void
test_zero_interval_always_probes (void)
{
    guint   emits;
    PnNode *node = make_node (0.0, &emits);

    feed (node, "tele/sonoff19/SENSOR");
    feed (node, "tele/sonoff19/SENSOR");
    feed (node, "tele/sonoff19/SENSOR");

    PN_CHECK_CMPINT (emits, ==, 3);

    g_object_unref (node);
}

/* Distinct devices are each probed promptly even under a long interval:
 * the rate limit is per-device, not global. */
static void
test_distinct_devices_each_probed (void)
{
    guint   emits;
    PnNode *node = make_node (3600.0, &emits);

    feed (node, "tele/sonoff19/SENSOR");
    feed (node, "tele/sonoff47/SENSOR");
    feed (node, "stat/plug03/STATUS5");

    PN_CHECK_CMPINT (emits, ==, 3);

    g_object_unref (node);
}

/* All three Tasmota topic families (tele/stat/cmnd) carry a discoverable
 * device. */
static void
test_all_families_discover (void)
{
    guint   emits;
    PnNode *node = make_node (3600.0, &emits);

    feed (node, "tele/dev_a/SENSOR");
    feed (node, "stat/dev_b/STATUS");
    feed (node, "cmnd/dev_c/POWER");

    PN_CHECK_CMPINT (emits, ==, 3);

    g_object_unref (node);
}

/* Traffic that is not a Tasmota device topic is ignored: a non-Tasmota
 * prefix, and a topic with too few segments to carry a device. */
static void
test_non_tasmota_ignored (void)
{
    guint   emits;
    PnNode *node = make_node (300.0, &emits);

    feed (node, "homeassistant/sensor/x/config"); /* wrong prefix */
    feed (node, "tele/onlytwo");                   /* < 3 segments */
    feed (node, "");                               /* empty topic */

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* The group topic "tasmotas" is never probed: a group Status yields no
 * reply, so polling it is pointless and it is not a real device. */
static void
test_group_topic_ignored (void)
{
    guint   emits;
    PnNode *node = make_node (0.0, &emits);

    feed (node, "cmnd/tasmotas/Status");
    feed (node, "stat/tasmotas/RESULT");

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-probe");
    pn_test_add ("new_device_probed",     test_new_device_probed);
    pn_test_add ("request_section",       test_request_section_sets_payload);
    pn_test_add ("known_rate_limited",    test_known_device_rate_limited);
    pn_test_add ("zero_interval_always",  test_zero_interval_always_probes);
    pn_test_add ("distinct_devices",      test_distinct_devices_each_probed);
    pn_test_add ("all_families_discover", test_all_families_discover);
    pn_test_add ("non_tasmota_ignored",   test_non_tasmota_ignored);
    pn_test_add ("group_topic_ignored",   test_group_topic_ignored);
    return pn_test_run ();
}
