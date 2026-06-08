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

/* Characterization test for the shared SSH argv builder and its login
 * snapshot (TODO #47.3 / #47.4) — the path the shell and host-monitoring
 * nodes route through.  Two layers are covered:
 *
 *   - pn_ssh_build_argv(host, PnSshLogin, timeout, base): the exact argv it
 *     produces is a wire contract handed straight to g_spawn_sync.  With an
 *     ambient (or NULL) login it must reproduce the fixed argv the nodes
 *     hard-coded before the ssh-login profile type existed (BatchMode=yes,
 *     StrictHostKeyChecking=accept-new, ConnectTimeout=N); a populated login
 *     adds -l/-p/-i and maps the host-key policy.  Secrets are not part of
 *     the snapshot, so they can never leak onto the command line.
 *
 *   - pn_ssh_login_resolve(node, ref, out): the main-thread step that turns a
 *     node's "auth-profile" reference into that plain snapshot.  "" follows
 *     the primary SSH Login profile; with none configured it is ambient.
 *
 * #PnSshLogin exists because the nodes run ssh on a worker thread and the
 * vault-owned #PnProfile is main-thread only — the snapshot is the safe
 * value copy carried across.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ssh-profile.h"
#include "pn-node-factory.h"
#include "pn-node.h"
#include "pn-vault.h"

static gchar *tmp_dir = NULL;

/* --- a minimal PnNode subclass carrying an ssh-login profile-ref ------- */

#define TEST_TYPE_SSH_NODE (test_ssh_node_get_type ())
G_DECLARE_FINAL_TYPE (TestSshNode, test_ssh_node, TEST, SSH_NODE, PnNode)

struct _TestSshNode
{
    PnNode  parent_instance;
    gchar  *auth_profile;
};

G_DEFINE_TYPE (TestSshNode, test_ssh_node, PN_TYPE_NODE)

enum { PROP_0, PROP_AUTH_PROFILE, N_TEST_PROPS };
static GParamSpec *test_ssh_node_props[N_TEST_PROPS];

