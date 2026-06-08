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
