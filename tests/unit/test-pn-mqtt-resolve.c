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
 * resolution shared by the MQTT Source and Sink.  Resolution is PER-FIELD:
 * for each of url / username / password / client-id, a non-empty inline value
 * wins, otherwise the profile's field (when a profile resolved) is used, else
 * "".  So a typed Broker URL takes effect even with a profile selected, while
 * the fields left blank still come from the profile (the password via the
 * secret path, so a node migrated into a profile never loses its stored
 * password).  With no profile the inline values are used, NULL coercing to "".
 *
 * Like test-pn-vault, this binds a vault to a temp file (the profile fields
 * resolve env-override > stored > schema-default), so it is not part of the
 * filesystem-free test-pn-mqtt-util set. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt.h"
#include "pn-mqtt-util.h"
#include "pn-mqtt-profile.h"
#include "pn-node.h"
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
                                &url, &user, &pass, &cid, NULL);

    PN_CHECK_CMPSTR (url,  ==, "tcp://inline:1883");
    PN_CHECK_CMPSTR (user, ==, "alice");
    PN_CHECK_CMPSTR (pass, ==, "s3cret");
    PN_CHECK_CMPSTR (cid,  ==, "cid1");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
}

/* Inline ("Custom settings") mode exposes no TLS / keep-alive UI, so the
 * options resolve to their unset defaults: no CA bundle, no mTLS pair,
 * verification on, keep-alive 0 (-> the node's compiled default). */
static void
test_inline_opts_default (void)
{
    gchar         *url = NULL;
    PnMqttConnOpts opts = { 0 };

    pn_mqtt_resolve_connection (NULL, "tcp://inline", NULL, NULL, NULL,
                                &url, NULL, NULL, NULL, &opts);

    PN_CHECK_CMPSTR (opts.ca_file,     ==, "");
    PN_CHECK_CMPSTR (opts.client_cert, ==, "");
    PN_CHECK_CMPSTR (opts.client_key,  ==, "");
    PN_CHECK (opts.tls_insecure == FALSE);
    PN_CHECK_CMPINT (opts.keepalive, ==, 0);

    g_free (url);
    pn_mqtt_conn_opts_clear (&opts);
}

/* NULL inline values coerce to "" (never NULL), so callers can always
 * g_free + dereference the result. */
static void
test_inline_nulls_become_empty (void)
{
    gchar *url = NULL, *user = NULL, *pass = NULL, *cid = NULL;

    pn_mqtt_resolve_connection (NULL, NULL, NULL, NULL, NULL,
                                &url, &user, &pass, &cid, NULL);

    PN_CHECK_CMPSTR (url,  ==, "");
    PN_CHECK_CMPSTR (user, ==, "");
    PN_CHECK_CMPSTR (pass, ==, "");
    PN_CHECK_CMPSTR (cid,  ==, "");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
}

/* With a provisioned profile, every field comes from it -- the inline values
 * are ignored entirely (profile XOR inline). */
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
                                &url, &user, &pass, &cid, NULL);

    PN_CHECK_CMPSTR (url,  ==, "ssl://broker:8883");
    PN_CHECK_CMPSTR (user, ==, "vault-user");
    PN_CHECK_CMPSTR (pass, ==, "vault-secret");   /* via the secret path */
    PN_CHECK_CMPSTR (cid,  ==, "vault-cid");

    g_free (url); g_free (user); g_free (pass); g_free (cid);
    g_object_unref (v);
    g_free (path);
}

/* TLS trust material + keep-alive come from the profile (review M3).  A field
 * left unset resolves to its schema default: "" for the paths, FALSE for the
 * insecure toggle, and 60 for keep-alive. */
