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
/*  Pipnode sound-effects plugin                                       */
/*                                                                     */
/*  Ships the PnSciFiSound sink node — a sci-fi sound effects player   */
/*  whose clip library is not bundled with the .so but is downloaded   */
/*  on demand from a curated set of URLs the user opts into through a  */
/*  button on the node's settings dialog.  The plugin therefore        */
/*  installs only code; no audio data ever ships through this          */
/*  archive.                                                           */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-sci-fi-sound.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-sound-effects",
        .version     = "1.0.0",
        .description = "Sci-fi sound effect sinks.  Ships the "
                       "PnSciFiSound node, which plays a chosen clip "
                       "from a per-user cache directory on every "
                       "incoming message and offers a one-click "
                       "download of a classic Star Trek effects set "
                       "on the settings dialog.",
    };

    pn_node_factory_register (factory, PN_TYPE_SCI_FI_SOUND);

    return &info;
}
