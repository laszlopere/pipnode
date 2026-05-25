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

/* ------------------------------------------------------------------ */
/*  vaultecho — reference plugin for host-provisioned profiles (ABI v5) */
/*                                                                     */
/*  The minimal worked example of the credentials feature (PLUGINS     */
/*  section 18): a plugin that DECLARES a profile type, a node that     */
/*  REFERENCES it and RESOLVES the values at run time.  Built noinst    */
/*  for the test suite; test_credentials_profile.py loads it under      */
/*  pipnode-run and checks the resolved values.                        */
/*                                                                     */
/*  PnVaultEcho is a pass-through filter that, for each message,        */
/*  resolves its "vaultecho" profile and stamps the greeting onto the   */
/*  envelope.  It emits the secret's LENGTH, never its value — a real   */
/*  node must not leak a credential into a message; the length is       */
/*  enough for the test to prove resolution (and the env override)      */
/*  worked.                                                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-vaultecho.h"

#include "pn-message.h"
#include "pn-node-factory.h"
#include "pn-plugin.h"
#include "pn-profile-schema.h"
#include "pn-vault.h"

#include <gmodule.h>
#include <string.h>

#define PN_VAULTECHO_PROFILE_TYPE "vaultecho"

struct _PnVaultEcho
{
    PnNode  parent_instance;
    gchar  *profile;   /* referenced profile id; "" follows the primary */
};

G_DEFINE_TYPE (PnVaultEcho, pn_vault_echo, PN_TYPE_NODE)

enum { PROP_0, PROP_PROFILE, N_PROPS };
static GParamSpec *props[N_PROPS];

static void
pn_vault_echo_receive (PnNode *node, PnMessage *message)
{
    PnProfile *p        = pn_node_get_profile (node, "profile");
    gchar     *greeting = p != NULL ? pn_profile_get_string (p, "greeting")
                                    : g_strdup ("");
    gchar     *secret   = p != NULL ? pn_profile_get_secret (p, "secret")
                                    : g_strdup ("");
    PnMessage *out      = pn_message_new (node, "vaultecho");

    (void) message;

    pn_message_set_string  (out, "greeting",   greeting);
    pn_message_set_int     (out, "secret_len", (gint64) strlen (secret));
    pn_message_set_string  (out, "output",     greeting);
    pn_message_set_boolean (out, "success",    TRUE);

    pn_node_emit_message (node, out);

    g_object_unref (out);
    g_free (greeting);
    g_free (secret);
}

static void
pn_vault_echo_get_property (GObject *object, guint id, GValue *value,
                            GParamSpec *pspec)
{
    PnVaultEcho *self = PN_VAULT_ECHO (object);
    if (id == PROP_PROFILE)
        g_value_set_string (value, self->profile);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
pn_vault_echo_set_property (GObject *object, guint id, const GValue *value,
                            GParamSpec *pspec)
{
    PnVaultEcho *self = PN_VAULT_ECHO (object);
    if (id == PROP_PROFILE)
    {
        g_free (self->profile);
        self->profile = g_value_dup_string (value);
    }
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
pn_vault_echo_finalize (GObject *object)
{
    g_free (PN_VAULT_ECHO (object)->profile);
    G_OBJECT_CLASS (pn_vault_echo_parent_class)->finalize (object);
}

static void
pn_vault_echo_class_init (PnVaultEchoClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_vault_echo_get_property;
    object_class->set_property = pn_vault_echo_set_property;
    object_class->finalize     = pn_vault_echo_finalize;

    node_class->receive    = pn_vault_echo_receive;
    node_class->class_name = "Vault Echo";
    node_class->icon       = "\xef\x84\x9e";  /* fa-key U+F084 */
    node_class->color      = (PnColor){ 0.55, 0.65, 0.95, 1.0 };
    node_class->category   = "Test";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_PROFILE] = g_param_spec_string (
            "profile", "Profile",
            "Id of the vaultecho credential profile to resolve; empty "
            "follows the type's primary.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Render the property as a profile picker rather than a text entry. */
    pn_param_spec_set_profile_ref (props[PROP_PROFILE],
                                   PN_VAULTECHO_PROFILE_TYPE);
}

static void
pn_vault_echo_init (PnVaultEcho *self)
{
    self->profile = g_strdup ("");
    pn_node_set_class_name (PN_NODE (self), "Vault Echo");
    pn_node_set_has_input  (PN_NODE (self), TRUE);
    pn_node_set_has_output (PN_NODE (self), TRUE);
}

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "vaultecho",
        .version     = "1.0.0",
        .description = "Reference plugin for host-provisioned profiles: "
                       "declares a 'vaultecho' profile type and a node "
                       "that resolves it.",
    };
    PnProfileSchema *schema;

    /* Declare the profile type this plugin's node references. */
    schema = pn_profile_schema_new (PN_VAULTECHO_PROFILE_TYPE, "Vault Echo");
    pn_profile_schema_field (schema, "greeting", "Greeting", PN_FIELD_STRING);
    pn_profile_schema_field_default (schema, "greeting", "hello");
    pn_profile_schema_field (schema, "secret", "Secret", PN_FIELD_SECRET);
    pn_node_factory_register_profile_type (factory, schema);

    pn_node_factory_register (factory, PN_TYPE_VAULT_ECHO);
    return &info;
}
