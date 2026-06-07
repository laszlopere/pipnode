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

/* Characterization test for the "mqtt-broker" profile schema.  The MQTT
 * Source, Sink, and any out-of-tree PnMqtt subclass resolve their broker
 * connection by looking these exact field names up in a provisioned profile
 * (see resolve_connection in pn-mqtt.c / pn-mqtt-sink.c), so the field set,
 * its order, and — critically — that "password" is a %PN_FIELD_SECRET (and
 * therefore scrubbed on save and stored only in the 0600 vault) are a
 * load-bearing contract.  Renaming a field or dropping the secret tag would
 * silently break credential resolution or leak the password into a workflow
 * file; this test trips on either. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt-profile.h"
#include "pn-node-factory.h"
#include "pn-profile-schema.h"

static PnProfileSchema *
mqtt_schema (void)
{
    PnNodeFactory *factory = pn_node_factory_get_default ();

    /* The default factory already registers this in its built-in pass;
     * registering again is an idempotent no-op (the duplicate ref is
     * dropped), and keeps the test self-contained regardless of that
     * ordering. */
    pn_mqtt_register_profile_type (factory);

    return pn_node_factory_lookup_profile_type (factory,
                                                PN_PROFILE_TYPE_MQTT_BROKER);
}

static void
test_registered (void)
{
    PnProfileSchema *s = mqtt_schema ();

    PN_CHECK (s != NULL);
    if (s == NULL)
        return;

    PN_CHECK_CMPSTR (pn_profile_schema_get_type_id (s), ==, "mqtt-broker");
    PN_CHECK_CMPSTR (pn_profile_schema_get_display_name (s), ==, "MQTT Broker");
}

static void
test_fields (void)
{
    PnProfileSchema *s = mqtt_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    /* Nine fields, in this order: the four identity fields, then the TLS
     * trust material + keep-alive added for review M3.  The CA / mTLS paths
     * are plain strings (filesystem paths the host trusts, not credentials),
     * the insecure escape hatch is a bool, and keep-alive is an int. */
    PN_CHECK_CMPINT (pn_profile_schema_get_n_fields (s), ==, 9);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 0), ==, "url");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 0) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 1), ==, "username");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 1) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 2), ==, "password");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 2) == PN_FIELD_SECRET);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 3), ==, "client-id");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 3) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 4), ==, "ca-file");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 4) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 5), ==, "client-cert");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 5) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 6), ==, "client-key");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 6) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 7), ==, "tls-insecure");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 7) == PN_FIELD_BOOL);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 8), ==, "keepalive");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 8) == PN_FIELD_INT);
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_default (s, 8), ==, "60");
}

/* The single most important assertion: the password is a secret, so the
 * credentials manager masks it, the vault stores it in the 0600 file, and
 * the secret-scrub keeps it out of any saved workflow. */
static void
test_password_is_secret (void)
{
    PnProfileSchema *s = mqtt_schema ();
    gint             idx;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    idx = pn_profile_schema_find_field (s, "password");
    PN_CHECK_CMPINT (idx, ==, 2);
    PN_CHECK (pn_profile_schema_field_get_kind (s, idx) == PN_FIELD_SECRET);
}

/* Every field carries hover help: the credentials manager applies it as the
 * label + editor tooltip, so a missing one leaves a widget unexplained. */
static void
test_field_tooltips (void)
{
    PnProfileSchema *s = mqtt_schema ();
    guint            i, n;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    n = pn_profile_schema_get_n_fields (s);
    for (i = 0; i < n; i++)
    {
        const gchar *tip = pn_profile_schema_field_get_tooltip (s, i);
        /* Non-NULL and non-empty for each field. */
        PN_CHECK (tip != NULL && *tip != '\0');
    }
}

/* The Help button on the Credentials dialog's MQTT page is wired to this. */
static void
test_help_page (void)
{
    PnProfileSchema *s = mqtt_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    PN_CHECK_CMPSTR (pn_profile_schema_get_help_page (s),
                     ==, "MqttBrokerProfile.html");
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mqtt-profile");
    pn_test_add ("registered",        test_registered);
    pn_test_add ("fields",            test_fields);
    pn_test_add ("password_is_secret", test_password_is_secret);
    pn_test_add ("field_tooltips",    test_field_tooltips);
    pn_test_add ("help_page",         test_help_page);
    return pn_test_run ();
}
