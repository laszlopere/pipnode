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
/*  Pipnode host-monitoring plugin                                     */
/*                                                                     */
/*  Bundled core-only plugin: ships in the same source tree as         */
/*  pipnode itself, builds against libpipnode-core, and installs into  */
/*  $pkglibdir/plugins/ where the host's default plugin search path    */
/*  picks it up automatically at startup.  Every node here samples the */
/*  local machine through /proc, /sys, or a remote probe and is pure   */
/*  logic — no GTK — so the whole plugin is a single GTK-free .so that */
/*  a headless server can install and run under pipnode-run.           */
/*                                                                     */
/*  System Load, Net Connections, Disk I/O, Network I/O, CPU, CPU      */
/*  Temperature, Ambient Temperature and Memory used to be built into  */
/*  libpipnode and registered from register_builtins(); they now live  */
/*  in this directory and reach the factory via the standard           */
/*  pn_plugin_init() entry point.                                      */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-ambient.h"
#include "pn-connections.h"
#include "pn-cpu.h"
#include "pn-disk-io.h"
#include "pn-load.h"
#include "pn-memory.h"
#include "pn-net-io.h"
#include "pn-temp.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-host-monitoring",
        .version     = "1.0.0",
        .description = "Bundled host-monitoring nodes: System Load, Net "
                       "Connections, Disk I/O, Network I/O, CPU, CPU "
                       "Temperature, Ambient Temperature and Memory — "
                       "local-machine probes that sample /proc, /sys, or "
                       "a remote sensor and emit a numeric reading.",
    };

    /* Same registration order the factory's built-in pass used to have,
     * so the palette's "Host monitoring" category presents the probes in
     * the order users have learned. */
    pn_node_factory_register (factory, PN_TYPE_LOAD);
    pn_node_factory_register (factory, PN_TYPE_CONNECTIONS);
    pn_node_factory_register (factory, PN_TYPE_DISK_IO);
    pn_node_factory_register (factory, PN_TYPE_NET_IO);
    pn_node_factory_register (factory, PN_TYPE_CPU);
    pn_node_factory_register (factory, PN_TYPE_TEMP);
    pn_node_factory_register (factory, PN_TYPE_AMBIENT);
    pn_node_factory_register (factory, PN_TYPE_MEMORY);

    return &info;
}
