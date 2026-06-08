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

#include "pn-ssh-profile.h"
#include "pn-profile-schema.h"
#include "pn-vault.h"

void
pn_ssh_register_profile_type (PnNodeFactory *factory)
{
    PnProfileSchema *schema;

    /* Host-key policy tokens map onto OpenSSH's StrictHostKeyChecking values
     * in the argv builder (item 47.3): accept-new → accept-new (trust on
     * first use, today's default), strict → yes (refuse unknown / changed
     * keys), off → no (accept anything; insecure escape hatch).  Stored as
     * these friendly tokens; the builder does the translation. */
    static const gchar *const host_key_policy_choices[] = {
        "accept-new", "strict", "off", NULL
    };

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    schema = pn_profile_schema_new (PN_PROFILE_SSH, "SSH Login");

    /* username + port say which account on which port; both override the
     * ambient defaults (the local user, port 22).  Leaving username blank
     * keeps today's "log in as the desktop user" behaviour. */
    pn_profile_schema_field (schema, "username", "Username", PN_FIELD_STRING);
    pn_profile_schema_field (schema, "port",     "Port",     PN_FIELD_INT);
    pn_profile_schema_field_default (schema, "port", "22");

    /* The private key to authenticate with (ssh -i).  A filesystem path the
     * host trusts, not a credential, so it is unmasked and rendered with a
     * browse button like the MQTT TLS material. */
    pn_profile_schema_field (schema, "identity-file",
                             "Identity file (private key)", PN_FIELD_FILE);

    /* Two secrets: the passphrase that unlocks the key, and an optional
     * password for password auth.  Both are masked, scrubbed on save, and
     * live only in the 0600 vault.  Whether the password is usable at all
     * under BatchMode is decided in item 47.5. */
    pn_profile_schema_field (schema, "passphrase",
                             "Key passphrase", PN_FIELD_SECRET);
    pn_profile_schema_field (schema, "password",
                             "Password", PN_FIELD_SECRET);

    /* Host-key checking policy.  Defaults to accept-new to match the flags
     * the SSH nodes hard-code today (StrictHostKeyChecking=accept-new). */
    pn_profile_schema_field (schema, "host-key-policy",
                             "Host-key policy", PN_FIELD_ENUM);
    pn_profile_schema_field_choices (schema, "host-key-policy",
                                     host_key_policy_choices);
    pn_profile_schema_field_default (schema, "host-key-policy", "accept-new");

    /* Hover hints shown on each field in the Credentials dialog (the full
     * reference lives in SshProfile.html). */
    pn_profile_schema_field_tooltip (schema, "username",
            "Remote login name (ssh -l). Leave empty to log in as the "
            "local desktop user, as the SSH nodes do today.");
    pn_profile_schema_field_tooltip (schema, "port",
            "TCP port of the remote SSH server (ssh -p). Default 22.");
    pn_profile_schema_field_tooltip (schema, "identity-file",
            "Path to the private key to authenticate with (ssh -i). "
            "Leave empty to use the agent / default keys.");
    pn_profile_schema_field_tooltip (schema, "passphrase",
            "Passphrase that unlocks the private key above. Stored in the "
            "0600 vault, never in a saved worksheet.");
    pn_profile_schema_field_tooltip (schema, "password",
            "Password for password authentication. Whether this is usable "
            "depends on the connection mode — see the help page.");
    pn_profile_schema_field_tooltip (schema, "host-key-policy",
            "How to treat the remote host key: accept-new trusts it on first "
            "sight (default); strict refuses an unknown or changed key; off "
            "disables checking entirely (insecure).");

    pn_profile_schema_set_help_page (schema, "SshProfile.html");

    pn_node_factory_register_profile_type (factory, schema);
}

/* Map an "ssh-login" host-key-policy token onto the value OpenSSH expects
 * for -o StrictHostKeyChecking=.  Anything unrecognised (or unset) falls
 * back to accept-new — the policy the SSH nodes hard-coded before this
 * type existed, so a node with no profile is unchanged. */
static const gchar *
ssh_strict_host_key_checking (const gchar *policy)
{
    if (g_strcmp0 (policy, "strict") == 0)
        return "yes";
    if (g_strcmp0 (policy, "off") == 0)
        return "no";
    return "accept-new";
}

/* ------------------------------------------------------------------ */
/*  Login snapshot (thread-safe value copy of a resolved profile)      */
/* ------------------------------------------------------------------ */

void
pn_ssh_login_resolve (PnNode      *node,
                      const gchar *ref_prop,
                      PnSshLogin  *out)
{
    PnProfile *profile;

    g_return_if_fail (PN_IS_NODE (node));
    g_return_if_fail (ref_prop != NULL);
    g_return_if_fail (out != NULL);

    /* Resolve on the caller's (main) thread — pn_node_get_profile reads a
     * GObject property and the mutable vault singleton, neither of which is
     * safe off the main thread. */
    profile = pn_node_get_profile (node, ref_prop);

    out->has_profile     = (profile != NULL);
    out->username        = profile ? pn_profile_get_string (profile, "username")
                                   : g_strdup ("");
    out->identity_file   = profile ? pn_profile_get_string (profile, "identity-file")
                                   : g_strdup ("");
    out->host_key_policy = profile ? pn_profile_get_string (profile, "host-key-policy")
                                   : g_strdup ("");
    out->port            = profile ? (gint) pn_profile_get_int (profile, "port")
                                   : 0;
}

