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

/* Unit tests for pn_mqtt_resolve_connection() -- the connection-identity
 * resolution shared by the MQTT Source and Sink.  Two behaviours matter:
 * with a provisioned profile every field comes from it (the password via
 * the secret path, so it follows the vault, not a plaintext property); with
 * no profile the inline fallback values are used, NULL coercing to "".  A
 * provisioned profile takes precedence over the inline values entirely.
 *
 * Like test-pn-vault, this binds a vault to a temp file (the profile fields
 * resolve env-override > stored > schema-default), so it is not part of the
 * filesystem-free test-pn-mqtt-util set. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt-util.h"
#include "pn-mqtt-profile.h"
#include "pn-node-factory.h"
#include "pn-vault.h"

static gchar *tmp_dir = NULL;

/* No profile -> inline values are used verbatim; the profile wins when set. */
static void
test_inline_fallback (void)
{
    gchar *url = NULL, *user = NULL, *pass = NULL, *cid = NULL;

    pn_mqtt_resolve_connection (NULL,
                                "tcp://inline:1883", "alice", "s3cret", "cid1",
                                &url, &user, &pass, &cid);

    PN_CHECK_CMPSTR (url,  ==, "tcp://inline:1883");
    PN_CHECK_CMPSTR (user, ==, "alice");
    PN_CHECK_CMPSTR (pass, ==, "s3cret");
    PN_CHECK_CMPSTR (cid,  ==, "cid1");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
}

/* NULL inline values coerce to "" (never NULL), so callers can always
 * g_free + dereference the result. */
static void
test_inline_nulls_become_empty (void)
{
    gchar *url = NULL, *user = NULL, *pass = NULL, *cid = NULL;

    pn_mqtt_resolve_connection (NULL, NULL, NULL, NULL, NULL,
                                &url, &user, &pass, &cid);

    PN_CHECK_CMPSTR (url,  ==, "");
    PN_CHECK_CMPSTR (user, ==, "");
    PN_CHECK_CMPSTR (pass, ==, "");
    PN_CHECK_CMPSTR (cid,  ==, "");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
}

/* With a provisioned profile, every field comes from it -- and the inline
 * values are ignored entirely. */
static void
test_profile_wins (void)
{
    gchar     *path = g_build_filename (tmp_dir, "resolve.json", NULL);
    PnVault   *v    = pn_vault_new_for_path (path);
    PnProfile *p    = pn_vault_create_profile (v, PN_PROFILE_TYPE_MQTT_BROKER,
                                               "Broker");
    gchar     *url = NULL, *user = NULL, *pass = NULL, *cid = NULL;

    pn_profile_set_field (p, "url",       "ssl://broker:8883");
    pn_profile_set_field (p, "username",  "vault-user");
    pn_profile_set_field (p, "password",  "vault-secret");
    pn_profile_set_field (p, "client-id", "vault-cid");

    /* Inline values are deliberately different; the profile must win. */
    pn_mqtt_resolve_connection (p,
                                "tcp://INLINE", "INLINE-U", "INLINE-P", "INLINE-C",
                                &url, &user, &pass, &cid);

    PN_CHECK_CMPSTR (url,  ==, "ssl://broker:8883");
    PN_CHECK_CMPSTR (user, ==, "vault-user");
    PN_CHECK_CMPSTR (pass, ==, "vault-secret");   /* via the secret path */
    PN_CHECK_CMPSTR (cid,  ==, "vault-cid");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
    g_object_unref (v);
    g_free (path);
}

int
main (int argc, char **argv)
{
    gchar *cred;
    int    rc;

    /* Bind the singleton vault to a temp store before first use, like
     * test-pn-vault, so nothing touches the real ~/.config. */
    tmp_dir = g_dir_make_tmp ("pn-mqtt-resolve-XXXXXX", NULL);
    cred    = g_build_filename (tmp_dir, "singleton.json", NULL);
    g_setenv ("PIPNODE_CREDENTIALS_FILE", cred, TRUE);
    g_free (cred);

    pn_test_init (&argc, &argv, "pn-mqtt-resolve");

    /* The mqtt-broker schema must be registered so the vault knows the
     * field set (and that "password" is a secret). */
    pn_mqtt_register_profile_type (pn_node_factory_get_default ());

    pn_test_add ("inline_fallback",          test_inline_fallback);
    pn_test_add ("inline_nulls_become_empty", test_inline_nulls_become_empty);
    pn_test_add ("profile_wins",             test_profile_wins);

    rc = pn_test_run ();

    g_free (tmp_dir);
    return rc;
}
