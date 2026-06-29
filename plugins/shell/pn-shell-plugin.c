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
/*  Pipnode shell plugin                                               */
/*                                                                     */
/*  Bundled-but-out-of-process plugin shipping node types that drive   */
/*  the local shell.  Exports PnShellCommand (a periodic one-line      */
/*  shell command runner), PnFreeCommand (a periodic `free` runner    */
/*  that emits its output pre-parsed into PnTableModel's data.table    */
/*  shape, with a per-cell `name` for symbolic addressing) and        */
/*  PnDfCommand (the same convenience for `df`, using the two-pass    */
/*  column detector PnTableModel ships) and PnLxcLsCommand (the same  */
/*  convenience for `sudo lxc-ls -f`); the plugin is structured so    */
/*  that future shell-adjacent node types can be added under the      */
/*  same Shell palette category without spawning a separate plugin    */
/*  .so.                                                              */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-shell-command.h"
#include "pn-shell-script.h"
#include "pn-free-command.h"
#include "pn-df-command.h"
#include "pn-lxc-ls-command.h"
#include "pn-tmux-monitor.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-shell",
        .version     = "1.6.0",
        .description = "Bundled shell nodes: Shell Command (periodic "
                       "one-line shell command runner), Shell Script "
                       "(the same for a multi-line script, edited in a "
                       "GtkSourceView code editor), Free Command "
                       "(periodic `free` runner emitting a parsed table), "
                       "Df Command (the same for `df`), Lxc Ls Command "
                       "(the same for `sudo lxc-ls -f`) and Tmux Monitor "
                       "(periodic `tmux capture-pane` against a named "
                       "session, with a live session combobox in the "
                       "settings dialog).  Each node carries a `host` "
                       "property defaulting to the empty string (the "
                       "local machine, shown as a grey hint); setting "
                       "it to a remote host routes the command through "
                       "passwordless ssh.",
    };

    pn_node_factory_register (factory, PN_TYPE_SHELL_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_SHELL_SCRIPT);
    pn_node_factory_register (factory, PN_TYPE_FREE_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_DF_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_LXC_LS_COMMAND);
    pn_node_factory_register (factory, PN_TYPE_TMUX_MONITOR);

    return &info;
}
