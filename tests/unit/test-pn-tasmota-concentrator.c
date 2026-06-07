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

/* Unit tests for PnTasmotaConcentrator: the collector half of the
 * Tasmota concentrator.  It accepts stat/<device>/STATUS# replies (and,
 * by default, tele/<device>/STATE|SENSOR telemetry), merges each
 * fragment into a per-device record, and emits ONE combined snapshot
 * (data.devices = { <device>: {merged}, ... }) when enough time has
 * elapsed since the last emit.  It ignores non-collectible topics, the
 * group topic, and payload-less messages, and rate-limits its emits by
 * the configurable interval. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-concentrator.h"

#include <json-glib/json-glib.h>

/* A message capture: refs the most recently emitted snapshot so a test
 * can assert on its data.devices object. */
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
make_node (gdouble interval, Capture *cap)
{
    PnNode *node = g_object_new (PN_TYPE_TASMOTA_CONCENTRATOR,
                                 "interval", interval, NULL);
    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

/* Feed one inbound message carrying @topic and a structured payload
 * { <section>: { "v": 1 } } -- the shape a Tasmota STATUS# reply (or a
 * tele payload) takes once PnMqtt has parsed it into data.payload. */
static void
feed (PnNode *node, const gchar *topic, const gchar *section)
{
    PnMessage  *msg     = pn_message_new (NULL, topic);
    JsonObject *payload = json_object_new ();
    JsonObject *inner   = json_object_new ();
    JsonNode   *pnode;

    json_object_set_int_member (inner, "v", 1);
    json_object_set_object_member (payload, section, inner); /* takes inner */

    pnode = json_node_alloc ();
    json_node_init_object (pnode, payload);                  /* refs payload */
    json_object_unref (payload);
    pn_message_set_member (msg, "payload", pnode);

    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Feed a message with a topic but no data.payload member. */
static void
feed_no_payload (PnNode *node, const gchar *topic)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* The data.devices object of @message, or %NULL when absent. */
static JsonObject *
devices_of (PnMessage *message)
{
    JsonNode *n = pn_message_get_member (message, "devices");
    return (n != NULL && JSON_NODE_HOLDS_OBJECT (n))
           ? json_node_get_object (n) : NULL;
}

/* The merged record object for @device inside @message's snapshot. */
static JsonObject *
device_record (PnMessage *message, const gchar *device)
{
    JsonObject *devices = devices_of (message);
    JsonNode   *n;

    if (devices == NULL || !json_object_has_member (devices, device))
        return NULL;
    n = json_object_get_member (devices, device);
    return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

/* A STATUS reply is collected into a one-device snapshot carrying the
 * standard value / success / devices contract. */
static void
test_status_reply_collected (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);
    JsonObject *rec;

    feed (node, "stat/sonoff19/STATUS", "Status");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_CMPSTR (pn_message_get_topic (cap.last), ==, "tasmota/devices");

    rec = device_record (cap.last, "sonoff19");
    PN_CHECK (rec != NULL);
    PN_CHECK (rec != NULL && json_object_has_member (rec, "Status"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* Successive sections of one device merge into a single record: a
 * STATUS5 reply and a STATUS11 reply both end up in sonoff19's object. */
static void
test_sections_merge (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);
    JsonObject *rec;

    feed (node, "stat/sonoff19/STATUS5",  "StatusNET");
    feed (node, "stat/sonoff19/STATUS11", "StatusSTS");

    /* interval 0 -> the last emit reflects every fragment seen so far. */
    PN_CHECK_CMPINT (cap.count, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 1.0, 1e-9);

    rec = device_record (cap.last, "sonoff19");
    PN_CHECK (rec != NULL);
    PN_CHECK (rec != NULL && json_object_has_member (rec, "StatusNET"));
    PN_CHECK (rec != NULL && json_object_has_member (rec, "StatusSTS"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* Distinct devices accumulate side by side: the snapshot keys both and
 * value counts them. */
static void
test_multiple_devices (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    feed (node, "stat/sonoff19/STATUS", "Status");
    feed (node, "stat/sonoff47/STATUS", "Status");

    PN_CHECK_CMPINT (cap.count, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 2.0, 1e-9);
    PN_CHECK (device_record (cap.last, "sonoff19") != NULL);
    PN_CHECK (device_record (cap.last, "sonoff47") != NULL);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* A long interval debounces: the first accepted message emits (never
 * emitted before), the rest are suppressed inside the window. */
static void
test_debounce_suppresses (void)
{
    Capture cap;
    PnNode *node = make_node (3600.0, &cap);

    feed (node, "stat/sonoff19/STATUS", "Status");  /* first -> emit */
    feed (node, "stat/sonoff47/STATUS", "Status");  /* within window -> drop */
    feed (node, "stat/plug03/STATUS",   "Status");  /* within window -> drop */

    PN_CHECK_CMPINT (cap.count, ==, 1);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* interval 0 disables the debounce: every accepted message emits. */
static void
test_zero_interval_emits_each (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    feed (node, "stat/sonoff19/STATUS", "Status");
    feed (node, "stat/sonoff47/STATUS", "Status");
    feed (node, "stat/plug03/STATUS",   "Status");

    PN_CHECK_CMPINT (cap.count, ==, 3);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* With merge-telemetry on (the default), tele/<device>/SENSOR and
 * tele/<device>/STATE are folded into the record too. */
static void
test_telemetry_merged (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);
    JsonObject *rec;

    feed (node, "tele/sonoff19/SENSOR", "AM2301");
    feed (node, "tele/sonoff19/STATE",  "Wifi");

    PN_CHECK_CMPINT (cap.count, ==, 2);
    rec = device_record (cap.last, "sonoff19");
    PN_CHECK (rec != NULL);
    PN_CHECK (rec != NULL && json_object_has_member (rec, "AM2301"));
    PN_CHECK (rec != NULL && json_object_has_member (rec, "Wifi"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* With merge-telemetry off, tele/ traffic is ignored; only STATUS#
 * replies are collected. */
static void
test_telemetry_off_ignored (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    g_object_set (node, "merge-telemetry", FALSE, NULL);

    feed (node, "tele/sonoff19/SENSOR", "AM2301");
    feed (node, "tele/sonoff19/STATE",  "Wifi");

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* Topics that are not collectible -- a stat/ topic that is not a STATUS
 * reply, a tele/ topic that is neither STATE nor SENSOR, and a
 * non-Tasmota prefix -- are ignored. */
static void
test_non_collectible_ignored (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    feed (node, "stat/sonoff19/POWER",     "x");  /* not STATUS#      */
    feed (node, "tele/sonoff19/RESULT",    "x");  /* not STATE/SENSOR */
    feed (node, "homeassistant/x/config",  "x");  /* wrong prefix     */

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* The group topic "tasmotas" never appears as a device: a group Status
 * is answer-suppressed and is not a real endpoint. */
static void
test_group_topic_ignored (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    feed (node, "stat/tasmotas/STATUS", "Status");

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* A collectible topic with no structured payload has nothing to merge
 * and is dropped without an emit. */
static void
test_no_payload_ignored (void)
{
    Capture cap;
    PnNode *node = make_node (0.0, &cap);

    feed_no_payload (node, "stat/sonoff19/STATUS");

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-concentrator");
    pn_test_add ("status_reply_collected", test_status_reply_collected);
    pn_test_add ("sections_merge",         test_sections_merge);
    pn_test_add ("multiple_devices",       test_multiple_devices);
    pn_test_add ("debounce_suppresses",    test_debounce_suppresses);
    pn_test_add ("zero_interval_each",     test_zero_interval_emits_each);
    pn_test_add ("telemetry_merged",       test_telemetry_merged);
    pn_test_add ("telemetry_off_ignored",  test_telemetry_off_ignored);
    pn_test_add ("non_collectible",        test_non_collectible_ignored);
    pn_test_add ("group_topic_ignored",    test_group_topic_ignored);
    pn_test_add ("no_payload_ignored",     test_no_payload_ignored);
    return pn_test_run ();
}
