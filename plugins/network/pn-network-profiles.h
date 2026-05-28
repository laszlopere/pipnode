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

#ifndef PN_NETWORK_PROFILES_H
#define PN_NETWORK_PROFILES_H

#include <glib.h>

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* The host-provisioned profile types this plugin's nodes reference.
 * Currently just "http-basic" (shared by the HTTPS Tunnel Sender and
 * Receiver).  The "mqtt-broker" schema lives in core alongside the
 * MQTT Source / Sink base class, registered through
 * pn_mqtt_register_profile_type(). */
#define PN_NETWORK_PROFILE_HTTP_BASIC  "http-basic"

/**
 * pn_network_register_profile_types:
 * @factory: the process-wide factory
 *
 * Registers the "http-basic" profile type.  Called once from the plugin's
 * pn_plugin_init().
 */
void pn_network_register_profile_types (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_NETWORK_PROFILES_H */
