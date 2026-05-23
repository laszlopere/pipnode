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

#ifndef PN_GUI_H
#define PN_GUI_H

#include <glib.h>

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* pn_gui_install_builtin_nodes:
 *
 * Install the gui-tier vfunc slots (cairo painters + settings-dialog
 * customisations) onto the built-in dual-nature node classes whose
 * logic half lives in libpipnode-core.  The editor calls this once at
 * startup, after the factory has registered the built-ins; the headless
 * runtime (pipnode-run) never does, so those nodes load and run without
 * GTK.
 *
 * As each dual node is split (TODO #23, Phase 4) its
 * pn_<node>_gui_install() is added to the body of this function.  This
 * is the in-tree counterpart of the per-plugin pn_plugin_gui_init()
 * companion entry point introduced in Phase 6.  */
void pn_gui_install_builtin_nodes (void);

/* pn_gui_load_plugin_companions:
 * @factory: the process-wide factory whose logic plugins have already
 *           been loaded (via pn_node_factory_load_plugins_default()).
 *
 * For every plugin .so the factory loaded this session, look for the
 * sibling companion GUI module (<base>-gui.<suffix>, see
 * PN_PLUGIN_GUI_INFIX) next to it, dlopen it, and call its
 * pn_plugin_gui_init() so it installs the cairo painter / settings-
 * dialog vfunc slots onto the classes the logic .so registered.  A
 * missing companion is not an error.  Call once at editor startup,
 * after pn_node_factory_load_plugins_default(); the headless runtime
 * (pipnode-run) never calls this, so a server-installed plugin pulls no
 * GTK (TODO #23, Phase 6).  This is the per-plugin counterpart of
 * pn_gui_install_builtin_nodes().  */
void pn_gui_load_plugin_companions (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_GUI_H */
