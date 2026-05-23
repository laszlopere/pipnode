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
#include "pn-gui.h"
#include "pn-preferences.h"

/* Editor plugin-load policy: skip plugins the user disabled in
 * Preferences.  Installed on the factory before the discovery scan so the
 * GTK-free core never has to reach into the GTK-bound PnPreferences itself
 * (TODO #23, Phase 5).  pipnode-run installs no filter, so it loads every
 * discovered plugin. */
static gboolean
editor_plugin_disabled (const gchar *basename, gpointer user_data)
{
    (void) user_data;
    return pn_preferences_is_plugin_disabled (pn_preferences_get_default (),
                                              basename);
}

/* Plugin discovery has to run before g_application_run() (the first
 * window's palette needs the plugin node types), which is ahead of the
 * GApplication option parser.  So --no-plugins is detected by a small
 * manual scan of argv here, rather than read back from the parsed
 * options.  It is still registered as a real GOptionEntry in
 * pn_application_new() so it shows up in --help and is not rejected as
 * unknown.  Stop at "--": everything past the end-of-options marker is
 * positional and must not be treated as the flag. */
static gboolean
argv_has_no_plugins (int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (g_strcmp0 (argv[i], "--") == 0)
            break;
        if (g_strcmp0 (argv[i], "--no-plugins") == 0)
            return TRUE;
    }
    return FALSE;
}

int
main (
        int    argc,
        char **argv)
{
    PnApplication *app;
    PnNodeFactory *factory;
    gboolean       no_plugins;
    int status;

    /* --no-plugins lets a session start with the built-in nodes only —
     * handy for isolating whether a problem comes from a plugin, or for
     * a clean editor when a stale/broken plugin would otherwise hang the
     * dlopen scan. */
    no_plugins = argv_has_no_plugins (argc, argv);

    /* Build the singleton (which lazily registers the built-in node
     * types), install the preferences-backed plugin filter, and then
     * auto-discover plugins from PIPNODE_PLUGIN_PATH, the per-user data
     * directory, and the system pkglibdir.  Done before
     * g_application_run() so the palette construction in PnWindow already
     * sees the plugin types when the first window is built.  Skipped
     * entirely under --no-plugins. */
    factory = pn_node_factory_get_default ();
    if (!no_plugins)
    {
        pn_node_factory_set_plugin_filter (factory, editor_plugin_disabled,
                                           NULL, NULL);
        pn_node_factory_load_plugins_default (factory);
    }

    /* Editor only: install the gui-tier vfunc slots (cairo painters +
     * settings dialogs) onto the dual-nature node classes whose logic
     * half lives in the headless core (TODO #23, Phase 4).  pipnode-run
     * never calls this, so those nodes run without GTK.  These are
     * built-ins, so they are installed regardless of --no-plugins. */
    pn_gui_install_builtin_nodes ();

    /* Editor only: for each plugin whose logic .so just loaded, load the
     * sibling companion GUI module (<base>-gui.so) if present and let it
     * install the plugin node's painter/dialog vfuncs — the per-plugin
     * analogue of the built-in installs above (TODO #23, Phase 6).
     * pipnode-run never calls this, so a server-installed plugin pulls no
     * GTK.  Nothing to companion when plugins were not loaded. */
    if (!no_plugins)
        pn_gui_load_plugin_companions (factory);

    app = pn_application_new ();
    status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    return status;
}
