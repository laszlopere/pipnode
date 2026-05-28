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

#ifndef PN_MQTT_PROFILE_H
#define PN_MQTT_PROFILE_H

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* The credential profile type the MQTT Source and Sink (and any subclass
 * of #PnMqtt — e.g. a Zigbee2MQTT bridge plugin) reference for their
 * connection identity.  Declared here, not in the network plugin, so
 * out-of-tree plugins can subclass PnMqtt and inherit the same broker
 * profile without re-registering it. */
#define PN_PROFILE_TYPE_MQTT_BROKER "mqtt-broker"

/**
 * pn_mqtt_register_profile_type:
 * @factory: the process-wide factory
 *
 * Registers the "mqtt-broker" profile type with @factory.  Called once
 * from the host's built-in registration pass — the type is part of core
 * because the MQTT base class lives in core.
 */
void pn_mqtt_register_profile_type (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_MQTT_PROFILE_H */
