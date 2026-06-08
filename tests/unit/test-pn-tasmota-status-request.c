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

/* Unit tests for PnTasmotaStatusRequest, the manual single-shot Status
 * poller.  Every inbound message is a bare trigger -- the data bag value
 * is irrelevant -- and is reshaped in place into a Tasmota query envelope
 * (topic cmnd/<target>/Status, payload "<n>") and forwarded for a
 * downstream MQTT Sink to publish.  Unlike the relay nodes this one has no
 * unconfigured/error state: a Status query is read-only, so an empty
 * device falls back to the "tasmotas" group topic (every device) which is
 * also the default.  The Request enum value is the wire argument. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tasmota-status-request.h"
#include "pn-tasmota-common.h"

/* Fire one trigger at a node and report the emit count.  The node
 * rewrites @msg in place, so the caller inspects @msg afterwards. */
static guint
fire (PnNode *node, PnMessage *msg)
{
    guint emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    pn_node_receive_message (node, msg);
    return emits;
}

static void
test_default_broadcasts (void)
{
    /* Fresh node: device defaults to the "tasmotas" group topic and the
     * request to Status 0 (everything). */
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    PnMessage *msg  = pn_message_new (NULL, "trigger");

    PN_CHECK_CMPINT (fire (node, msg), ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/tasmotas/Status");
    PN_CHECK_CMPSTR (pn_test_str (msg, "payload"), ==, "0");
    PN_CHECK        (pn_test_bool (msg, "success"));
    PN_CHECK_CMPSTR (pn_test_str (msg, "device"), ==, "tasmotas");
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "tasmotas: request Status 0");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_targets_device (void)
{
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    PnMessage *msg  = pn_message_new (NULL, "trigger");

    g_object_set (node, "device", "sonoff19", NULL);

    PN_CHECK_CMPINT (fire (node, msg), ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/sonoff19/Status");
    PN_CHECK_CMPSTR (pn_test_str (msg, "device"), ==, "sonoff19");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_empty_device_broadcasts (void)
{
    /* An empty device field resolves to the group topic at send time. */
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    PnMessage *msg  = pn_message_new (NULL, "trigger");

    g_object_set (node, "device", "", NULL);

    PN_CHECK_CMPINT (fire (node, msg), ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/tasmotas/Status");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_request_kind_payload (void)
{
    /* The Status section enum value is stringified onto data.payload. */
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    PnMessage *msg  = pn_message_new (NULL, "trigger");

    g_object_set (node, "request", PN_TASMOTA_STATUS_STATE, NULL); /* 11 */

    PN_CHECK_CMPINT (fire (node, msg), ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "payload"), ==, "11");
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==,
                     "tasmotas: request Status 11");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_value_irrelevant (void)
{
    /* The trigger's own data bag is ignored: a message with no value (or
     * any value) still produces the query. */
    PnNode    *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    PnMessage *msg  = pn_message_new (NULL, "trigger");

    pn_message_set_double (msg, "value", 0.0);

    PN_CHECK_CMPINT (fire (node, msg), ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "cmnd/tasmotas/Status");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_props_round_trip (void)
{
    PnNode              *node = g_object_new (PN_TYPE_TASMOTA_STATUS_REQUEST, NULL);
    gchar               *device = NULL;
    PnTasmotaStatusKind  req = PN_TASMOTA_STATUS_ALL;

    /* Defaults. */
    g_object_get (node, "device", &device, "request", &req, NULL);
    PN_CHECK_CMPSTR (device, ==, "tasmotas");
    PN_CHECK_CMPINT (req, ==, PN_TASMOTA_STATUS_ALL);
    g_free (device);

    g_object_set (node, "device", "tasmota13",
                        "request", PN_TASMOTA_STATUS_NETWORK, NULL);
    g_object_get (node, "device", &device, "request", &req, NULL);
    PN_CHECK_CMPSTR (device, ==, "tasmota13");
    PN_CHECK_CMPINT (req, ==, PN_TASMOTA_STATUS_NETWORK);
    g_free (device);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tasmota-status-request");

    pn_test_add ("default_broadcasts",      test_default_broadcasts);
    pn_test_add ("targets_device",          test_targets_device);
    pn_test_add ("empty_device_broadcasts", test_empty_device_broadcasts);
    pn_test_add ("request_kind_payload",    test_request_kind_payload);
    pn_test_add ("value_irrelevant",        test_value_irrelevant);
    pn_test_add ("props_round_trip",        test_props_round_trip);

    return pn_test_run ();
}