void
pn_ssh_login_copy (PnSshLogin       *dst,
                   const PnSshLogin *src)
{
    g_return_if_fail (dst != NULL);
    g_return_if_fail (src != NULL);

    dst->has_profile     = src->has_profile;
    dst->username        = g_strdup (src->username        ? src->username        : "");
    dst->identity_file   = g_strdup (src->identity_file   ? src->identity_file   : "");
    dst->host_key_policy = g_strdup (src->host_key_policy ? src->host_key_policy : "");
    dst->port            = src->port;
}

void
pn_ssh_login_clear (PnSshLogin *login)
{
    if (login == NULL)
        return;

    g_clear_pointer (&login->username,        g_free);
    g_clear_pointer (&login->identity_file,   g_free);
    g_clear_pointer (&login->host_key_policy, g_free);
    login->has_profile = FALSE;
    login->port        = 0;
}

void
pn_ssh_login_refresh (PnNode      *node,
                      const gchar *ref_prop,
                      GMutex      *mutex,
                      PnSshLogin  *slot)
{
    PnSshLogin fresh = { 0 };

    g_return_if_fail (mutex != NULL);
    g_return_if_fail (slot != NULL);

    /* Resolve outside the lock — pn_ssh_login_resolve calls g_object_get on
     * @ref_prop, which re-enters the node's property getter; taking @mutex
     * here would deadlock a node whose getter also locks it. */
    pn_ssh_login_resolve (node, ref_prop, &fresh);

    g_mutex_lock (mutex);
    pn_ssh_login_clear (slot);
    *slot = fresh;                 /* move ownership of fresh's strings */
    g_mutex_unlock (mutex);
}

void
pn_ssh_login_dup_locked (GMutex           *mutex,
                         const PnSshLogin *slot,
                         PnSshLogin       *out)
{
    g_return_if_fail (mutex != NULL);
    g_return_if_fail (slot != NULL);
    g_return_if_fail (out != NULL);

    g_mutex_lock (mutex);
    pn_ssh_login_copy (out, slot);
    g_mutex_unlock (mutex);
}

GParamSpec *
pn_ssh_auth_profile_param_spec (void)
{
    GParamSpec *pspec = g_param_spec_string (
            "auth-profile", "Auth profile",
            "Id of the SSH Login credential profile this node logs in with. "
            "Empty follows the primary SSH Login profile (and, when none is "
            "configured, keeps the node's ambient-key behaviour); pick another "
            "to use a different username / identity / port / host-key policy. "
            "The credentials live in the host vault, not in the workflow file. "
            "The in-node host field stays separate — it says WHERE, the "
            "profile says HOW to log in.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    pn_param_spec_set_profile_ref (pspec, PN_PROFILE_SSH);
    return pspec;
}

/* ------------------------------------------------------------------ */
/*  Argv builder                                                       */
/* ------------------------------------------------------------------ */

gchar **
pn_ssh_build_argv (const gchar        *host,
                   const PnSshLogin   *login,
                   guint               connect_timeout,
                   const gchar *const *base_argv)
{
    GPtrArray   *out;
    const gchar *policy_token = "accept-new";
    const gchar *username     = NULL;
    const gchar *identity     = NULL;
    gint         port         = 0;
    guint        timeout      = (connect_timeout > 0) ? connect_timeout : 5;
    guint        i;

    if (base_argv == NULL || base_argv[0] == NULL)
        return g_new0 (gchar *, 1);

    if (login != NULL && login->has_profile)
    {
        if (login->host_key_policy != NULL && *login->host_key_policy != '\0')
            policy_token = ssh_strict_host_key_checking (login->host_key_policy);

        username = login->username;
        identity = login->identity_file;
        port     = login->port;
    }

    out = g_ptr_array_new ();
    g_ptr_array_add (out, g_strdup ("ssh"));

    /* BatchMode stays on unconditionally — no interactive prompt is ever
     * acceptable from a node running headless behind a desktop launcher.
     * A passphrase / password profile thus needs the 47.5 path; the
     * secrets are deliberately not part of #PnSshLogin. */
    g_ptr_array_add (out, g_strdup ("-o"));
    g_ptr_array_add (out, g_strdup ("BatchMode=yes"));
    g_ptr_array_add (out, g_strdup ("-o"));
    g_ptr_array_add (out, g_strdup_printf ("StrictHostKeyChecking=%s",
                                           policy_token));
    g_ptr_array_add (out, g_strdup ("-o"));
    g_ptr_array_add (out, g_strdup_printf ("ConnectTimeout=%u", timeout));

    /* Login bits, each emitted only when set so an ambient (or NULL) login
     * reproduces the bare host-key argv above. */
    if (username != NULL && *username != '\0')
    {
        g_ptr_array_add (out, g_strdup ("-l"));
        g_ptr_array_add (out, g_strdup (username));
    }
    if (port > 0 && port != 22)
    {
        g_ptr_array_add (out, g_strdup ("-p"));
        g_ptr_array_add (out, g_strdup_printf ("%d", port));
    }
    if (identity != NULL && *identity != '\0')
    {
        g_ptr_array_add (out, g_strdup ("-i"));
        g_ptr_array_add (out, g_strdup (identity));
    }

    g_ptr_array_add (out, g_strdup (host));

    for (i = 0; base_argv[i] != NULL; i++)
        g_ptr_array_add (out, g_strdup (base_argv[i]));

    g_ptr_array_add (out, NULL);

    return (gchar **) g_ptr_array_free (out, FALSE);
}