static void
test_ssh_node_get_property (GObject *o, guint id, GValue *v, GParamSpec *p)
{
    TestSshNode *self = TEST_SSH_NODE (o);
    if (id == PROP_AUTH_PROFILE)
        g_value_set_string (v, self->auth_profile);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_ssh_node_set_property (GObject *o, guint id, const GValue *v, GParamSpec *p)
{
    TestSshNode *self = TEST_SSH_NODE (o);
    if (id == PROP_AUTH_PROFILE)
    {
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (v);
    }
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_ssh_node_finalize (GObject *o)
{
    g_free (TEST_SSH_NODE (o)->auth_profile);
    G_OBJECT_CLASS (test_ssh_node_parent_class)->finalize (o);
}

static void
test_ssh_node_class_init (TestSshNodeClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS (klass);

    oc->get_property = test_ssh_node_get_property;
    oc->set_property = test_ssh_node_set_property;
    oc->finalize     = test_ssh_node_finalize;

    /* The real nodes install exactly this via pn_ssh_auth_profile_param_spec(). */
    test_ssh_node_props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();
    g_object_class_install_property (oc, PROP_AUTH_PROFILE,
                                     test_ssh_node_props[PROP_AUTH_PROFILE]);
}

static void
test_ssh_node_init (TestSshNode *self)
{
    self->auth_profile = g_strdup ("");
}

/* --- argv comparison helper ------------------------------------------- */

/* Assert @got matches @want element-for-element, same length.  Both are
 * NULL-terminated.  Reports the first mismatch via PN_CHECK_CMPSTR so a
 * failure names the offending argv slot. */
static void
check_argv (gchar **got, const gchar *const *want)
{
    guint i;

    PN_CHECK (got != NULL);
    if (got == NULL)
        return;

    for (i = 0; want[i] != NULL; i++)
    {
        PN_CHECK (got[i] != NULL);
        if (got[i] == NULL)
            return;
        PN_CHECK_CMPSTR (got[i], ==, want[i]);
    }

    /* Same length: @got must terminate exactly where @want does, so no
     * stray trailing element (e.g. a spurious -p / -i) slips through. */
    PN_CHECK (got[i] == NULL);
}

/* ---- build_argv: ambient (no profile) -------------------------------- */

/* A NULL login -> the fixed accept-new argv the nodes built inline before. */
static void
test_null_login (void)
{
    const gchar *base[]     = { "cat", "/proc/stat", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "remote.example",
        "cat", "/proc/stat",
        NULL
    };
    gchar **argv = pn_ssh_build_argv ("remote.example", NULL, 5, base);

    check_argv (argv, expected);
    g_strfreev (argv);
}

/* An ambient snapshot (has_profile FALSE) is identical to NULL: its other
 * fields are ignored entirely. */
static void
test_ambient_login_ignores_fields (void)
{
    const gchar *base[]     = { "cat", "/proc/stat", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "h",
        "cat", "/proc/stat",
        NULL
    };
    /* has_profile FALSE: username/port/identity must NOT be emitted. */
    PnSshLogin login = {
        .has_profile = FALSE,
        .username = (gchar *) "ignored", .identity_file = (gchar *) "/nope",
        .host_key_policy = (gchar *) "off", .port = 2222,
    };
    gchar **argv = pn_ssh_build_argv ("h", &login, 5, base);

    check_argv (argv, expected);
    g_strfreev (argv);
}

/* A 0 timeout falls back to the documented default of 5 seconds. */
static void
test_timeout_zero_defaults_to_5 (void)
{
    const gchar *base[] = { "cat", "/proc/loadavg", NULL };
    gchar      **argv   = pn_ssh_build_argv ("h", NULL, 0, base);

    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK_CMPSTR (argv[6], ==, "ConnectTimeout=5");
    g_strfreev (argv);
}

/* A non-default timeout is threaded straight into ConnectTimeout. */
static void
test_timeout_passthrough (void)
{
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    gchar      **argv   = pn_ssh_build_argv ("h", NULL, 29, base);

    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK_CMPSTR (argv[6], ==, "ConnectTimeout=29");
    g_strfreev (argv);
}

/* An empty remote command yields an empty (argv[0] == NULL) vector — the
 * builder never opens a bare login shell. */
static void
test_empty_base_argv (void)
{
    const gchar *base[] = { NULL };
    gchar      **argv   = pn_ssh_build_argv ("h", NULL, 5, base);

    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK (argv[0] == NULL);
    g_strfreev (argv);

    /* A NULL base behaves the same. */
    argv = pn_ssh_build_argv ("h", NULL, 5, NULL);
    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK (argv[0] == NULL);
    g_strfreev (argv);
}

/* The remote command is appended verbatim, including a pre-quoted single
 * `sh -c '<script>'` word the way the Connections node passes it — the
 * builder must not split or re-quote it. */
static void
test_base_argv_appended_verbatim (void)
{
    const gchar *base[]     = { "sh -c 'echo hi'", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "h",
        "sh -c 'echo hi'",
        NULL
    };
    gchar **argv = pn_ssh_build_argv ("h", NULL, 5, base);

    check_argv (argv, expected);
    g_strfreev (argv);
}

/* ---- build_argv: populated login ------------------------------------- */

/* A fully-populated login adds -l / -p / -i and maps strict -> yes. */
static void
test_login_full (void)
{
    const gchar *base[]     = { "cat", "/proc/stat", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "ConnectTimeout=7",
        "-l", "ops",
        "-p", "2222",
        "-i", "/home/ops/id_ed25519",
        "remote.example",
        "cat", "/proc/stat",
        NULL
    };
    PnSshLogin login = {
        .has_profile = TRUE,
        .username = (gchar *) "ops",
        .identity_file = (gchar *) "/home/ops/id_ed25519",
        .host_key_policy = (gchar *) "strict",
        .port = 2222,
    };
    gchar **argv = pn_ssh_build_argv ("remote.example", &login, 7, base);

    check_argv (argv, expected);
    g_strfreev (argv);
}

/* host-key-policy "off" maps to StrictHostKeyChecking=no. */
static void
test_login_policy_off (void)
{
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    PnSshLogin login = { .has_profile = TRUE,
                         .username = (gchar *) "",
                         .identity_file = (gchar *) "",
                         .host_key_policy = (gchar *) "off", .port = 22 };
    gchar **argv = pn_ssh_build_argv ("h", &login, 5, base);

    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK_CMPSTR (argv[4], ==, "StrictHostKeyChecking=no");
    g_strfreev (argv);
}

/* A login at its defaults (port 22, blank username / identity / policy)
 * reproduces the ambient argv: no -l, no -p (22 omitted), no -i, accept-new. */
static void
test_login_defaults_omitted (void)
{
    const gchar *base[]     = { "cat", "/proc/stat", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "h",
        "cat", "/proc/stat",
        NULL
    };
    PnSshLogin login = { .has_profile = TRUE,
                         .username = (gchar *) "",
                         .identity_file = (gchar *) "",
                         .host_key_policy = (gchar *) "", .port = 22 };
    gchar **argv = pn_ssh_build_argv ("h", &login, 5, base);

    check_argv (argv, expected);
    g_strfreev (argv);
}

/* Username alone, on the default port, adds only -l (no -p for port 22). */
static void
test_login_username_only (void)
{
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    PnSshLogin login = { .has_profile = TRUE,
                         .username = (gchar *) "alice",
                         .identity_file = (gchar *) "",
                         .host_key_policy = (gchar *) "accept-new", .port = 22 };
    gchar **argv = pn_ssh_build_argv ("h", &login, 5, base);

    PN_CHECK (argv != NULL);
    if (argv != NULL)
    {
        PN_CHECK_CMPSTR (argv[7], ==, "-l");
        PN_CHECK_CMPSTR (argv[8], ==, "alice");
        /* host follows directly — no -p slipped in for the default port. */
        PN_CHECK_CMPSTR (argv[9], ==, "h");
    }
    g_strfreev (argv);
}

/* ---- pn_ssh_login_resolve -------------------------------------------- */

/* With no SSH Login profile configured, "" resolves to an ambient snapshot
 * (has_profile FALSE) — today's behaviour for an un-provisioned node.  This
 * runs FIRST, before any profile is created in the singleton vault. */
static void
test_resolve_ambient (void)
{
    TestSshNode *node = g_object_new (TEST_TYPE_SSH_NODE, NULL);
    PnSshLogin   login = { 0 };

    pn_ssh_login_resolve (PN_NODE (node), "auth-profile", &login);

    PN_CHECK (login.has_profile == FALSE);
    /* Strings are still allocated (never NULL) so build_argv / clear are safe. */
    PN_CHECK_CMPSTR (login.username, ==, "");
    PN_CHECK_CMPSTR (login.identity_file, ==, "");
    PN_CHECK_CMPSTR (login.host_key_policy, ==, "");
    PN_CHECK_CMPINT (login.port, ==, 0);

    pn_ssh_login_clear (&login);
    g_object_unref (node);
}

/* An empty ref follows the type's primary profile; its fields land in the
 * snapshot and then flow through the builder end-to-end. */
static void
test_resolve_follows_primary (void)
{
    PnVault     *v = pn_vault_get_default ();
    PnProfile   *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "Primary");
    TestSshNode *node;
    PnSshLogin   login = { 0 };
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "ConnectTimeout=5",
        "-l", "ops",
        "-p", "2222",
        "-i", "/home/ops/id",
        "h",
        "cat", "/proc/stat",
        NULL
    };
    gchar **argv;

    PN_CHECK (p != NULL);
    if (p == NULL)
        return;

    pn_profile_set_field (p, "username",        "ops");
    pn_profile_set_field (p, "port",            "2222");
    pn_profile_set_field (p, "identity-file",   "/home/ops/id");
    pn_profile_set_field (p, "host-key-policy", "strict");
    /* Secrets exist but must never surface in the snapshot or argv. */
    pn_profile_set_field (p, "passphrase",      "keypass");
    pn_profile_set_field (p, "password",        "loginpass");

    node = g_object_new (TEST_TYPE_SSH_NODE, NULL);   /* default ref "" */
    pn_ssh_login_resolve (PN_NODE (node), "auth-profile", &login);

    PN_CHECK (login.has_profile == TRUE);
    PN_CHECK_CMPSTR (login.username, ==, "ops");
    PN_CHECK_CMPINT (login.port, ==, 2222);
    PN_CHECK_CMPSTR (login.identity_file, ==, "/home/ops/id");
    PN_CHECK_CMPSTR (login.host_key_policy, ==, "strict");

    argv = pn_ssh_build_argv ("h", &login, 5, base);
    check_argv (argv, expected);

    /* No argv word equals either secret. */
    if (argv != NULL)
    {
        guint i;
        for (i = 0; argv[i] != NULL; i++)
        {
            PN_CHECK (g_strcmp0 (argv[i], "keypass")   != 0);
            PN_CHECK (g_strcmp0 (argv[i], "loginpass") != 0);
        }
    }

    g_strfreev (argv);
    pn_ssh_login_clear (&login);
    g_object_unref (node);
}

