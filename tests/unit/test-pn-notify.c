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

/* Unit tests for PnNotify.  Receiving is an unconditional outward effect —
 * pn_notify_receive() always calls the org.freedesktop.Notifications
 * "Notify" method on the session bus — and the templated summary/body
 * expansion is inseparable from that D-Bus call (the expand_placeholders
 * helper is static and only ever feeds pn_notify_send).  So these tests
 * NEVER call pn_node_receive_message: doing so would raise a real desktop
 * notification.  What is covered headlessly is everything observable
 * without firing: the full property surface (summary/body/icon/app-name/
 * urgency/timeout/replace) with its template defaults and round-trips, the
 * empty-string normalisation, the urgency enum registration, and the
 * sink shape.  The templating itself is left to the integration layer. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-notify.h"

static void
test_template_defaults (void)
{
    PnNode *node     = PN_NODE (pn_notify_new ());
    gchar  *summary  = NULL;
    gchar  *body     = NULL;
    gchar  *icon     = NULL;
    gchar  *app_name = NULL;

    /* The factory defaults make a useful notification out of the standard
     * message envelope without any configuration: the topic as the title,
     * the human-readable output as the body. */
    g_object_get (node,
                  "summary",  &summary,
                  "body",     &body,
                  "icon",     &icon,
                  "app-name", &app_name,
                  NULL);

    PN_CHECK_CMPSTR (summary,  ==, "${topic}");
    PN_CHECK_CMPSTR (body,     ==, "${data/output}");
    PN_CHECK_CMPSTR (icon,     ==, "dialog-information");
    PN_CHECK_CMPSTR (app_name, ==, "pipnode");

    g_free (summary);
    g_free (body);
    g_free (icon);
    g_free (app_name);
    g_object_unref (node);
}

static void
test_string_props_round_trip (void)
{
    PnNode *node = PN_NODE (pn_notify_new ());
    gchar  *s    = NULL;

    g_object_set (node, "summary", "Alert: ${data/host}", NULL);
    g_object_get (node, "summary", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "Alert: ${data/host}");
    g_free (s); s = NULL;

    g_object_set (node, "body", "value is ${data/value}", NULL);
    g_object_get (node, "body", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "value is ${data/value}");
    g_free (s); s = NULL;

    g_object_set (node, "icon", "/path/to/icon.png", NULL);
    g_object_get (node, "icon", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "/path/to/icon.png");
    g_free (s); s = NULL;

    g_object_set (node, "app-name", "Sensors", NULL);
    g_object_get (node, "app-name", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "Sensors");
    g_free (s);

    g_object_unref (node);
}

static void
test_empty_string_normalises (void)
{
    PnNode *node = PN_NODE (pn_notify_new ());
    gchar  *s    = (gchar *) "sentinel";

    /* A NULL / empty assignment is stored as "" (never left NULL), so the
     * D-Bus marshalling — which passes the strings straight through — is
     * always handed a valid string. */
    g_object_set (node, "summary", NULL, NULL);
    g_object_get (node, "summary", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "");
    g_free (s); s = NULL;

    g_object_set (node, "icon", "", NULL);
    g_object_get (node, "icon", &s, NULL);
    PN_CHECK_CMPSTR (s, ==, "");
    g_free (s);

    g_object_unref (node);
}

