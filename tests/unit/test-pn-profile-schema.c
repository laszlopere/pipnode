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
test_visible_when (void)
{
    static const gchar *const key_only[]  = { "key-file", NULL };
    static const gchar *const pwd_or_key[] = { "key-file", "password", NULL };
    PnProfileSchema *s = pn_profile_schema_new ("ssh-login", "SSH Login");
    const gchar *const *vals;

    /* auth-schema (controller) + host (always) + a couple of dependents */
    pn_profile_schema_field (s, "auth-schema", "Authentication",
                             PN_FIELD_ENUM);
    pn_profile_schema_field (s, "host",        "Host",     PN_FIELD_STRING);
    pn_profile_schema_field (s, "identity",    "Key file", PN_FIELD_FILE);
    pn_profile_schema_field (s, "username",    "Username", PN_FIELD_STRING);

    pn_profile_schema_field_visible_when (s, "identity", "auth-schema", key_only);
    pn_profile_schema_field_visible_when (s, "username", "auth-schema",
                                          pwd_or_key);

    /* Read-back: a field with no rule reports NULL. */
    PN_CHECK (pn_profile_schema_field_get_visible_when (s, 0) == NULL);
    PN_CHECK (pn_profile_schema_field_get_visible_when (s, 1) == NULL);
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_visible_when (s, 2), ==,
                     "auth-schema");

    vals = pn_profile_schema_field_get_visible_values (s, 2);
    PN_CHECK (vals != NULL);
    if (vals != NULL)
    {
        PN_CHECK_CMPSTR (vals[0], ==, "key-file");
        PN_CHECK (vals[1] == NULL);
    }

    /* Evaluator: no rule -> always visible regardless of controller value. */
    PN_CHECK (pn_profile_schema_field_visible_for (s, 1, "anything") == TRUE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 1, NULL) == TRUE);

    /* identity: only for "key-file". */
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, "key-file")  == TRUE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, "password")  == FALSE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, "passwordless-simple")
              == FALSE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, NULL) == FALSE);

    /* username: for both key-file and password. */
    PN_CHECK (pn_profile_schema_field_visible_for (s, 3, "key-file") == TRUE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 3, "password") == TRUE);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 3, "passwordless-simple")
              == FALSE);

    /* A NULL controller field clears the rule -> always visible again. */
    pn_profile_schema_field_visible_when (s, "identity", NULL, NULL);
    PN_CHECK (pn_profile_schema_field_get_visible_when (s, 2) == NULL);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, "password") == TRUE);

    /* A rule with an empty value set -> never visible. */
    pn_profile_schema_field_visible_when (s, "identity", "auth-schema", NULL);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 2, "key-file") == FALSE);

    /* Out-of-range index is safe (treated as always-visible). */
    PN_CHECK (pn_profile_schema_field_get_visible_when (s, 99) == NULL);
    PN_CHECK (pn_profile_schema_field_visible_for (s, 99, "x") == TRUE);

    pn_profile_schema_unref (s);
}

static void
test_help_page (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("kodi-server", "Kodi Server");

    /* Default: no help page registered. */
    PN_CHECK (pn_profile_schema_get_help_page (s) == NULL);

    pn_profile_schema_set_help_page (s, "KodiServer.html");
    PN_CHECK_CMPSTR (pn_profile_schema_get_help_page (s), ==, "KodiServer.html");

    /* %NULL and empty string both clear. */
    pn_profile_schema_set_help_page (s, NULL);
    PN_CHECK (pn_profile_schema_get_help_page (s) == NULL);

    pn_profile_schema_set_help_page (s, "Again.html");
    PN_CHECK_CMPSTR (pn_profile_schema_get_help_page (s), ==, "Again.html");
    pn_profile_schema_set_help_page (s, "");
    PN_CHECK (pn_profile_schema_get_help_page (s) == NULL);

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
    pn_test_add ("visible_when", test_visible_when);
    pn_test_add ("help_page", test_help_page);
    pn_test_add ("refcount", test_refcount);
    return pn_test_run ();
}
