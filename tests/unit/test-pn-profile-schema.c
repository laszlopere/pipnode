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

/* Unit tests for PnProfileSchema — the GTK-free, declarative description of a
 * host-provisioned profile type (plugin ABI v5).  Pure data: build it, read it
 * back through the indexed accessors the credentials manager and the vault use.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-profile-schema.h"

static void
test_identity (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("mqtt-broker", "MQTT Broker");

    PN_CHECK_CMPSTR (pn_profile_schema_get_type_id (s), ==, "mqtt-broker");
    PN_CHECK_CMPSTR (pn_profile_schema_get_display_name (s), ==, "MQTT Broker");
    PN_CHECK_CMPINT (pn_profile_schema_get_n_fields (s), ==, 0);

    pn_profile_schema_unref (s);
}

static void
test_display_name_defaults_to_id (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("kodi-server", NULL);
    PN_CHECK_CMPSTR (pn_profile_schema_get_display_name (s), ==, "kodi-server");
    pn_profile_schema_unref (s);
}

static void
test_fields_in_order (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("kodi-server", "Kodi Server");

    pn_profile_schema_field (s, "host",     "Host",     PN_FIELD_STRING);
    pn_profile_schema_field (s, "port",     "Port",     PN_FIELD_INT);
    pn_profile_schema_field (s, "username", "Username", PN_FIELD_STRING);
    pn_profile_schema_field (s, "password", "Password", PN_FIELD_SECRET);
    pn_profile_schema_field (s, "control",  "Allow playback control",
                             PN_FIELD_PERMISSION);

    PN_CHECK_CMPINT (pn_profile_schema_get_n_fields (s), ==, 5);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 0), ==, "host");
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_label (s, 0), ==, "Host");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 0) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 1), ==, "port");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 1) == PN_FIELD_INT);

    PN_CHECK (pn_profile_schema_field_get_kind (s, 3) == PN_FIELD_SECRET);
    PN_CHECK (pn_profile_schema_field_get_kind (s, 4) == PN_FIELD_PERMISSION);

    /* out-of-range reads are safe */
    PN_CHECK (pn_profile_schema_field_name (s, 99) == NULL);

    pn_profile_schema_unref (s);
}

static void
test_label_defaults_to_name (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("t", "T");
    pn_profile_schema_field (s, "host", NULL, PN_FIELD_STRING);
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_label (s, 0), ==, "host");
    pn_profile_schema_unref (s);
}

static void
test_find_field (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("t", "T");
    pn_profile_schema_field (s, "host",     "Host",     PN_FIELD_STRING);
    pn_profile_schema_field (s, "password", "Password", PN_FIELD_SECRET);

    PN_CHECK_CMPINT (pn_profile_schema_find_field (s, "host"),     ==, 0);
    PN_CHECK_CMPINT (pn_profile_schema_find_field (s, "password"), ==, 1);
    PN_CHECK_CMPINT (pn_profile_schema_find_field (s, "nope"),     ==, -1);

    pn_profile_schema_unref (s);
}

static void
test_default_value (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("t", "T");
    pn_profile_schema_field (s, "port", "Port", PN_FIELD_INT);
    pn_profile_schema_field_default (s, "port", "8080");

    PN_CHECK_CMPSTR (pn_profile_schema_field_get_default (s, 0), ==, "8080");
    /* a field with no default reads NULL */
    pn_profile_schema_field (s, "host", "Host", PN_FIELD_STRING);
    PN_CHECK (pn_profile_schema_field_get_default (s, 1) == NULL);

    pn_profile_schema_unref (s);
}

static void
test_choices_force_enum (void)
{
    static const gchar *const choices[] = { "tcp", "ssl", NULL };
    PnProfileSchema *s = pn_profile_schema_new ("t", "T");
    const gchar *const *got;

    /* declared as a plain STRING, but adding choices promotes it to ENUM */
    pn_profile_schema_field (s, "scheme", "Scheme", PN_FIELD_STRING);
    pn_profile_schema_field_choices (s, "scheme", choices);

    PN_CHECK (pn_profile_schema_field_get_kind (s, 0) == PN_FIELD_ENUM);

    got = pn_profile_schema_field_get_choices (s, 0);
    PN_CHECK (got != NULL);
    if (got != NULL)
    {
        PN_CHECK_CMPSTR (got[0], ==, "tcp");
        PN_CHECK_CMPSTR (got[1], ==, "ssl");
        PN_CHECK (got[2] == NULL);
    }

    pn_profile_schema_unref (s);
}

static void
test_refcount (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("t", "T");
    pn_profile_schema_field (s, "host", "Host", PN_FIELD_STRING);

    pn_profile_schema_ref (s);
    pn_profile_schema_unref (s);   /* back to 1 — still alive */
    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 0), ==, "host");
    pn_profile_schema_unref (s);   /* freed */
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-profile-schema");
    pn_test_add ("identity", test_identity);
    pn_test_add ("display_name_defaults_to_id", test_display_name_defaults_to_id);
    pn_test_add ("fields_in_order", test_fields_in_order);
    pn_test_add ("label_defaults_to_name", test_label_defaults_to_name);
    pn_test_add ("find_field", test_find_field);
    pn_test_add ("default_value", test_default_value);
    pn_test_add ("choices_force_enum", test_choices_force_enum);
    pn_test_add ("refcount", test_refcount);
    return pn_test_run ();
}