static void
test_profile_tls_opts (void)
{
    gchar         *path = g_build_filename (tmp_dir, "tls.json", NULL);
    PnVault       *v    = pn_vault_new_for_path (path);
    PnProfile     *p    = pn_vault_create_profile (v, PN_PROFILE_TYPE_MQTT_BROKER,
                                                   "TLS Broker");
    gchar         *url  = NULL;
    PnMqttConnOpts opts = { 0 };

    pn_profile_set_field (p, "url",          "ssl://broker:8883");
    pn_profile_set_field (p, "ca-file",      "/etc/pki/ca.pem");
    pn_profile_set_field (p, "client-cert",  "/etc/pki/client.pem");
    pn_profile_set_field (p, "client-key",   "/etc/pki/client.key");
    pn_profile_set_field (p, "tls-insecure", "true");
    pn_profile_set_field (p, "keepalive",    "30");

    pn_mqtt_resolve_connection (p, NULL, NULL, NULL, NULL,
                                &url, NULL, NULL, NULL, &opts);

    PN_CHECK_CMPSTR (url,               ==, "ssl://broker:8883");
    PN_CHECK_CMPSTR (opts.ca_file,      ==, "/etc/pki/ca.pem");
    PN_CHECK_CMPSTR (opts.client_cert,  ==, "/etc/pki/client.pem");
    PN_CHECK_CMPSTR (opts.client_key,   ==, "/etc/pki/client.key");
    PN_CHECK (opts.tls_insecure == TRUE);
    PN_CHECK_CMPINT (opts.keepalive,    ==, 30);

    g_free (url);
    pn_mqtt_conn_opts_clear (&opts);
    g_object_unref (v);
    g_free (path);
}

/* An unconfigured profile yields the schema defaults: empty TLS paths,
 * verification on, and the 60 s keep-alive default. */
static void
test_profile_opts_defaults (void)
{
    gchar         *path = g_build_filename (tmp_dir, "tls-default.json", NULL);
    PnVault       *v    = pn_vault_new_for_path (path);
    PnProfile     *p    = pn_vault_create_profile (v, PN_PROFILE_TYPE_MQTT_BROKER,
                                                   "Plain Broker");
    PnMqttConnOpts opts = { 0 };

    pn_profile_set_field (p, "url", "ssl://broker:8883");

    pn_mqtt_resolve_connection (p, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, &opts);

    PN_CHECK_CMPSTR (opts.ca_file,     ==, "");
    PN_CHECK_CMPSTR (opts.client_cert, ==, "");
    PN_CHECK_CMPSTR (opts.client_key,  ==, "");
    PN_CHECK (opts.tls_insecure == FALSE);
    PN_CHECK_CMPINT (opts.keepalive,   ==, 60);   /* schema default */

    pn_mqtt_conn_opts_clear (&opts);
    g_object_unref (v);
    g_free (path);
}

/* The "Custom settings" sentinel makes pn_node_get_profile() return NULL even
 * when a primary profile exists, so the node resolves to its inline settings.
 * Empty ("Default") still follows the primary, and an explicit id resolves it. */
static void
test_custom_sentinel_uses_inline (void)
{
    PnVault   *v = pn_vault_get_default ();   /* bound to the temp singleton */
    PnProfile *primary;
    PnNode    *node;

    /* First profile of its type becomes the primary. */
    primary = pn_vault_create_profile (v, PN_PROFILE_TYPE_MQTT_BROKER, "Primary");
    PN_CHECK (primary != NULL);

    node = g_object_new (PN_TYPE_MQTT, NULL);

    /* Default (empty ref) follows the primary. */
    g_object_set (node, "broker-profile", "", NULL);
    {
        PnProfile *got = pn_node_get_profile (node, "broker-profile");
        PN_CHECK (got == primary);
    }

    /* Custom sentinel -> no profile (inline settings are used). */
    g_object_set (node, "broker-profile", PN_PROFILE_REF_CUSTOM, NULL);
    PN_CHECK (pn_node_get_profile (node, "broker-profile") == NULL);

    /* An explicit id still resolves to that profile. */
    g_object_set (node, "broker-profile", pn_profile_get_id (primary), NULL);
    PN_CHECK (pn_node_get_profile (node, "broker-profile") == primary);

    g_object_unref (node);
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
    pn_test_add ("inline_opts_default",      test_inline_opts_default);
    pn_test_add ("profile_wins",             test_profile_wins);
    pn_test_add ("profile_tls_opts",         test_profile_tls_opts);
    pn_test_add ("profile_opts_defaults",    test_profile_opts_defaults);
    pn_test_add ("custom_sentinel_uses_inline", test_custom_sentinel_uses_inline);

    rc = pn_test_run ();

    g_free (tmp_dir);
    return rc;
}
