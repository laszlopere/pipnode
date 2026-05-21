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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-application.h"
#include "pn-node-factory.h"

int
main (
        int    argc,
        char **argv)
{
    PnApplication *app;
    int status;

    /* Build the singleton (which lazily registers the built-in node
     * types) and then auto-discover plugins from PIPNODE_PLUGIN_PATH,
     * the per-user data directory, and the system pkglibdir.  Done
     * before g_application_run() so the palette construction in
     * PnWindow already sees the plugin types when the first window
     * is built. */
    pn_node_factory_load_plugins_default (pn_node_factory_get_default ());

    app = pn_application_new ();
    status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    return status;
}
