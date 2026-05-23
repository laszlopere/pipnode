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

#include "pn-gui.h"
#include "pn-plugin.h"

#include <gmodule.h>
#include <string.h>

#include "pn-analog-meter-gui.h"
#include "pn-chat-gui.h"
#include "pn-dial-gui.h"
#include "pn-file-viewer-gui.h"
#include "pn-filedrop-gui.h"
#include "pn-filter-gui.h"
#include "pn-graph-gui.h"
#include "pn-knob-gui.h"
#include "pn-led-gui.h"
#include "pn-meshtastic-gui.h"
#include "pn-notify-gui.h"
#include "pn-rate-gui.h"
#include "pn-set-gui.h"
#include "pn-sound-gui.h"
#include "pn-switch-gui.h"
#include "pn-table-gui.h"
#include "pn-table-view-gui.h"
#include "pn-text-view-gui.h"
#include "pn-tts-gui.h"
#include "pn-weather-report-gui.h"

void
pn_gui_install_builtin_nodes (void)
{
    /* Dual-nature built-ins whose drawing + dialog have been split out
     * to the gui tier (TODO #23, Phase 4).  Each entry installs the
     * gui-only vfunc slots onto the core-registered class. */
    pn_analog_meter_gui_install ();
    pn_chat_gui_install ();
    pn_dial_gui_install ();
    pn_file_viewer_gui_install ();
    pn_filedrop_gui_install ();
    pn_filter_gui_install ();
    pn_graph_gui_install ();
    pn_knob_gui_install ();
    pn_led_gui_install ();
    pn_meshtastic_gui_install ();
    pn_notify_gui_install ();
    pn_rate_gui_install ();
    pn_set_gui_install ();
    pn_sound_gui_install ();
    pn_switch_gui_install ();
    pn_table_gui_install ();
    pn_table_view_gui_install ();
    pn_text_view_gui_install ();
    pn_tts_gui_install ();
    pn_weather_report_gui_install ();
}

/* ------------------------------------------------------------------ */
/*  Plugin GUI companion loader (TODO #23, Phase 6)                     */
/*                                                                     */
/*  The per-plugin analogue of the built-in installs above: where the  */
/*  in-tree split nodes have their pn_<node>_gui_install() called from  */
/*  pn_gui_install_builtin_nodes(), an out-of-tree plugin ships its     */
/*  drawing/dialog code in a sibling companion module that exports      */
/*  pn_plugin_gui_init().  The editor loads the logic .so via the core  */
/*  factory, then calls pn_gui_load_plugin_companions() to load each    */
/*  companion; pipnode-run links neither this file nor the gui tier, so */
/*  a headless run never opens a companion.                            */
/* ------------------------------------------------------------------ */

/** Derive the companion path "<dir>/<base>-gui.<suffix>" from a loaded
 *  logic plugin path "<dir>/<base>.<suffix>".  Returns %NULL if @logic_path
 *  does not end in the platform module suffix (it always does — the loader
 *  only records such files — but we stay defensive).  Caller frees. */
static gchar *
companion_path_for (const gchar *logic_path)
{
    const gchar *mod_suffix = "." G_MODULE_SUFFIX;
    gsize        slen       = strlen (mod_suffix);
    gsize        plen       = strlen (logic_path);
    gchar       *stem;
    gchar       *companion;

    if (plen <= slen || !g_str_has_suffix (logic_path, mod_suffix))
        return NULL;

    stem      = g_strndup (logic_path, plen - slen);   /* drop ".<suffix>" */
    companion = g_strconcat (stem, PN_PLUGIN_GUI_INFIX, mod_suffix, NULL);
    g_free (stem);
    return companion;
}

/** dlopen @gui_path, look up pn_plugin_gui_init, run it, ABI-check the
 *  returned descriptor and pin the module resident.  Warns (does not
 *  abort) on every failure mode — a broken companion must not take down
 *  the whole editor, the node simply falls back to the host defaults. */
static void
load_companion (PnNodeFactory *factory, const gchar *gui_path)
{
    GModule             *module;
    gpointer             symbol = NULL;
    PnPluginGuiInitFunc  gui_init;
    const PnPluginInfo  *info;

    module = g_module_open (gui_path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (module == NULL)
    {
        g_warning ("pipnode: failed to open plugin GUI companion \"%s\": %s",
                   gui_path, g_module_error ());
        return;
    }

    if (!g_module_symbol (module, PN_PLUGIN_GUI_INIT_SYMBOL, &symbol)
        || symbol == NULL)
    {
        g_warning ("pipnode: plugin GUI companion \"%s\" does not export %s",
                   gui_path, PN_PLUGIN_GUI_INIT_SYMBOL);
        g_module_close (module);
        return;
    }

    gui_init = (PnPluginGuiInitFunc) symbol;
    info     = gui_init (factory);

    if (info == NULL)
    {
        g_warning ("pipnode: plugin GUI companion \"%s\": %s returned NULL",
                   gui_path, PN_PLUGIN_GUI_INIT_SYMBOL);
        g_module_close (module);
        return;
    }

    if (info->abi_version != PN_PLUGIN_ABI_VERSION)
    {
        g_warning ("pipnode: plugin GUI companion \"%s\": ABI version %d does "
                   "not match host ABI %d",
                   gui_path, info->abi_version, PN_PLUGIN_ABI_VERSION);
        g_module_close (module);
        return;
    }

    /* Pin resident — the vfunc slots the companion just wrote point into
     * its .text, and that painter/dialog code must outlive every node it
     * draws (same rationale as the logic .so; see pn-plugin.h). */
    g_module_make_resident (module);

    g_message ("pipnode: loaded plugin GUI companion \"%s\" %s from %s",
               info->name != NULL ? info->name : "(unnamed)",
               info->version != NULL ? info->version : "",
               gui_path);
}

void
pn_gui_load_plugin_companions (PnNodeFactory *factory)
{
    /* Companion paths already loaded this process, so a second call (or a
     * plugin reachable through two search dirs) installs each exactly
     * once.  Lives for the process lifetime, like the resident modules
     * it tracks. */
    static GHashTable *loaded = NULL;
    GList *paths, *l;

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    if (!g_module_supported ())
        return;

    if (loaded == NULL)
        loaded = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    paths = pn_node_factory_get_loaded_plugin_paths (factory);
    for (l = paths; l != NULL; l = l->next)
    {
        gchar *gui_path = companion_path_for ((const gchar *) l->data);

        if (gui_path == NULL)
            continue;

        /* No companion next to this logic plugin: not an error — the
         * node renders with the host default painter and dialog. */
        if (!g_file_test (gui_path, G_FILE_TEST_IS_REGULAR)
            || g_hash_table_contains (loaded, gui_path))
        {
            g_free (gui_path);
            continue;
        }

        load_companion (factory, gui_path);
        g_hash_table_add (loaded, gui_path);   /* takes ownership */
    }
    g_list_free_full (paths, g_free);
}
