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
    pn_profile_schema_set_help_page (schema, "MqttBrokerProfile.html");
    pn_node_factory_register_profile_type (factory, schema);
}
