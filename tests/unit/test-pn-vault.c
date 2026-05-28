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

/* Unit tests for PnVault — the host's 0600 store of provisioned profile
 * instances (plugin ABI v5).  Covers: create / set / read; resolution
 * precedence (env override > stored value > schema default); the file is
 * created 0600; load/save round-trip; the per-type primary ("default")
 * profile; and the node bridge pn_node_get_profile() (empty ref follows the
 * primary, a set ref overrides it).  Headless — never opens a display. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-vault.h"
#include "pn-profile-schema.h"
#include "pn-node.h"
#include "pn-node-factory.h"

#include <glib/gstdio.h>

/* --- a throwaway node with a profile-ref property -------------------- */

#define TEST_TYPE_NODE (test_node_get_type ())
G_DECLARE_FINAL_TYPE (TestNode, test_node, TEST, NODE, PnNode)

struct _TestNode
{
    PnNode  parent_instance;
    gchar  *broker;
};

G_DEFINE_TYPE (TestNode, test_node, PN_TYPE_NODE)

enum { PROP_0, PROP_BROKER, N_PROPS };
static GParamSpec *test_node_props[N_PROPS];

static void
test_node_get_property (GObject *o, guint id, GValue *v, GParamSpec *p)
{
    TestNode *self = TEST_NODE (o);
    if (id == PROP_BROKER)
        g_value_set_string (v, self->broker);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_node_set_property (GObject *o, guint id, const GValue *v, GParamSpec *p)
{
    TestNode *self = TEST_NODE (o);
    if (id == PROP_BROKER)
    {
        g_free (self->broker);
        self->broker = g_value_dup_string (v);
    }
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_node_finalize (GObject *o)
{
    g_free (TEST_NODE (o)->broker);
    G_OBJECT_CLASS (test_node_parent_class)->finalize (o);
}

static void
test_node_class_init (TestNodeClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS (klass);

    oc->get_property = test_node_get_property;
    oc->set_property = test_node_set_property;
    oc->finalize     = test_node_finalize;

    test_node_props[PROP_BROKER] = g_param_spec_string (
            "broker", "Broker", "Referenced mqtt-broker profile id.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property (oc, PROP_BROKER,
                                    test_node_props[PROP_BROKER]);

    pn_param_spec_set_profile_ref (test_node_props[PROP_BROKER], "test-broker");
}

static void
test_node_init (TestNode *self)
{
    self->broker = g_strdup ("");
}

/* --- shared temp store ---------------------------------------------- */

static gchar *tmp_dir = NULL;

static gchar *
tmp_file (const gchar *name)
{
    return g_build_filename (tmp_dir, name, NULL);
}

/* --- tests ---------------------------------------------------------- */

static void
test_create_and_read (void)
{
    gchar     *path = tmp_file ("create.json");
    PnVault   *v    = pn_vault_new_for_path (path);
    PnProfile *p    = pn_vault_create_profile (v, "test-broker", "Home");
    gchar     *url;

    PN_CHECK (p != NULL);
    PN_CHECK_CMPSTR (pn_profile_get_type_id (p), ==, "test-broker");
    PN_CHECK_CMPSTR (pn_profile_get_name (p), ==, "Home");
    PN_CHECK_CMPSTR (pn_profile_get_id (p), ==, "home");   /* slugified */

    pn_profile_set_field (p, "url", "tcp://broker:1883");
    url = pn_profile_get_string (p, "url");
    PN_CHECK_CMPSTR (url, ==, "tcp://broker:1883");
    g_free (url);

    /* lookup by id */
    PN_CHECK (pn_vault_get_profile (v, "home") == p);
    PN_CHECK (pn_vault_get_profile (v, "nope") == NULL);

    g_object_unref (v);
    g_free (path);
}

static void
test_resolution_precedence (void)
{
    gchar     *path = tmp_file ("resolve.json");
    PnVault   *v    = pn_vault_new_for_path (path);
    PnProfile *p    = pn_vault_create_profile (v, "test-broker", "Home");
    gchar     *s;

    /* (1) nothing stored -> schema default */
    s = pn_profile_get_string (p, "scheme");
    PN_CHECK_CMPSTR (s, ==, "tcp");
    g_free (s);
    PN_CHECK_CMPINT (pn_profile_get_int (p, "port"), ==, 1883);

    /* (2) stored value beats the schema default */
    pn_profile_set_field (p, "scheme", "ssl");
    s = pn_profile_get_string (p, "scheme");
    PN_CHECK_CMPSTR (s, ==, "ssl");
    g_free (s);

    /* (3) env override beats the stored value */
    pn_profile_set_field (p, "password", "stored-pw");
    g_setenv ("PIPNODE_PROFILE_HOME_PASSWORD", "env-pw", TRUE);
    s = pn_profile_get_secret (p, "password");
    PN_CHECK_CMPSTR (s, ==, "env-pw");
    g_free (s);
    g_unsetenv ("PIPNODE_PROFILE_HOME_PASSWORD");
    s = pn_profile_get_secret (p, "password");
    PN_CHECK_CMPSTR (s, ==, "stored-pw");
    g_free (s);

    /* unknown field with no default -> "" / 0 */
    s = pn_profile_get_string (p, "nope");
    PN_CHECK_CMPSTR (s, ==, "");
    g_free (s);
    PN_CHECK_CMPINT (pn_profile_get_int (p, "nope"), ==, 0);

    g_object_unref (v);
    g_free (path);
}

static void
test_bool_and_permission (void)
{
    gchar     *path = tmp_file ("bool.json");
    PnVault   *v    = pn_vault_new_for_path (path);
    PnProfile *p    = pn_vault_create_profile (v, "test-broker", "Home");

    /* a permission is denied until explicitly granted */
    PN_CHECK_FALSE (pn_profile_get_permission (p, "control"));
    pn_profile_set_field (p, "control", "true");
    PN_CHECK (pn_profile_get_permission (p, "control"));
    pn_profile_set_field (p, "control", "0");
    PN_CHECK_FALSE (pn_profile_get_permission (p, "control"));

    /* truthy spellings */
    pn_profile_set_field (p, "flag", "yes");
    PN_CHECK (pn_profile_get_bool (p, "flag"));
    pn_profile_set_field (p, "flag", "off");
    PN_CHECK_FALSE (pn_profile_get_bool (p, "flag"));

    g_object_unref (v);
    g_free (path);
}

static void
test_file_is_0600 (void)
{
    gchar    *path = tmp_file ("perm.json");
    PnVault  *v    = pn_vault_new_for_path (path);
    GStatBuf  st;

    pn_vault_create_profile (v, "test-broker", "Home");   /* triggers a save */

    PN_CHECK_CMPINT (g_stat (path, &st), ==, 0);
    PN_CHECK_CMPINT ((gint) (st.st_mode & 0777), ==, 0600);

    g_object_unref (v);
    g_free (path);
}

static void
test_roundtrip (void)
{
    gchar *path = tmp_file ("roundtrip.json");

    {
        PnVault   *v = pn_vault_new_for_path (path);
        PnProfile *p = pn_vault_create_profile (v, "test-broker", "Home");
        pn_profile_set_field (p, "url", "tcp://x:1883");
        pn_profile_set_field (p, "password", "s3cr3t");
        g_object_unref (v);
    }
    {
        PnVault   *v = pn_vault_new_for_path (path);
        PnProfile *p = pn_vault_get_profile (v, "home");
        gchar     *u, *pw;

        PN_CHECK (p != NULL);
        u  = pn_profile_get_string (p, "url");
        pw = pn_profile_get_secret (p, "password");
        PN_CHECK_CMPSTR (u, ==, "tcp://x:1883");
        PN_CHECK_CMPSTR (pw, ==, "s3cr3t");
        g_free (u);
        g_free (pw);
        g_object_unref (v);
    }
    g_free (path);
}

static void
test_default_profile (void)
{
    gchar     *path = tmp_file ("default.json");
    PnVault   *v    = pn_vault_new_for_path (path);
    PnProfile *a, *b;

    PN_CHECK (pn_vault_get_default_profile (v, "test-broker") == NULL);

    a = pn_vault_create_profile (v, "test-broker", "Home");   /* first = primary */
    PN_CHECK (pn_vault_get_default_profile (v, "test-broker") == a);

    b = pn_vault_create_profile (v, "test-broker", "Work");
    PN_CHECK (pn_vault_get_default_profile (v, "test-broker") == a); /* unchanged */

    pn_vault_set_default_profile (v, pn_profile_get_id (b));
    PN_CHECK (pn_vault_get_default_profile (v, "test-broker") == b);

    /* deleting the primary clears the mapping; the lone survivor is primary */
    pn_vault_delete_profile (v, pn_profile_get_id (b));
    PN_CHECK (pn_vault_get_default_profile (v, "test-broker") == a);

    g_object_unref (v);
    g_free (path);
}

static void
test_node_ref_resolution (void)
{
    /* The node bridge uses the process-wide singleton, bound in main() to a
     * temp file via PIPNODE_CREDENTIALS_FILE. */
    PnVault   *v    = pn_vault_get_default ();
    PnProfile *home = pn_vault_create_profile (v, "test-broker", "Primary");
    PnProfile *work = pn_vault_create_profile (v, "test-broker", "Other");
    TestNode  *n    = g_object_new (TEST_TYPE_NODE, NULL);

    /* empty ref -> the type's primary */
    PN_CHECK (pn_node_get_profile (PN_NODE (n), "broker") == home);

    /* explicit ref -> that specific profile */
    g_object_set (n, "broker", pn_profile_get_id (work), NULL);
    PN_CHECK (pn_node_get_profile (PN_NODE (n), "broker") == work);

    /* dangling ref -> falls back to the primary */
    g_object_set (n, "broker", "does-not-exist", NULL);
    PN_CHECK (pn_node_get_profile (PN_NODE (n), "broker") == home);

    g_object_unref (n);
}

static void
register_test_schema (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("test-broker", "Test Broker");

    pn_profile_schema_field (s, "url",      "URL",      PN_FIELD_STRING);
    pn_profile_schema_field (s, "username", "Username", PN_FIELD_STRING);
    pn_profile_schema_field (s, "password", "Password", PN_FIELD_SECRET);
    pn_profile_schema_field (s, "scheme",   "Scheme",   PN_FIELD_STRING);
    pn_profile_schema_field_default (s, "scheme", "tcp");
    pn_profile_schema_field (s, "port",     "Port",     PN_FIELD_INT);
    pn_profile_schema_field_default (s, "port", "1883");

    pn_node_factory_register_profile_type (pn_node_factory_get_default (), s);
}

int
main (int argc, char **argv)
{
    gchar *cred;

    /* Isolate every test from the real ~/.config: a fresh temp store, with
     * the singleton bound to a file inside it BEFORE the first
     * pn_vault_get_default() call. */
    tmp_dir = g_dir_make_tmp ("pn-vault-XXXXXX", NULL);
    cred    = g_build_filename (tmp_dir, "singleton.json", NULL);
    g_setenv ("PIPNODE_CREDENTIALS_FILE", cred, TRUE);
    g_free (cred);

    pn_test_init (&argc, &argv, "pn-vault");
    register_test_schema ();

    pn_test_add ("create_and_read", test_create_and_read);
    pn_test_add ("resolution_precedence", test_resolution_precedence);
    pn_test_add ("bool_and_permission", test_bool_and_permission);
    pn_test_add ("file_is_0600", test_file_is_0600);
    pn_test_add ("roundtrip", test_roundtrip);
    pn_test_add ("default_profile", test_default_profile);
    pn_test_add ("node_ref_resolution", test_node_ref_resolution);
    return pn_test_run ();
}
