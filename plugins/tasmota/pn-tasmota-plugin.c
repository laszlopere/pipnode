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
/*  Pipnode tasmota plugin                                             */
/*                                                                     */
/*  Bundled plugin: ships in the source tree alongside pipnode itself, */
/*  builds against libpipnode, and installs into $pkglibdir/plugins/   */
/*  where the host's default plugin search path picks it up at         */
/*  startup.  Provides filter/transformer nodes that pluck individual  */
/*  sensor readings out of Tasmota MQTT publishes (the JSON-shaped     */
/*  blobs PnMqtt forwards under the "tele/<device>/SENSOR" topic       */
/*  family) and reshape them into the standard data-bag                */
/*  members (data.value / data.unit / data.device) that the rest of    */
/*  the palette already speaks.                                        */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-tasmota-apparent-power.h"
#include "pn-tasmota-current.h"
#include "pn-tasmota-humidity.h"
#include "pn-tasmota-power.h"
#include "pn-tasmota-power-factor.h"
#include "pn-tasmota-reactive-power.h"
#include "pn-tasmota-status.h"
#include "pn-tasmota-relay-status.h"
#include "pn-tasmota-relay-command.h"
#include "pn-tasmota-switch.h"
#include "pn-tasmota-temperature.h"
#include "pn-tasmota-voltage.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-tasmota",
        .version     = "1.0.0",
        .description = "Filters that extract Tasmota sensor readings "
                       "(temperature, humidity) from MQTT publishes "
                       "into the standard data.value / data.unit / "
                       "data.device shape, plus a STATUS-only "
                       "topic-family pass filter.",
    };

    pn_node_factory_register (factory, PN_TYPE_TASMOTA_TEMPERATURE);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_HUMIDITY);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_STATUS);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_RELAY_STATUS);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_RELAY_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_SWITCH);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_VOLTAGE);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_CURRENT);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_POWER);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_APPARENT_POWER);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_REACTIVE_POWER);
    pn_node_factory_register (factory, PN_TYPE_TASMOTA_POWER_FACTOR);

    return &info;
}
