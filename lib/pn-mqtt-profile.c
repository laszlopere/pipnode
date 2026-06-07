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

    /* Hover hints shown on each field's label + editor in the Credentials
     * dialog (the full reference lives in MqttBrokerProfile.html). */
    pn_profile_schema_field_tooltip (schema, "url",
            "Broker address: tcp://host[:port] (plain, default 1883), "
            "ssl:// or mqtts://host[:port] (TLS, default 8883), or "
            "ws:// / wss:// for WebSockets.");
    pn_profile_schema_field_tooltip (schema, "username",
            "Broker login name. Leave empty for an anonymous broker.");
    pn_profile_schema_field_tooltip (schema, "password",
            "Password for the username. Stored in the host vault (mode 0600), "
            "never written into a worksheet file.");
    pn_profile_schema_field_tooltip (schema, "client-id",
            "Optional stable MQTT client identifier for this machine "
            "(e.g. pipnode-livingroom). Blank → a random id each connect.");
    pn_profile_schema_field_tooltip (schema, "ca-file",
            "Optional. Absolute path to the PEM CA bundle that signed the "
            "broker's certificate. Needed only for a private / self-signed "
            "CA; blank verifies against the system trust store. TLS URLs only.");
    pn_profile_schema_field_tooltip (schema, "client-cert",
            "Optional. Path to this client's certificate for mutual TLS. "
            "Must be paired with the client key.");
    pn_profile_schema_field_tooltip (schema, "client-key",
            "Optional. Path to the private key for the client certificate "
            "(mutual TLS).");
    pn_profile_schema_field_tooltip (schema, "tls-insecure",
            "Accept the broker's certificate even if it does not verify or "
            "match the hostname. Insecure — local development only.");
    pn_profile_schema_field_tooltip (schema, "keepalive",
            "Seconds between pings to an idle broker. Blank or 0 → the default "
            "(60); maximum 65535.");

    pn_profile_schema_set_help_page (schema, "MqttBrokerProfile.html");
    pn_node_factory_register_profile_type (factory, schema);
}
