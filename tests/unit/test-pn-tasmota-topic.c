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

/* Unit tests for the Tasmota topic-family gate.  The Devices dialog and
 * the discovery-side nodes share one broad MQTT subscription with every
 * other app on the broker (zigbee2mqtt, Shelly, Node-RED).  Only topics
 * under a Tasmota root (tele/, stat/, cmnd/) name a real device; without
 * the gate a foreign topic like "zigbee2mqtt/bridge/state" parses to the
 * device id "bridge" and is mistaken for an (unanswerable) Tasmota unit.
 *
 *   - pn_tasmota_topic_is_family() is the shared predicate.
 *   - PnTasmotaStatus re-emits ONLY genuine stat/<device>/STATUS# replies,
 *     so a foreign "<root>/<x>/STATUS" topic is not taken for a reply. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-common.h"
#include "pn-tasmota-status.h"

/* --- the shared predicate ----------------------------------------- */

static void
test_family_accepts_tasmota_roots (void)
{
    PN_CHECK (pn_tasmota_topic_is_family ("tele/sonoff19/STATE"));
    PN_CHECK (pn_tasmota_topic_is_family ("stat/sonoff19/STATUS11"));
    PN_CHECK (pn_tasmota_topic_is_family ("cmnd/sonoff19/Status"));
    PN_CHECK (pn_tasmota_topic_is_family ("tele/sonoff_rf_bridge_01/LWT"));
}

static void
test_family_rejects_foreign (void)
{
    /* The zigbee2mqtt bridge's own control topics -- the real-world leak. */
    PN_CHECK_FALSE (pn_tasmota_topic_is_family ("zigbee2mqtt/bridge/state"));
    PN_CHECK_FALSE (pn_tasmota_topic_is_family ("zigbee2mqtt/bridge/health"));
    PN_CHECK_FALSE (pn_tasmota_topic_is_family ("shellies/shelly1/relay/0"));
    /* A substring match must not pass: the root has to be a real segment. */
    PN_CHECK_FALSE (pn_tasmota_topic_is_family ("home/tele/x"));
    PN_CHECK_FALSE (pn_tasmota_topic_is_family ("telemetry/x/y"));
}

static void
test_family_edge_cases (void)
{
    PN_CHECK_FALSE (pn_tasmota_topic_is_family (NULL));
    PN_CHECK_FALSE (pn_tasmota_topic_is_family (""));
}

/* --- the Status node gate ----------------------------------------- */

typedef struct { guint count; } Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;
    (void) node; (void) message;
    cap->count++;
}

static guint
emits_for (const gchar *topic)
{
    Capture    cap  = { 0 };
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS, NULL);
    PnMessage *msg  = pn_message_new (NULL, topic);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);
    pn_node_receive_message (node, msg);

    g_object_unref (msg);
    g_object_unref (node);
    return cap.count;
}

static void
test_status_passes_real_reply (void)
{
    PN_CHECK_CMPINT (emits_for ("stat/sonoff19/STATUS"),   ==, 1);
    PN_CHECK_CMPINT (emits_for ("stat/sonoff19/STATUS11"), ==, 1);
}

static void
test_status_rejects_foreign (void)
{
    /* Right family, wrong section -- always ignored. */
    PN_CHECK_CMPINT (emits_for ("stat/sonoff19/RESULT"),    ==, 0);
    /* Wrong root, even with a STATUS-looking last segment: the loose gate
     * used to let this through. */
    PN_CHECK_CMPINT (emits_for ("zigbee2mqtt/bridge/STATUS"), ==, 0);
    PN_CHECK_CMPINT (emits_for ("zigbee2mqtt/bridge/state"),  ==, 0);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-topic");

    pn_test_add ("family_accepts_tasmota_roots", test_family_accepts_tasmota_roots);
    pn_test_add ("family_rejects_foreign",       test_family_rejects_foreign);
    pn_test_add ("family_edge_cases",            test_family_edge_cases);
    pn_test_add ("status_passes_real_reply",     test_status_passes_real_reply);
    pn_test_add ("status_rejects_foreign",       test_status_rejects_foreign);

    return pn_test_run ();
}
