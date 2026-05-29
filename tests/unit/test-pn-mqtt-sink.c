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
#include "pn-message.h"
#include "pn-node.h"

static PnMqttSink *
make_sink (void)
{
    return PN_MQTT_SINK (g_object_new (PN_TYPE_MQTT_SINK, NULL));
}

/* ------------------------------------------------------------------ */
/*  A minimal subclass exercising the derivable surface               */
/*                                                                     */
/*  Proves a plugin can now subclass PnMqttSink (it used to be a final */
/*  type) and override the two class vfuncs.  The override observations */
/*  are captured in single-threaded globals — these tests never start a */
/*  broker session or a main loop, so the property setters that drive   */
/*  the base run synchronously on this thread.                          */
/* ------------------------------------------------------------------ */

static gint     g_process_calls;
static gboolean g_drop_next;
static gchar   *g_last_error;

typedef struct { PnMqttSink     parent; } TestSink;
typedef struct { PnMqttSinkClass parent; } TestSinkClass;

static GType test_sink_get_type (void);
G_DEFINE_TYPE (TestSink, test_sink, PN_TYPE_MQTT_SINK)

static gboolean
test_sink_process_message (PnMqttSink *self, PnMessage *message)
{
    (void) self;
    (void) message;
    g_process_calls++;
    return !g_drop_next;
}

static void
test_sink_report_error (PnMqttSink *self, const gchar *message)
{
    (void) self;
    g_free (g_last_error);
    g_last_error = g_strdup (message);
}

static void
test_sink_init (TestSink *self)
{
    (void) self;
}

static void
test_sink_class_init (TestSinkClass *klass)
{
    PnMqttSinkClass *sink_class = PN_MQTT_SINK_CLASS (klass);

    sink_class->process_message = test_sink_process_message;
    sink_class->report_error    = test_sink_report_error;
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

/* The node dialog lays one editor row out per editable property in
 * g_object_class_list_properties() order (see build_property_grid() in
 * pn-node-dialog.c), filtering exactly as below.  Pin that order so the
 * "publish topic/payload/retain + QoS sit last, after the broker/
 * credential fields" layout can't silently regress: the profile-ref
 * inline fields (url/username/password/client-id) stay grouped right
 * after the picker, then the non-profile publish settings come last. */
static void
test_property_order (void)
{
    static const gchar *expected[] = {
        "broker-profile",
        "url",
        "username",
        "password",
        "client-id",
        "topic-template",
        "payload",
        "retain",
        "qos",
    };
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT_SINK);
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
        if (p->owner_type != PN_TYPE_MQTT_SINK)
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
    GObjectClass *object_class = g_type_class_ref (PN_TYPE_MQTT_SINK);
    GParamSpec   *pspec;
    const gchar  *ref;

    pspec = g_object_class_find_property (object_class, "broker-profile");
    PN_CHECK (pspec != NULL);

    ref = pn_param_spec_get_profile_ref (pspec);
    PN_CHECK_CMPSTR (ref, ==, "mqtt-broker");

    g_type_class_unref (object_class);
}

/* The type is now derivable (was G_DECLARE_FINAL_TYPE) so plugins can
 * implement their own version, and the base wires its two extension
 * vfuncs to the exported defaults. */
static void
test_derivable (void)
{
    GType            st = test_sink_get_type ();
    PnMqttSinkClass *base_class;
    TestSink        *sub;

    PN_CHECK (g_type_is_a (st, PN_TYPE_MQTT_SINK));

    base_class = g_type_class_ref (PN_TYPE_MQTT_SINK);
    PN_CHECK (base_class->process_message == pn_mqtt_sink_real_process_message);
    PN_CHECK (base_class->report_error    == pn_mqtt_sink_real_report_error);
    g_type_class_unref (base_class);

    sub = g_object_new (st, NULL);
    PN_CHECK (sub != NULL);
    PN_CHECK (PN_IS_MQTT_SINK (sub));
    g_object_unref (sub);
}

/* The default report_error records the message in the per-node Log ring
 * at error level — the channel an inherited class inherits for free. */
static void
test_default_report_error_logs (void)
{
    PnMqttSink *self = make_sink ();
    GPtrArray  *log;
    PnLogEntry *entry;

    pn_node_clear_log (PN_NODE (self));
    pn_mqtt_sink_real_report_error (self, "boom 42");

    log = pn_node_get_log (PN_NODE (self));
    PN_CHECK (log != NULL && log->len >= 1);
    if (log != NULL && log->len >= 1)
    {
        entry = g_ptr_array_index (log, log->len - 1);
        PN_CHECK_CMPINT (pn_log_entry_get_level (entry), ==, PN_LOG_LEVEL_ERROR);
        PN_CHECK_CMPSTR (pn_log_entry_get_message (entry), ==, "boom 42");
    }

    g_object_unref (self);
}

/* Every error the base senses is funnelled through the report_error
 * vfunc, so a subclass override sees it.  An invalid broker URL on a
 * "Custom settings" node (inline url, no profile) drives the
 * invalid-URL path synchronously from the property setter. */
static void
test_error_funnel_reaches_subclass (void)
{
    TestSink *self = g_object_new (test_sink_get_type (), NULL);

    g_free (g_last_error);
    g_last_error = NULL;

    /* Force the inline url to be the effective connection (no profile),
     * then hand it a url with an empty host part — pn_mqtt_parse_url
     * rejects "tcp://", which the base reports as an error. */
    g_object_set (self, "broker-profile", PN_PROFILE_REF_CUSTOM, NULL);
    g_object_set (self, "url", "tcp://", NULL);

    PN_CHECK (g_last_error != NULL);
    if (g_last_error != NULL)
        PN_CHECK (g_strstr_len (g_last_error, -1, "invalid broker URL") != NULL);

    g_free (g_last_error);
    g_last_error = NULL;
    g_object_unref (self);
}

/* The receive path invokes process_message before resolving the publish,
 * whether or not the hook drops the message. */
static void
test_process_message_invoked (void)
{
    TestSink  *self = g_object_new (test_sink_get_type (), NULL);
    PnNode    *node = PN_NODE (self);
    PnMessage *msg  = pn_message_new (NULL, "x");

    g_process_calls = 0;

    g_drop_next = FALSE;
    PN_NODE_GET_CLASS (node)->receive (node, msg);
    PN_CHECK_CMPINT (g_process_calls, ==, 1);

    g_drop_next = TRUE;
    PN_NODE_GET_CLASS (node)->receive (node, msg);
    PN_CHECK_CMPINT (g_process_calls, ==, 2);
    g_drop_next = FALSE;

    g_object_unref (msg);
    g_object_unref (self);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mqtt-sink");
    pn_test_add ("type_registered",    test_type_registered);
    pn_test_add ("class_metadata",     test_class_metadata);
    pn_test_add ("property_defaults",  test_property_defaults);
    pn_test_add ("property_roundtrip", test_property_roundtrip);
    pn_test_add ("property_order",     test_property_order);
    pn_test_add ("broker_profile_ref", test_broker_profile_ref_tag);
    pn_test_add ("derivable",          test_derivable);
    pn_test_add ("default_report_error", test_default_report_error_logs);
    pn_test_add ("error_funnel_subclass", test_error_funnel_reaches_subclass);
    pn_test_add ("process_message_invoked", test_process_message_invoked);
    return pn_test_run ();
}