/* An explicit id resolves to that profile; the "Custom settings" sentinel
 * resolves to ambient (no primary fallback). */
static void
test_resolve_explicit_and_custom (void)
{
    PnVault     *v = pn_vault_get_default ();
    PnProfile   *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "Box");
    TestSshNode *node = g_object_new (TEST_TYPE_SSH_NODE, NULL);
    PnSshLogin   login = { 0 };

    PN_CHECK (p != NULL);
    if (p == NULL)
    {
        g_object_unref (node);
        return;
    }
    pn_profile_set_field (p, "username", "boxuser");

    g_object_set (node, "auth-profile", pn_profile_get_id (p), NULL);
    pn_ssh_login_resolve (PN_NODE (node), "auth-profile", &login);
    PN_CHECK (login.has_profile == TRUE);
    PN_CHECK_CMPSTR (login.username, ==, "boxuser");
    pn_ssh_login_clear (&login);

    /* The "Custom settings" sentinel means "no profile" -> ambient. */
    g_object_set (node, "auth-profile", PN_PROFILE_REF_CUSTOM, NULL);
    pn_ssh_login_resolve (PN_NODE (node), "auth-profile", &login);
    PN_CHECK (login.has_profile == FALSE);
    pn_ssh_login_clear (&login);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    gchar *cred;
    int    rc;

    /* Bind the singleton vault to a throwaway store before first use so the
     * resolve cases never touch the real ~/.config. */
    tmp_dir = g_dir_make_tmp ("pn-ssh-build-argv-XXXXXX", NULL);
    cred    = g_build_filename (tmp_dir, "singleton.json", NULL);
    g_setenv ("PIPNODE_CREDENTIALS_FILE", cred, TRUE);
    g_free (cred);

    pn_test_init (&argc, &argv, "pn-ssh-build-argv");

    /* The vault needs the ssh-login schema to know the field set (and the
     * port default) before pn_vault_create_profile is called. */
    pn_ssh_register_profile_type (pn_node_factory_get_default ());

    /* build_argv — independent of the vault. */
    pn_test_add ("null_login",                test_null_login);
    pn_test_add ("ambient_login",             test_ambient_login_ignores_fields);
    pn_test_add ("timeout_zero_defaults_5",   test_timeout_zero_defaults_to_5);
    pn_test_add ("timeout_passthrough",       test_timeout_passthrough);
    pn_test_add ("empty_base_argv",           test_empty_base_argv);
    pn_test_add ("base_argv_verbatim",        test_base_argv_appended_verbatim);
    pn_test_add ("login_full",                test_login_full);
    pn_test_add ("login_policy_off",          test_login_policy_off);
    pn_test_add ("login_defaults_omitted",    test_login_defaults_omitted);
    pn_test_add ("login_username_only",       test_login_username_only);

    /* resolve — ambient case MUST run before any profile is created. */
    pn_test_add ("resolve_ambient",           test_resolve_ambient);
    pn_test_add ("resolve_follows_primary",   test_resolve_follows_primary);
    pn_test_add ("resolve_explicit_custom",   test_resolve_explicit_and_custom);

    rc = pn_test_run ();

    g_free (tmp_dir);
    return rc;
}
