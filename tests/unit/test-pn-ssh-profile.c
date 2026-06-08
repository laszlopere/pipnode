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

/* Characterization test for the "ssh-login" profile schema (TODO #47.1).
 * The shell and host-monitoring SSH nodes resolve their login by looking
 * these exact field names up in a provisioned profile, and the argv builder
 * (item 47.3) maps the host-key-policy tokens onto OpenSSH options, so the
 * field set, its order, the two secret tags, the port default and the enum
 * choices are a load-bearing contract.  Renaming a field, dropping a secret
 * tag, or drifting the policy tokens would silently break login resolution
 * or leak a credential into a workflow file; this test trips on any of it. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ssh-profile.h"
#include "pn-node-factory.h"
#include "pn-profile-schema.h"

static PnProfileSchema *
ssh_schema (void)
{
    PnNodeFactory *factory = pn_node_factory_get_default ();

    /* The default factory already registers this in its built-in pass;
     * registering again is an idempotent no-op (the duplicate ref is
     * dropped), and keeps the test self-contained regardless of ordering. */
    pn_ssh_register_profile_type (factory);

    return pn_node_factory_lookup_profile_type (factory, PN_PROFILE_SSH);
}

static void
test_registered (void)
{
    PnProfileSchema *s = ssh_schema ();

    PN_CHECK (s != NULL);
    if (s == NULL)
        return;

    PN_CHECK_CMPSTR (pn_profile_schema_get_type_id (s), ==, "ssh-login");
    PN_CHECK_CMPSTR (pn_profile_schema_get_display_name (s), ==, "SSH Login");
}

static void
test_fields (void)
{
    PnProfileSchema *s = ssh_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    /* Six fields, in this order: username, port (default 22), identity-file,
     * the two secrets, and the host-key-policy enum. */
    PN_CHECK_CMPINT (pn_profile_schema_get_n_fields (s), ==, 6);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 0), ==, "username");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 0) == PN_FIELD_STRING);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 1), ==, "port");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 1) == PN_FIELD_INT);
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_default (s, 1), ==, "22");

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 2), ==, "identity-file");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 2) == PN_FIELD_FILE);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 3), ==, "passphrase");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 3) == PN_FIELD_SECRET);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 4), ==, "password");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 4) == PN_FIELD_SECRET);

    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 5), ==, "host-key-policy");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 5) == PN_FIELD_ENUM);
}

/* Both credential fields must be secrets: the manager masks them, the vault
 * stores them in the 0600 file, and the secret-scrub keeps them out of any
 * saved workflow. */
static void
test_secrets (void)
{
    PnProfileSchema *s = ssh_schema ();
    gint             idx;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    idx = pn_profile_schema_find_field (s, "passphrase");
    PN_CHECK_CMPINT (idx, ==, 3);
    PN_CHECK (pn_profile_schema_field_get_kind (s, idx) == PN_FIELD_SECRET);

    idx = pn_profile_schema_find_field (s, "password");
    PN_CHECK_CMPINT (idx, ==, 4);
    PN_CHECK (pn_profile_schema_field_get_kind (s, idx) == PN_FIELD_SECRET);
}

/* The host-key-policy enum offers exactly accept-new / strict / off and
 * defaults to accept-new (today's hard-coded behaviour). */
static void
test_host_key_policy (void)
{
    PnProfileSchema    *s = ssh_schema ();
    const gchar *const *choices;
    gint                idx;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    idx = pn_profile_schema_find_field (s, "host-key-policy");
    PN_CHECK_CMPINT (idx, ==, 5);

    choices = pn_profile_schema_field_get_choices (s, idx);
    PN_CHECK (choices != NULL);
    if (choices != NULL)
    {
        PN_CHECK_CMPSTR (choices[0], ==, "accept-new");
        PN_CHECK_CMPSTR (choices[1], ==, "strict");
        PN_CHECK_CMPSTR (choices[2], ==, "off");
        PN_CHECK (choices[3] == NULL);
    }

    PN_CHECK_CMPSTR (pn_profile_schema_field_get_default (s, idx),
                     ==, "accept-new");
}

/* Every field carries hover help: the credentials manager applies it as the
 * label + editor tooltip, so a missing one leaves a widget unexplained. */
static void
test_field_tooltips (void)
{
    PnProfileSchema *s = ssh_schema ();
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
        PN_CHECK (tip != NULL && *tip != '\0');
    }
}

/* The Help button on the Credentials dialog's SSH page is wired to this. */
static void
test_help_page (void)
{
    PnProfileSchema *s = ssh_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    PN_CHECK_CMPSTR (pn_profile_schema_get_help_page (s), ==, "SshProfile.html");
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ssh-profile");
    pn_test_add ("registered",      test_registered);
    pn_test_add ("fields",          test_fields);
    pn_test_add ("secrets",         test_secrets);
    pn_test_add ("host_key_policy", test_host_key_policy);
    pn_test_add ("field_tooltips",  test_field_tooltips);
    pn_test_add ("help_page",       test_help_page);
    return pn_test_run ();
}
