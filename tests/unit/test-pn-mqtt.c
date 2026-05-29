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

/* Characterization tests for PnMqtt's GObject surface.  Pins the type's
 * registration, parent class, palette metadata, property defaults, and the
 * mqtt-broker profile-ref tag — the public contract a subclass or a saved
 * workflow depends on.  No broker is contacted; the node is constructed and
 * inspected in process.  These tests are deliberately byte-identical
 * before and after PnMqtt moves from plugins/network/ back into core, so
 * a botched move (missed type registration, dropped property, renamed
 * topic field, lost profile-ref) trips immediately. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt.h"
#include "pn-node.h"

static PnMqtt *
make_mqtt (void)
{
    return PN_MQTT (g_object_new (PN_TYPE_MQTT, NULL));
}

static void
test_type_registered (void)
{
    GType t = PN_TYPE_MQTT;

    PN_CHECK (t != G_TYPE_INVALID);
    PN_CHECK_CMPSTR (g_type_name (t), ==, "PnMqtt");
    PN_CHECK (g_type_is_a (t, PN_TYPE_NODE));
}

static void
test_class_metadata (void)
{
    PnMqttClass *klass     = g_type_class_ref (PN_TYPE_MQTT);
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    PN_CHECK_CMPSTR (node_class->class_name, ==, "MQTT Source");
    PN_CHECK_CMPSTR (node_class->category,   ==, "Network");
    PN_CHECK (node_class->has_input  == FALSE);
    PN_CHECK (node_class->has_output == TRUE);

    /* Derivable-type vfuncs default to the exported "real" implementations
     * so a subclass can chain to them.  Pinning these here is what makes
     * the subclassing contract visible at test time. */
    PN_CHECK (klass->accept_topic    == pn_mqtt_real_accept_topic);
    PN_CHECK (klass->process_message == pn_mqtt_real_process_message);

    g_type_class_unref (klass);
}

static void
test_property_defaults (void)
{
    PnMqtt *self = make_mqtt ();
    gchar  *broker_profile = NULL;
    gchar  *url            = NULL;
    gchar  *topic          = NULL;
    gchar  *username       = NULL;
    gchar  *password       = NULL;
    gchar  *client_id      = NULL;
    guint   qos            = 99;

    g_object_get (self,
                  "broker-profile",  &broker_profile,
                  "url",             &url,
                  "subscribe-topic", &topic,
                  "username",        &username,
                  "password",        &password,
                  "client-id",       &client_id,
                  "qos",             &qos,
                  NULL);

    PN_CHECK_CMPSTR (broker_profile, ==, "");
    PN_CHECK_CMPSTR (url,            ==, "");
    PN_CHECK_CMPSTR (topic,          ==, "#");
    PN_CHECK (username  == NULL);
    PN_CHECK (password  == NULL);
    PN_CHECK (client_id == NULL);
    PN_CHECK_CMPINT (qos,            ==, 0u);

    g_free (broker_profile);
    g_free (url);
    g_free (topic);
    g_free (username);
    g_free (password);
    g_free (client_id);
    g_object_unref (self);
}

static void
test_property_roundtrip (void)
{
    PnMqtt *self = make_mqtt ();
    gchar  *url   = NULL;
    gchar  *topic = NULL;
    guint   qos   = 0;

    g_object_set (self,
                  "url",             "tcp://broker.example:1883",
                  "subscribe-topic", "zigbee2mqtt/#",
                  "qos",             1u,
                  NULL);

    g_object_get (self,
                  "url",             &url,
                  "subscribe-topic", &topic,
                  "qos",             &qos,
                  NULL);

    PN_CHECK_CMPSTR (url,   ==, "tcp://broker.example:1883");
    PN_CHECK_CMPSTR (topic, ==, "zigbee2mqtt/#");
    PN_CHECK_CMPINT (qos,   ==, 1u);

    g_free (url);
    g_free (topic);
    g_object_unref (self);
}

/* The node dialog lays one editor row out per editable property in
 * g_object_class_list_properties() order (see build_property_grid() in
 * pn-node-dialog.c), filtering exactly as below.  Pin that order so the
 * "Subscribe topic / Subscribe QoS sit last, after the broker/credential
 * fields" layout can't silently regress: the profile-ref inline fields
 * (url/username/password/client-id) stay grouped right after the picker,
 * then the non-profile subscribe settings come last and adjacent. */
static void
test_property_order (void)
{
    static const gchar *expected[] = {
        "broker-profile",
        "url",
        "username",
        "password",
        "client-id",
        "subscribe-topic",
        "qos",
    };
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT);
    guint         n_props      = 0;
    GParamSpec  **pspecs       = g_object_class_list_properties (object_class,
                                                                &n_props);
    guint         row          = 0;
    guint         i;

    for (i = 0; i < n_props; i++)
    {
        GParamSpec *p = pspecs[i];

        /* Same filter the dialog applies: own properties only, writable,
         * not construct-only. */
        if (p->owner_type != PN_TYPE_MQTT)
            continue;
        if ((p->flags & G_PARAM_WRITABLE) == 0)
            continue;
        if ((p->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
            continue;

        if (row < G_N_ELEMENTS (expected))
            PN_CHECK_CMPSTR (g_param_spec_get_name (p), ==, expected[row]);
        row++;
    }

    PN_CHECK_CMPINT (row, ==, G_N_ELEMENTS (expected));

    g_free (pspecs);
    g_type_class_unref (object_class);
}

static void
test_broker_profile_ref_tag (void)
{
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT);
    GParamSpec   *pspec;
    const gchar  *ref;

    pspec = g_object_class_find_property (object_class, "broker-profile");
    PN_CHECK (pspec != NULL);

    ref = pn_param_spec_get_profile_ref (pspec);
    PN_CHECK_CMPSTR (ref, ==, "mqtt-broker");

    g_type_class_unref (object_class);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mqtt");
    pn_test_add ("type_registered",       test_type_registered);
    pn_test_add ("class_metadata",        test_class_metadata);
    pn_test_add ("property_defaults",     test_property_defaults);
    pn_test_add ("property_roundtrip",    test_property_roundtrip);
    pn_test_add ("property_order",        test_property_order);
    pn_test_add ("broker_profile_ref",    test_broker_profile_ref_tag);
    return pn_test_run ();
}
