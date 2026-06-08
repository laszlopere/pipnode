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

void
pn_network_register_profile_types (PnNodeFactory *factory)
{
    PnProfileSchema *http;

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    http = pn_profile_schema_new (PN_NETWORK_PROFILE_HTTP_BASIC,
                                  "Web Service Login");
    pn_profile_schema_field (http, "username", "Username", PN_FIELD_STRING);
    pn_profile_schema_field (http, "password", "Password", PN_FIELD_SECRET);
    pn_profile_schema_set_help_page (http, "HttpBasicAuthProfile.html");
    pn_node_factory_register_profile_type (factory, http);
}
