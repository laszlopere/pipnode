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

#include "pn-mqtt-profile.h"
#include "pn-profile-schema.h"

void
pn_mqtt_register_profile_type (PnNodeFactory *factory)
{
    PnProfileSchema *schema;

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    schema = pn_profile_schema_new (PN_PROFILE_TYPE_MQTT_BROKER,
                                    "MQTT Broker");
    /* No default URL: a site-specific broker address does not belong in the
     * source — the user fills it in here once, as a local setting. */
    pn_profile_schema_field (schema, "url",       "Broker URL", PN_FIELD_STRING);
    pn_profile_schema_field (schema, "username",  "Username",  PN_FIELD_STRING);
    pn_profile_schema_field (schema, "password",  "Password",  PN_FIELD_SECRET);
    pn_profile_schema_field (schema, "client-id", "Client ID", PN_FIELD_STRING);

    /* TLS trust material (review M3).  These are filesystem paths the host
     * trusts, not credentials, so they are plain strings (unmasked, stored in
     * the workflow-free vault like the rest), not secrets.  A CA bundle lets a
     * private / self-signed broker verify; the cert+key pair enables mutual
     * TLS; the insecure toggle is a deliberate, logged dev-only escape hatch.
     * All are only consulted when the URL scheme selects TLS (ssl:// / mqtts://). */
    pn_profile_schema_field (schema, "ca-file",      "CA certificate file (TLS)",      PN_FIELD_STRING);
    pn_profile_schema_field (schema, "client-cert",  "Client certificate (mutual TLS)", PN_FIELD_STRING);
    pn_profile_schema_field (schema, "client-key",   "Client key (mutual TLS)",         PN_FIELD_STRING);
    pn_profile_schema_field (schema, "tls-insecure", "Skip TLS verification (insecure)", PN_FIELD_BOOL);

    /* Keep-alive seconds: 0/blank → the built-in default (60). */
    pn_profile_schema_field (schema, "keepalive", "Keep-alive (seconds)", PN_FIELD_INT);
    pn_profile_schema_field_default (schema, "keepalive", "60");

    pn_profile_schema_set_help_page (schema, "MqttBrokerProfile.html");
    pn_node_factory_register_profile_type (factory, schema);
}
