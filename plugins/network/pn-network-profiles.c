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

#include "pn-network-profiles.h"

#include "pn-profile-schema.h"
#include "pn-vault.h"

void
pn_network_register_profile_types (PnNodeFactory *factory)
{
    PnProfileSchema *mqtt;
    PnProfileSchema *http;

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    mqtt = pn_profile_schema_new (PN_NETWORK_PROFILE_MQTT_BROKER,
                                  "MQTT Broker");
    /* No default URL: a site-specific broker address does not belong in the
     * plugin source — the user fills it in here once, as a local setting. */
    pn_profile_schema_field (mqtt, "url",       "Broker URL", PN_FIELD_STRING);
    pn_profile_schema_field (mqtt, "username",  "Username",  PN_FIELD_STRING);
    pn_profile_schema_field (mqtt, "password",  "Password",  PN_FIELD_SECRET);
    pn_profile_schema_field (mqtt, "client-id", "Client ID", PN_FIELD_STRING);
    pn_node_factory_register_profile_type (factory, mqtt);

    http = pn_profile_schema_new (PN_NETWORK_PROFILE_HTTP_BASIC,
                                  "HTTP Basic Auth");
    pn_profile_schema_field (http, "username", "Username", PN_FIELD_STRING);
    pn_profile_schema_field (http, "password", "Password", PN_FIELD_SECRET);
    pn_node_factory_register_profile_type (factory, http);
}

/* TRUE when @p already stores exactly the @n (@names,@values) pairs. */
static gboolean
profile_matches (PnProfile          *p,
                 const gchar *const *names,
                 const gchar *const *values,
                 guint               n)
{
    guint i;

    for (i = 0; i < n; i++)
    {
        gchar       *cur  = pn_profile_get_string (p, names[i]);
        const gchar *want = values[i] != NULL ? values[i] : "";
        gboolean     eq   = g_strcmp0 (cur, want) == 0;
        g_free (cur);
        if (!eq)
            return FALSE;
    }
    return TRUE;
}

gchar *
pn_network_import_profile (const gchar        *type_id,
                           const gchar        *suggested_name,
                           const gchar *const *names,
                           const gchar *const *values,
                           guint               n)
{
    PnVault   *vault = pn_vault_get_default ();
    GList     *existing, *l;
    PnProfile *p;
    guint      i;

    g_return_val_if_fail (type_id != NULL, NULL);

    /* Reuse a profile that already carries these exact values so repeated
     * opens of the same legacy file do not pile up duplicates. */
    existing = pn_vault_list_profiles (vault, type_id);
    for (l = existing; l != NULL; l = l->next)
    {
        if (profile_matches (l->data, names, values, n))
        {
            gchar *id = g_strdup (pn_profile_get_id (l->data));
            g_list_free (existing);
            return id;
        }
    }
    g_list_free (existing);

    p = pn_vault_create_profile (vault, type_id, suggested_name);
    if (p == NULL)
        return NULL;

    for (i = 0; i < n; i++)
        if (values[i] != NULL && *values[i] != '\0')
            pn_profile_set_field (p, names[i], values[i]);

    return g_strdup (pn_profile_get_id (p));
}
