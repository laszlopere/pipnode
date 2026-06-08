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

/* Characterization test for pn_ssh_build_argv (TODO #47.3) — the one shared
 * SSH argv builder the shell and host-monitoring nodes route through.  The
 * exact argv it produces is a wire contract: it is what every SSH node hands
 * g_spawn_sync, so drifting a flag, its order, or the host-key-policy mapping
 * silently changes how (or whether) those nodes log in.  Two invariants are
 * load-bearing here:
 *
 *   - With NO profile the builder must reproduce the fixed argv the nodes
 *     hard-coded before the ssh-login profile type existed
 *     (BatchMode=yes, StrictHostKeyChecking=accept-new, ConnectTimeout=N),
 *     so the conversion in 47.3 is behaviour-preserving for the common case.
 *   - With a profile, username / port / identity / host-key-policy shape the
 *     argv, while the passphrase / password secrets are NEVER emitted (a
 *     non-interactive feed is item 47.5; BatchMode forbids a prompt today).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ssh-profile.h"
#include "pn-node-factory.h"
#include "pn-vault.h"

static gchar *tmp_dir = NULL;

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

/* No profile -> the fixed accept-new argv the nodes built inline before. */
static void
test_no_profile (void)
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

/* A 0 timeout falls back to the documented default of 5 seconds. */
static void
test_timeout_zero_defaults_to_5 (void)
{
    const gchar *base[]     = { "cat", "/proc/loadavg", NULL };
    const gchar *expected[] = {
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=5",
        "h",
        "cat", "/proc/loadavg",
        NULL
    };
    gchar **argv = pn_ssh_build_argv ("h", NULL, 0, base);

    check_argv (argv, expected);
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

/* A fully-populated profile adds -l / -p / -i and maps strict -> yes, and
 * does NOT leak the passphrase / password into the argv. */
static void
test_profile_full (void)
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
    PnVault   *v = pn_vault_get_default ();
    PnProfile *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "Full");
    gchar    **argv;

    PN_CHECK (p != NULL);
    if (p == NULL)
        return;

    pn_profile_set_field (p, "username",        "ops");
    pn_profile_set_field (p, "port",            "2222");
    pn_profile_set_field (p, "identity-file",   "/home/ops/id_ed25519");
    pn_profile_set_field (p, "host-key-policy", "strict");
    /* Secrets are set but must never appear on the command line. */
    pn_profile_set_field (p, "passphrase",      "keypass");
    pn_profile_set_field (p, "password",        "loginpass");

    argv = pn_ssh_build_argv ("remote.example", p, 7, base);
    check_argv (argv, expected);

    /* Belt and braces: no argv word equals either secret. */
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
}

/* host-key-policy "off" maps to StrictHostKeyChecking=no. */
static void
test_profile_policy_off (void)
{
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    PnVault     *v = pn_vault_get_default ();
    PnProfile   *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "Off");
    gchar      **argv;

    PN_CHECK (p != NULL);
    if (p == NULL)
        return;

    pn_profile_set_field (p, "host-key-policy", "off");

    argv = pn_ssh_build_argv ("h", p, 5, base);
    PN_CHECK (argv != NULL);
    if (argv != NULL)
        PN_CHECK_CMPSTR (argv[4], ==, "StrictHostKeyChecking=no");
    g_strfreev (argv);
}

/* A profile left at its defaults (port 22, blank username / identity /
 * policy) reproduces the no-profile argv: no -l, no -p (22 is the default,
 * so it is omitted to stay minimal), no -i, accept-new. */
static void
test_profile_defaults_omitted (void)
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
    PnVault   *v = pn_vault_get_default ();
    PnProfile *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "Defaults");
    gchar    **argv;

    PN_CHECK (p != NULL);
    if (p == NULL)
        return;

    /* Nothing set: port resolves to the schema default "22" -> omitted. */
    argv = pn_ssh_build_argv ("h", p, 5, base);
    check_argv (argv, expected);
    g_strfreev (argv);
}

/* Username alone, on the default port, adds only -l (no -p for port 22). */
static void
test_profile_username_only (void)
{
    const gchar *base[] = { "cat", "/proc/stat", NULL };
    PnVault     *v = pn_vault_get_default ();
    PnProfile   *p = pn_vault_create_profile (v, PN_PROFILE_SSH, "UserOnly");
    gchar      **argv;

    PN_CHECK (p != NULL);
    if (p == NULL)
        return;

    pn_profile_set_field (p, "username", "alice");

    argv = pn_ssh_build_argv ("h", p, 5, base);
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

int
main (int argc, char **argv)
{
    gchar *cred;
    int    rc;

    /* Bind the singleton vault to a throwaway store before first use so the
     * profile-backed cases never touch the real ~/.config. */
    tmp_dir = g_dir_make_tmp ("pn-ssh-build-argv-XXXXXX", NULL);
    cred    = g_build_filename (tmp_dir, "singleton.json", NULL);
    g_setenv ("PIPNODE_CREDENTIALS_FILE", cred, TRUE);
    g_free (cred);

    pn_test_init (&argc, &argv, "pn-ssh-build-argv");

    /* The vault needs the ssh-login schema to know the field set (and the
     * port default) before pn_vault_create_profile is called. */
    pn_ssh_register_profile_type (pn_node_factory_get_default ());

    pn_test_add ("no_profile",                test_no_profile);
    pn_test_add ("timeout_zero_defaults_5",   test_timeout_zero_defaults_to_5);
    pn_test_add ("timeout_passthrough",       test_timeout_passthrough);
    pn_test_add ("empty_base_argv",           test_empty_base_argv);
    pn_test_add ("base_argv_verbatim",        test_base_argv_appended_verbatim);
    pn_test_add ("profile_full",              test_profile_full);
    pn_test_add ("profile_policy_off",        test_profile_policy_off);
    pn_test_add ("profile_defaults_omitted",  test_profile_defaults_omitted);
    pn_test_add ("profile_username_only",     test_profile_username_only);

    rc = pn_test_run ();

    g_free (tmp_dir);
    return rc;
}