static void
test_urgency_default_and_round_trip (void)
{
    PnNode          *node = PN_NODE (pn_notify_new ());
    PnNotifyUrgency  u    = PN_NOTIFY_URGENCY_CRITICAL;

    /* Defaults to normal; all three wire-format values round-trip. */
    g_object_get (node, "urgency", &u, NULL);
    PN_CHECK_CMPINT (u, ==, PN_NOTIFY_URGENCY_NORMAL);

    g_object_set (node, "urgency", PN_NOTIFY_URGENCY_LOW, NULL);
    g_object_get (node, "urgency", &u, NULL);
    PN_CHECK_CMPINT (u, ==, PN_NOTIFY_URGENCY_LOW);

    g_object_set (node, "urgency", PN_NOTIFY_URGENCY_CRITICAL, NULL);
    g_object_get (node, "urgency", &u, NULL);
    PN_CHECK_CMPINT (u, ==, PN_NOTIFY_URGENCY_CRITICAL);

    /* The values match the freedesktop hint integers exactly. */
    PN_CHECK_CMPINT (PN_NOTIFY_URGENCY_LOW,      ==, 0);
    PN_CHECK_CMPINT (PN_NOTIFY_URGENCY_NORMAL,   ==, 1);
    PN_CHECK_CMPINT (PN_NOTIFY_URGENCY_CRITICAL, ==, 2);

    g_object_unref (node);
}

static void
test_urgency_enum_registered (void)
{
    GType       type  = pn_notify_urgency_get_type ();
    GEnumClass *klass = g_type_class_ref (type);
    GEnumValue *val;

    PN_CHECK (G_TYPE_IS_ENUM (type));

    /* The nicks are what the value serialises as, so pin them. */
    val = g_enum_get_value (klass, PN_NOTIFY_URGENCY_LOW);
    PN_CHECK (val != NULL && g_strcmp0 (val->value_nick, "low") == 0);
    val = g_enum_get_value (klass, PN_NOTIFY_URGENCY_CRITICAL);
    PN_CHECK (val != NULL && g_strcmp0 (val->value_nick, "critical") == 0);

    g_type_class_unref (klass);
}

static void
test_timeout_prop (void)
{
    PnNode        *node = PN_NODE (pn_notify_new ());
    GParamSpec    *pspec;
    GParamSpecInt *ispec;
    gint           t = 12345;

    /* Default -1 = "server decides"; range -1 .. 24h in ms. */
    g_object_get (node, "timeout-ms", &t, NULL);
    PN_CHECK_CMPINT (t, ==, -1);

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node),
                                          "timeout-ms");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_INT (pspec));
    ispec = G_PARAM_SPEC_INT (pspec);
    PN_CHECK_CMPINT (ispec->minimum,       ==, -1);
    PN_CHECK_CMPINT (ispec->maximum,       ==, 86400000);
    PN_CHECK_CMPINT (ispec->default_value, ==, -1);

    /* 0 = never expires (user must dismiss) round-trips. */
    g_object_set (node, "timeout-ms", 0, NULL);
    g_object_get (node, "timeout-ms", &t, NULL);
    PN_CHECK_CMPINT (t, ==, 0);

    g_object_unref (node);
}

static void
test_replace_prop_and_sink (void)
{
    PnNode   *node = PN_NODE (pn_notify_new ());
    gboolean  rep  = FALSE;

    /* Replace-in-place is on by default so a fast source does not stack
     * up bubbles. */
    g_object_get (node, "replace", &rep, NULL);
    PN_CHECK (rep == TRUE);

    g_object_set (node, "replace", FALSE, NULL);
    g_object_get (node, "replace", &rep, NULL);
    PN_CHECK (rep == FALSE);

    /* Pure sink: an input port, no output port.  (We assert this rather
     * than count emits, because receiving would fire a real
     * notification.) */
    PN_CHECK (pn_node_get_has_input (node));
    PN_CHECK_FALSE (pn_node_get_has_output (node));
    PN_CHECK_CMPSTR (pn_node_get_class_name (node), ==, "Notify");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-notify");
    pn_test_add ("template_defaults",      test_template_defaults);
    pn_test_add ("string_props_round_trip", test_string_props_round_trip);
    pn_test_add ("empty_string_normalises", test_empty_string_normalises);
    pn_test_add ("urgency_round_trip",     test_urgency_default_and_round_trip);
    pn_test_add ("urgency_enum_registered", test_urgency_enum_registered);
    pn_test_add ("timeout_prop",           test_timeout_prop);
    pn_test_add ("replace_prop_and_sink",  test_replace_prop_and_sink);
    return pn_test_run ();
}
