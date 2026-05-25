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
/*  Pipnode network plugin                                             */
/*                                                                     */
/*  Bundled-but-out-of-process plugin: ships in the same source tree  */
/*  as pipnode itself, builds against libpipnode, and installs into   */
/*  $pkglibdir/plugins/ where the host's default plugin search path   */
/*  picks it up automatically at startup.  Demonstrates that the      */
/*  plugin ABI introduced in <pn-plugin.h> is sufficient to ship      */
/*  first-party node types separately from the core binary —         */
/*  Ping and DNS Check used to be built into libpipnode and          */
/*  registered from pn_node_factory_register_builtins(); they now    */
/*  live in this directory and reach the factory via the standard    */
/*  plugin entry point.                                               */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-dns.h"
#include "pn-https-tunnel-receiver.h"
#include "pn-https-tunnel-sender.h"
#include "pn-mqtt.h"
#include "pn-mqtt-sink.h"
#include "pn-network-profiles.h"
#include "pn-ping.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-network",
        .version     = "1.0.0",
        .description = "Bundled network nodes: Ping (ICMP echo), "
                       "DNS Check (resolver health probe), HTTPS "
                       "Tunnel Receiver and HTTPS Tunnel Sender — the "
                       "two ends of a pipnode-to-pipnode message "
                       "tunnel over HTTPS — and MQTT Source (broker "
                       "subscriber) and MQTT Sink (broker publisher).",
    };

    /* Same registration order the factory's built-in pass used to
     * have, so the palette's Network category presents the two
     * probes in the order users have learned; the HTTPS Tunnel
     * Receiver and Sender follow them as a paired receiver/sender,
     * and the MQTT Source / Sink close out the category as a paired
     * subscriber / publisher. */
    pn_node_factory_register (factory, PN_TYPE_PING);
    pn_node_factory_register (factory, PN_TYPE_DNS);
    pn_node_factory_register (factory, PN_TYPE_HTTPS_TUNNEL_RECEIVER);
    pn_node_factory_register (factory, PN_TYPE_HTTPS_TUNNEL_SENDER);
    pn_node_factory_register (factory, PN_TYPE_MQTT);
    pn_node_factory_register (factory, PN_TYPE_MQTT_SINK);

    /* Declare the credential profile types these nodes reference so the
     * host (editor or headless runner) can provision and resolve them. */
    pn_network_register_profile_types (factory);

    return &info;
}
