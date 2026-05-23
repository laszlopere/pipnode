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
/*  Pipnode Ollama plugin — logic half                                 */
/*                                                                     */
/*  Bundled two-tier plugin: ships in the same source tree as pipnode  */
/*  itself and installs into $pkglibdir/plugins/ where the host's      */
/*  default search path picks it up at startup.  This logic module is  */
/*  GTK-free — it submits data.output to an Ollama server's            */
/*  /api/generate over libsoup and emits the reply — so a headless     */
/*  server installs it and runs it under pipnode-run with no GTK.      */
/*                                                                     */
/*  The settings dialog's model picker (a /api/tags combo with a       */
/*  Refresh button) needs GTK, so it ships in the companion module     */
/*  pipnode_ollama-gui.so, which only the editor loads (via            */
/*  pn_plugin_gui_init).  The Ollama node used to be built into        */
/*  libpipnode and registered from register_builtins(); it now lives   */
/*  here and reaches the factory via the standard pn_plugin_init().    */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-ollama.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-ollama",
        .version     = "1.0.0",
        .description = "Ollama node: submits a message's data.output to a "
                       "local or remote Ollama server's /api/generate "
                       "endpoint and emits the model's reply.",
    };

    pn_node_factory_register (factory, PN_TYPE_OLLAMA);

    return &info;
}
