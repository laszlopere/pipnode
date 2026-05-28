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

/* Characterization tests for PnMqttSink's GObject surface.  Pin the type's
 * registration, palette metadata, property defaults, and the mqtt-broker
 * profile-ref tag — the public contract a saved workflow depends on.  No
 * broker is contacted.  Companion to test-pn-mqtt; identical pre/post the
 * core relocation. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt-sink.h"
#include "pn-node.h"

static PnMqttSink *
make_sink (void)
{
    return PN_MQTT_SINK (g_object_new (PN_TYPE_MQTT_SINK, NULL));
}

static void
test_type_registered (void)
{
    GType t = PN_TYPE_MQTT_SINK;

    PN_CHECK (t != G_TYPE_INVALID);
    PN_CHECK_CMPSTR (g_type_name (t), ==, "PnMqttSink");
    PN_CHECK (g_type_is_a (t, PN_TYPE_NODE));
}

static void
test_class_metadata (void)
{
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT_SINK);
    PnNodeClass  *node_class   = PN_NODE_CLASS (object_class);

    PN_CHECK_CMPSTR (node_class->class_name, ==, "MQTT Sink");
    PN_CHECK_CMPSTR (node_class->category,   ==, "Network");
    PN_CHECK (node_class->has_input  == TRUE);
    PN_CHECK (node_class->has_output == FALSE);

    g_type_class_unref (object_class);
}

static void
test_property_defaults (void)
{
    PnMqttSink *self = make_sink ();
    gchar      *broker_profile = NULL;
    gchar      *url            = NULL;
    gchar      *topic          = NULL;
    gchar      *payload        = NULL;
    gboolean    retain         = TRUE;
    gchar      *username       = NULL;
    gchar      *password       = NULL;
    gchar      *client_id      = NULL;
    guint       qos            = 99;

    g_object_get (self,
                  "broker-profile", &broker_profile,
                  "url",            &url,
                  "topic-template", &topic,
                  "payload",        &payload,
                  "retain",         &retain,
                  "username",       &username,
                  "password",       &password,
                  "client-id",      &client_id,
                  "qos",            &qos,
                  NULL);

    PN_CHECK_CMPSTR (broker_profile, ==, "");
    PN_CHECK_CMPSTR (url,            ==, "");
    PN_CHECK (topic     == NULL);
    PN_CHECK (payload   == NULL);
    PN_CHECK (retain == FALSE);
    PN_CHECK (username  == NULL);
    PN_CHECK (password  == NULL);
    PN_CHECK (client_id == NULL);
    PN_CHECK_CMPINT (qos,            ==, 0u);

    g_free (broker_profile);
    g_free (url);
    g_free (topic);
    g_free (payload);
    g_free (username);
    g_free (password);
    g_free (client_id);
    g_object_unref (self);
}

static void
test_property_roundtrip (void)
{
    PnMqttSink *self  = make_sink ();
    gchar      *topic = NULL;
    gchar      *payload = NULL;
    gboolean    retain = FALSE;
    guint       qos    = 0;

    g_object_set (self,
                  "topic-template", "cmnd/${data/device}/POWER",
                  "payload",        "${data/value}",
                  "retain",         TRUE,
                  "qos",            2u,
                  NULL);

    g_object_get (self,
                  "topic-template", &topic,
                  "payload",        &payload,
                  "retain",         &retain,
                  "qos",            &qos,
                  NULL);

    PN_CHECK_CMPSTR (topic,   ==, "cmnd/${data/device}/POWER");
    PN_CHECK_CMPSTR (payload, ==, "${data/value}");
    PN_CHECK (retain == TRUE);
    PN_CHECK_CMPINT (qos,     ==, 2u);

    g_free (topic);
    g_free (payload);
    g_object_unref (self);
}

static void
test_broker_profile_ref_tag (void)
{
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT_SINK);
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
    pn_test_init (&argc, &argv, "pn-mqtt-sink");
    pn_test_add ("type_registered",    test_type_registered);
    pn_test_add ("class_metadata",     test_class_metadata);
    pn_test_add ("property_defaults",  test_property_defaults);
    pn_test_add ("property_roundtrip", test_property_roundtrip);
    pn_test_add ("broker_profile_ref", test_broker_profile_ref_tag);
    return pn_test_run ();
}
