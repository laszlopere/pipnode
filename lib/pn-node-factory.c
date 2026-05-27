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

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include <gmodule.h>
#include <string.h>

#include "pn-analog-meter.h"
#include "pn-auto-injector.h"
#include "pn-auto-random.h"
#include "pn-chat.h"
#include "pn-comparator.h"
#include "pn-countdown.h"
#include "pn-digital-clock.h"
#include "pn-deadline.h"
#include "pn-debug.h"
#include "pn-dedup.h"
#include "pn-delay.h"
#include "pn-dial.h"
#include "pn-edge.h"
#include "pn-expression.h"
#include "pn-expression2.h"
#include "pn-failure.h"
#include "pn-file-viewer.h"
#include "pn-filedrop.h"
#include "pn-filter.h"
#include "pn-format.h"
#include "pn-graph.h"
#include "pn-http.h"
#include "pn-inject.h"
#include "pn-knob.h"
#include "pn-label.h"
#include "pn-led.h"
#include "pn-meshtastic.h"
#include "pn-notify.h"
#include "pn-numeric.h"
#include "pn-panel-display.h"
#include "pn-panel-input.h"
#include "pn-query.h"
#include "pn-rate.h"
#include "pn-rewrite.h"
#include "pn-rtc.h"
#include "pn-matrix57.h"
#include "pn-segment16.h"
#include "pn-set.h"
#include "pn-sound.h"
#include "pn-staircase.h"
#include "pn-stats.h"
#include "pn-success.h"
#include "pn-switch.h"
#include "pn-table.h"
#include "pn-table-model.h"
#include "pn-table-view.h"
#include "pn-text-view.h"
#include "pn-threshold.h"
#include "pn-throttle.h"
#include "pn-tts.h"
#include "pn-watchdog.h"
#include "pn-weather.h"
#include "pn-weather-report.h"

/* ------------------------------------------------------------------ */
/*  Registry entry                                                     */
/*                                                                     */
/*  Holds the #GType plus a class ref so the metadata pinned on the    */
/*  #PnNodeClass struct (category, icon, label, …) is reachable        */
/*  without re-acquiring a class ref at every lookup.  The factory     */
/*  itself never releases the ref — built-in node classes live for     */
/*  the lifetime of the process and would-be plugin classes need to    */
/*  outlive any #PnNode instance the user might still have on screen.  */
/* ------------------------------------------------------------------ */

typedef struct
{
    GType        type;
    PnNodeClass *klass;
} Entry;

struct _PnNodeFactory
{
    GObject parent_instance;

    /* Registration order matters — the palette iterates this array
     * front to back and the resulting visual order matches.  GArray
     * preserves order and gives us O(1) indexed access; the registry
     * is small (a few dozen entries even with plugins) so the linear
     * scans in lookup() / has_type() are fine. */
    GArray *entries; /* of Entry */

    /* Registered host-provisioned profile types (plugin ABI v5), in
     * registration order.  Each element is a PnProfileSchema* the factory
     * owns a ref on; like node classes they live for the whole process. */
    GPtrArray *profile_types; /* of PnProfileSchema* */

    /* Set of canonical paths for every plugin .so that has been
     * successfully loaded, so a plugin discovered via two overlapping
     * search directories (e.g. both $PIPNODE_PLUGIN_PATH and the
     * system pkglibdir) is loaded — and its types registered — only
     * once.  Keyed by the realpath() of the file we passed to
     * g_module_open. */
    GHashTable *loaded_paths;

    /* Parallel set of basenames for the same plugins.  Two .so files
     * with the same filename but different paths — e.g. the
     * installed `/usr/local/lib/pipnode/plugins/pipnode_network.so`
     * and the build-tree
     * `<srcdir>/plugins/network/.libs/pipnode_network.so` —
     * register the same #GType set, and a second registration would
     * crash with "cannot register existing type".  Path dedup alone
     * cannot catch this because the two paths are physically
     * different files.  Basename matches are the cheap, robust
     * heuristic: by convention every distribution ships its plugins
     * under a stable filename, so a plugin already loaded under
     * `pipnode_network.so` should not be re-loaded from a second
     * directory. */
    GHashTable *loaded_basenames;

    /* Map of basename -> const PnPluginInfo* for plugins that have
     * been successfully loaded this session.  Used by the
     * "Plugins" page of the Preferences dialog to label entries
     * with the plugin's friendly name / version / description; the
     * pointer is borrowed from the plugin's static const PnPluginInfo
     * (lifetime = process, since the .so is made resident). */
    GHashTable *loaded_plugins;

    /* Optional plugin-load policy hook (TODO #23, Phase 5).  When set,
     * load_plugin() consults it by basename and skips plugins it rejects.
     * The editor installs one backed by PnPreferences; the headless
     * runner leaves it NULL, so every discovered plugin loads. */
    PnPluginFilterFunc plugin_filter;
    gpointer           plugin_filter_data;
    GDestroyNotify     plugin_filter_destroy;
};

G_DEFINE_TYPE (PnNodeFactory, pn_node_factory, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static const Entry *
factory_find (PnNodeFactory *self, GType type)
{
    guint i;

    for (i = 0; i < self->entries->len; i++)
    {
        const Entry *e = &g_array_index (self->entries, Entry, i);
        if (e->type == type)
            return e;
    }
    return NULL;
}

/** Resolve the registered class struct, or %NULL when @type is not in
 *  the registry.  Centralised so every metadata accessor checks the
 *  same way and warns once on lookup failure rather than crashing in
 *  the caller. */
static PnNodeClass *
factory_klass (PnNodeFactory *self, GType type)
{
    const Entry *e;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);

    e = factory_find (self, type);
    if (e == NULL)
    {
        g_warning ("pn-node-factory: type \"%s\" is not registered",
                   g_type_name (type));
        return NULL;
    }
    return e->klass;
}

/* ------------------------------------------------------------------ */
/*  Built-in registration                                              */
/*                                                                     */
/*  The order below is the order the palette displays.  Categories     */
/*  appear in the order they are first introduced (Sources, Network,   */
/*  Filters, Sinks); within each category, types appear in            */
/*  the order they are registered.  Adding a new built-in type is a    */
/*  single line here — its category and label come from the type's own */
/*  #PnNodeClass.  Plugins later add themselves the same way through   */
/*  the public pn_node_factory_register() entry point.                 */
/* ------------------------------------------------------------------ */

static void
register_builtins (PnNodeFactory *self)
{
    /* Sources. */
    pn_node_factory_register (self, PN_TYPE_RTC);
    pn_node_factory_register (self, PN_TYPE_INJECT);
    pn_node_factory_register (self, PN_TYPE_FILEDROP);
    pn_node_factory_register (self, PN_TYPE_SWITCH);
    pn_node_factory_register (self, PN_TYPE_KNOB);
    pn_node_factory_register (self, PN_TYPE_PANEL_INPUT);
    pn_node_factory_register (self, PN_TYPE_AUTO_INJECTOR);
    pn_node_factory_register (self, PN_TYPE_AUTO_RANDOM);

    /* Host monitoring.  System Load, Net Connections, Disk I/O,
     * Network I/O, CPU, CPU Temperature, Ambient Temperature and
     * Memory have moved out of the host binary into the bundled
     * "pipnode-host-monitoring" plugin under plugins/host-monitoring —
     * they are still registered with the same factory through the
     * standard pn_plugin_init() entry point, just from a separately-
     * built .so the host loads at startup via
     * pn_node_factory_load_plugins_default(). */

    /* Network.  Ping and DNS Check have moved out of the host
     * binary into the bundled "pipnode-network" plugin under
     * plugins/network — they are still registered with the same
     * factory through the standard pn_plugin_init() entry point,
     * just from a separately-built .so the host loads at startup
     * via pn_node_factory_load_plugins_default().  Keeping HTTP and
     * Meshtastic here for now since they are not yet relocated. */
    pn_node_factory_register (self, PN_TYPE_HTTP);
    pn_node_factory_register (self, PN_TYPE_MESHTASTIC);
    pn_node_factory_register (self, PN_TYPE_WEATHER);

    /* Filters. */
    pn_node_factory_register (self, PN_TYPE_COMPARATOR);
    pn_node_factory_register (self, PN_TYPE_DEDUP);
    pn_node_factory_register (self, PN_TYPE_DELAY);
    pn_node_factory_register (self, PN_TYPE_EDGE);
    pn_node_factory_register (self, PN_TYPE_EXPRESSION);
    pn_node_factory_register (self, PN_TYPE_EXPRESSION2);
    pn_node_factory_register (self, PN_TYPE_SUCCESS);
    pn_node_factory_register (self, PN_TYPE_FAILURE);
    pn_node_factory_register (self, PN_TYPE_FILTER);
    pn_node_factory_register (self, PN_TYPE_FORMAT);
    pn_node_factory_register (self, PN_TYPE_QUERY);
    pn_node_factory_register (self, PN_TYPE_RATE);
    pn_node_factory_register (self, PN_TYPE_REWRITE);
    pn_node_factory_register (self, PN_TYPE_SET);
    pn_node_factory_register (self, PN_TYPE_STAIRCASE);
    pn_node_factory_register (self, PN_TYPE_STATS);
    pn_node_factory_register (self, PN_TYPE_TABLE_MODEL);
    pn_node_factory_register (self, PN_TYPE_DEADLINE);
    pn_node_factory_register (self, PN_TYPE_THRESHOLD);
    pn_node_factory_register (self, PN_TYPE_THROTTLE);
    pn_node_factory_register (self, PN_TYPE_WATCHDOG);

    /* Ollama (the LLM filter) has moved out of the host binary into the
     * bundled "pipnode-ollama" plugin under plugins/ollama — a two-tier
     * plugin whose GTK-free logic .so registers the node through the
     * standard pn_plugin_init() entry point at startup, with a companion
     * -gui.so the editor loads for the model-picker dialog. */

    /* Sinks. */
    pn_node_factory_register (self, PN_TYPE_DEBUG);
    pn_node_factory_register (self, PN_TYPE_GRAPH);
    pn_node_factory_register (self, PN_TYPE_WEATHER_REPORT);
    pn_node_factory_register (self, PN_TYPE_DIAL);
    pn_node_factory_register (self, PN_TYPE_ANALOG_METER);
    pn_node_factory_register (self, PN_TYPE_LED);
    pn_node_factory_register (self, PN_TYPE_COUNTDOWN);
    pn_node_factory_register (self, PN_TYPE_NUMERIC);
    pn_node_factory_register (self, PN_TYPE_SEGMENT16);
    pn_node_factory_register (self, PN_TYPE_MATRIX57);
    pn_node_factory_register (self, PN_TYPE_DIGITAL_CLOCK);
    pn_node_factory_register (self, PN_TYPE_LABEL);
    pn_node_factory_register (self, PN_TYPE_PANEL_DISPLAY);
    pn_node_factory_register (self, PN_TYPE_TABLE);
    pn_node_factory_register (self, PN_TYPE_TABLE_VIEW);
    pn_node_factory_register (self, PN_TYPE_TEXT_VIEW);
    pn_node_factory_register (self, PN_TYPE_FILE_VIEWER);
    pn_node_factory_register (self, PN_TYPE_CHAT);
    pn_node_factory_register (self, PN_TYPE_SOUND);
    pn_node_factory_register (self, PN_TYPE_TTS);
    pn_node_factory_register (self, PN_TYPE_NOTIFY);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_node_factory_finalize (GObject *object)
{
    PnNodeFactory *self = PN_NODE_FACTORY (object);
    guint          i;

    /* Class refs we acquired in pn_node_factory_register().  In
     * practice this finalizer never runs — the singleton lives until
     * process exit — but freeing here keeps leak-checkers quiet for
     * the test rigs that re-init GObject. */
    for (i = 0; i < self->entries->len; i++)
    {
        Entry *e = &g_array_index (self->entries, Entry, i);
        if (e->klass != NULL)
            g_type_class_unref (e->klass);
    }
    g_array_unref (self->entries);
    g_clear_pointer (&self->profile_types,    g_ptr_array_unref);
    g_clear_pointer (&self->loaded_paths,     g_hash_table_unref);
    g_clear_pointer (&self->loaded_basenames, g_hash_table_unref);
    g_clear_pointer (&self->loaded_plugins,   g_hash_table_unref);

    if (self->plugin_filter_destroy != NULL && self->plugin_filter_data != NULL)
        self->plugin_filter_destroy (self->plugin_filter_data);

    G_OBJECT_CLASS (pn_node_factory_parent_class)->finalize (object);
}

static void
pn_node_factory_class_init (PnNodeFactoryClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = pn_node_factory_finalize;
}

static void
pn_node_factory_init (PnNodeFactory *self)
{
    self->entries          = g_array_new (FALSE, FALSE, sizeof (Entry));
    self->profile_types    = g_ptr_array_new_with_free_func (
                                     (GDestroyNotify) pn_profile_schema_unref);
    self->loaded_paths     = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, NULL);
    self->loaded_basenames = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, NULL);
    self->loaded_plugins   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNodeFactory *
pn_node_factory_get_default (void)
{
    static PnNodeFactory *singleton = NULL;

    /* Single-threaded GTK app — no locking needed.  The first caller
     * (typically the main window's palette construction) populates
     * the registry; subsequent callers reuse the same instance. */
    if (singleton == NULL)
    {
        singleton = g_object_new (PN_TYPE_NODE_FACTORY, NULL);
        register_builtins (singleton);
    }
    return singleton;
}

void
pn_node_factory_set_plugin_filter (PnNodeFactory      *self,
                                   PnPluginFilterFunc  filter,
                                   gpointer            user_data,
                                   GDestroyNotify      user_data_destroy)
{
    g_return_if_fail (PN_IS_NODE_FACTORY (self));

    if (self->plugin_filter_destroy != NULL && self->plugin_filter_data != NULL)
        self->plugin_filter_destroy (self->plugin_filter_data);

    self->plugin_filter         = filter;
    self->plugin_filter_data    = user_data;
    self->plugin_filter_destroy = user_data_destroy;
}

gboolean
pn_node_factory_register (PnNodeFactory *self, GType type)
{
    Entry e;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), FALSE);

    if (!g_type_is_a (type, PN_TYPE_NODE))
    {
        g_warning ("pn-node-factory: refusing to register \"%s\" — "
                   "not a PnNode descendant",
                   g_type_name (type));
        return FALSE;
    }

    if (factory_find (self, type) != NULL)
        return FALSE;

    e.type  = type;
    e.klass = g_type_class_ref (type);
    g_array_append_val (self->entries, e);
    return TRUE;
}

gboolean
pn_node_factory_has_type (PnNodeFactory *self, GType type)
{
    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), FALSE);
    return factory_find (self, type) != NULL;
}

/* ------------------------------------------------------------------ */
/*  Profile types (plugin ABI v5)                                      */
/* ------------------------------------------------------------------ */

void
pn_node_factory_register_profile_type (PnNodeFactory   *self,
                                       PnProfileSchema *schema)
{
    const gchar *type_id;

    g_return_if_fail (PN_IS_NODE_FACTORY (self));
    g_return_if_fail (schema != NULL);

    type_id = pn_profile_schema_get_type_id (schema);

    if (pn_node_factory_lookup_profile_type (self, type_id) != NULL)
    {
        /* Already known (e.g. the plugin was found via two search dirs).
         * Drop the caller's transferred ref and keep the first. */
        pn_profile_schema_unref (schema);
        return;
    }

    g_ptr_array_add (self->profile_types, schema);
}

PnProfileSchema *
pn_node_factory_lookup_profile_type (PnNodeFactory *self,
                                     const gchar   *type_id)
{
    guint i;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);
    g_return_val_if_fail (type_id != NULL, NULL);

    for (i = 0; i < self->profile_types->len; i++)
    {
        PnProfileSchema *s = g_ptr_array_index (self->profile_types, i);
        if (g_strcmp0 (pn_profile_schema_get_type_id (s), type_id) == 0)
            return s;
    }
    return NULL;
}

GList *
pn_node_factory_list_profile_types (PnNodeFactory *self)
{
    GList *out = NULL;
    guint  i;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);

    for (i = self->profile_types->len; i > 0; i--)
        out = g_list_prepend (out,
                              g_ptr_array_index (self->profile_types, i - 1));
    return out;
}

guint
pn_node_factory_get_n_types (PnNodeFactory *self)
{
    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), 0);
    return self->entries->len;
}

GType
pn_node_factory_get_type_at (PnNodeFactory *self, guint index)
{
    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), G_TYPE_INVALID);

    if (index >= self->entries->len)
        return G_TYPE_INVALID;

    return g_array_index (self->entries, Entry, index).type;
}

GType
pn_node_factory_lookup (PnNodeFactory *self, const gchar *type_name)
{
    guint i;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), G_TYPE_INVALID);
    g_return_val_if_fail (type_name != NULL, G_TYPE_INVALID);

    for (i = 0; i < self->entries->len; i++)
    {
        const Entry *e = &g_array_index (self->entries, Entry, i);
        if (g_strcmp0 (g_type_name (e->type), type_name) == 0)
            return e->type;
    }
    return G_TYPE_INVALID;
}

const gchar *
pn_node_factory_get_class_name (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_class_name (k) : NULL;
}

const gchar *
pn_node_factory_get_category (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_category (k) : NULL;
}

const gchar *
pn_node_factory_get_icon (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_icon (k) : NULL;
}

const gchar *
pn_node_factory_get_palette_icon (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);

    /* Prefer the class-level palette override when set so node types
     * whose instance icon flips with runtime state still appear with
     * a stable glyph in the palette.  Falls back to the canonical
     * #PnNodeClass.icon — the same fallback chain pn-palette.c used
     * before the factory existed. */
    if (k == NULL)
        return NULL;
    return k->palette_icon != NULL ? k->palette_icon
                                   : pn_node_class_get_icon (k);
}

const PnColor *
pn_node_factory_get_color (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_color (k) : NULL;
}

gboolean
pn_node_factory_get_has_input (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_has_input (k) : FALSE;
}

gboolean
pn_node_factory_get_has_output (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_has_output (k) : FALSE;
}

const gchar *
pn_node_factory_get_plugin_name (PnNodeFactory *self, GType type)
{
    PnNodeClass *k = factory_klass (self, type);
    return k != NULL ? pn_node_class_get_plugin_name (k) : NULL;
}

PnNode *
pn_node_factory_create_for_type (PnNodeFactory *self, GType type)
{
    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);

    if (factory_find (self, type) == NULL)
    {
        g_warning ("pn-node-factory: cannot instantiate \"%s\" — "
                   "not registered",
                   g_type_name (type));
        return NULL;
    }
    return PN_NODE (g_object_new (type, NULL));
}

PnNode *
pn_node_factory_create (PnNodeFactory *self, const gchar *type_name)
{
    GType t;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);
    g_return_val_if_fail (type_name != NULL, NULL);

    t = pn_node_factory_lookup (self, type_name);
    if (t == G_TYPE_INVALID)
    {
        g_warning ("pn-node-factory: unknown node type \"%s\"", type_name);
        return NULL;
    }
    return pn_node_factory_create_for_type (self, t);
}

/* ------------------------------------------------------------------ */
/*  Plugin loading                                                     */
/*                                                                     */
/*  Every successfully-loaded module is pinned in memory               */
/*  (g_module_make_resident) — registered #GType machinery cannot be   */
/*  un-registered, so the .so must outlive every node instance the    */
/*  user might still hold on screen.  We deliberately do not keep a    */
/*  list of #GModule pointers; once a module is resident the runtime  */
/*  linker manages its lifetime and the host has no business closing   */
/*  it.                                                                */
/* ------------------------------------------------------------------ */

G_DEFINE_QUARK (pn-node-factory-plugin-error, pn_node_factory_plugin_error)

static gchar *
canonicalise_plugin_path (const gchar *path)
{
    /* Resolve symlinks / "../" so the same .so reachable through two
     * different paths deduplicates against itself in loaded_paths.
     * Falls back to a normalised absolute form when the file is gone
     * by the time we look (g_canonicalize_filename does not stat). */
    return g_canonicalize_filename (path, NULL);
}

gboolean
pn_node_factory_load_plugin (PnNodeFactory *self,
                             const gchar    *path,
                             GError        **error)
{
    GModule          *module;
    gpointer          symbol = NULL;
    PnPluginInitFunc  init;
    const PnPluginInfo *info;
    gchar            *canonical;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    if (!g_module_supported ())
    {
        g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                     PN_NODE_FACTORY_PLUGIN_ERROR_OPEN,
                     "GModule is not supported on this platform");
        return FALSE;
    }

    canonical = canonicalise_plugin_path (path);

    /* Rejected by the host's policy hook -- skip silently.  The dedup
     * tables stay untouched: that way a later call to
     * pn_node_factory_list_plugins() still surfaces the basename so
     * the Preferences dialog can show (and un-check) the row.  We
     * check the basename rather than the canonical path because the
     * policy (the editor's PnPreferences-backed filter) keys on the
     * user-visible filename, which survives the install vs build-tree
     * path distinction.  A headless run installs no filter, so this is
     * skipped entirely and every discovered plugin loads. */
    if (self->plugin_filter != NULL)
    {
        gchar *basename = g_path_get_basename (canonical);

        if (self->plugin_filter (basename, self->plugin_filter_data))
        {
            g_message ("pipnode: plugin \"%s\" skipped "
                       "(disabled by host policy)", basename);
            g_free (basename);
            g_free (canonical);
            return TRUE;
        }
        g_free (basename);
    }

    /* Already loaded — silently treat as success so a directory scan
     * that re-encounters the same plugin (via overlapping search
     * paths) is a no-op rather than an error. */
    if (g_hash_table_contains (self->loaded_paths, canonical))
    {
        g_free (canonical);
        return TRUE;
    }

    /* Same dedup, by filename.  Catches the case where the installed
     * plugin and the build-tree-fallback plugin are physically
     * different files but ship the same #GType set — see the
     * loaded_basenames comment in struct _PnNodeFactory.  Whichever
     * directory was scanned first wins; the search order in
     * pn_node_factory_load_plugins_default puts PIPNODE_PLUGIN_PATH
     * before the user dir, the user dir before the system dir, and
     * the system dir before the build-tree fallback, so the more
     * specific / authoritative copy wins on every overlap. */
    {
        gchar *basename = g_path_get_basename (canonical);
        if (g_hash_table_contains (self->loaded_basenames, basename))
        {
            g_free (basename);
            g_free (canonical);
            return TRUE;
        }
        g_free (basename);
    }

    module = g_module_open (canonical, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (module == NULL)
    {
        g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                     PN_NODE_FACTORY_PLUGIN_ERROR_OPEN,
                     "failed to open plugin \"%s\": %s",
                     canonical, g_module_error ());
        g_free (canonical);
        return FALSE;
    }

    if (!g_module_symbol (module, PN_PLUGIN_INIT_SYMBOL, &symbol)
        || symbol == NULL)
    {
        g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                     PN_NODE_FACTORY_PLUGIN_ERROR_NO_ENTRY,
                     "plugin \"%s\" does not export %s",
                     canonical, PN_PLUGIN_INIT_SYMBOL);
        g_module_close (module);
        g_free (canonical);
        return FALSE;
    }

    init = (PnPluginInitFunc) symbol;

    /* Run the entry point with current_plugin_name unset; the plugin
     * may pin its own #PnNodeClass.plugin_name in _class_init, but
     * for types that leave it at the inherited default we want to
     * fill in the plugin's identifier from PnPluginInfo.  We do this
     * in two passes: call init() with no name set, capture the
     * baseline registration count, then walk the new entries with
     * info->name afterwards.  This avoids racing the init function
     * (which could legitimately set its own per-class plugin_name). */
    {
        guint before = self->entries->len;
        guint i;

        info = init (self);

        if (info == NULL)
        {
            g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                         PN_NODE_FACTORY_PLUGIN_ERROR_INIT_FAILED,
                         "plugin \"%s\": %s returned NULL",
                         canonical, PN_PLUGIN_INIT_SYMBOL);
            g_module_close (module);
            g_free (canonical);
            return FALSE;
        }

        if (info->abi_version != PN_PLUGIN_ABI_VERSION)
        {
            g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                         PN_NODE_FACTORY_PLUGIN_ERROR_ABI_MISMATCH,
                         "plugin \"%s\": ABI version %d does not match "
                         "host ABI %d",
                         canonical, info->abi_version,
                         PN_PLUGIN_ABI_VERSION);
            /* The plugin may already have appended entries to the
             * registry before realising its ABI was wrong — pop them
             * back off and release the class refs we acquired so a
             * stale plugin does not leave half-broken types in the
             * palette. */
            while (self->entries->len > before)
            {
                Entry *e = &g_array_index (self->entries, Entry,
                                           self->entries->len - 1);
                if (e->klass != NULL)
                    g_type_class_unref (e->klass);
                g_array_remove_index (self->entries,
                                      self->entries->len - 1);
            }
            g_module_close (module);
            g_free (canonical);
            return FALSE;
        }

        if (info->name == NULL || *info->name == '\0')
        {
            g_set_error (error, PN_NODE_FACTORY_PLUGIN_ERROR,
                         PN_NODE_FACTORY_PLUGIN_ERROR_INIT_FAILED,
                         "plugin \"%s\": PnPluginInfo.name is empty",
                         canonical);
            g_module_close (module);
            g_free (canonical);
            return FALSE;
        }

        /* Backfill plugin_name onto every type the init function
         * registered that did not pin a value of its own.  Skip
         * built-in defaults so an in-process plugin that wraps the
         * factory cannot rename "Internal" to itself for nodes it
         * happened to discover already-registered. */
        for (i = before; i < self->entries->len; i++)
        {
            Entry *e = &g_array_index (self->entries, Entry, i);
            if (e->klass != NULL
                && g_strcmp0 (e->klass->plugin_name, "Internal") == 0)
            {
                e->klass->plugin_name = info->name;
            }
        }
    }

    /* Pin the .so for the lifetime of the process — see header. */
    g_module_make_resident (module);

    {
        gchar *bn = g_path_get_basename (canonical);
        /* loaded_plugins owns its key, hash table replaces dup keys
         * cleanly; loaded_basenames also wants ownership so we dup. */
        g_hash_table_insert (self->loaded_plugins, g_strdup (bn),
                             (gpointer) info);
        g_hash_table_add    (self->loaded_basenames, bn); /* takes own */
    }
    g_hash_table_add (self->loaded_paths, canonical);  /* takes ownership */

    g_message ("pipnode: loaded plugin \"%s\" %s from %s",
               info->name, info->version, path);
    return TRUE;
}

guint
pn_node_factory_load_plugins_in_dir (PnNodeFactory *self, const gchar *dir)
{
    GDir        *d;
    const gchar *name;
    const gchar *suffix = "." G_MODULE_SUFFIX;
    gsize        suffix_len;
    guint        loaded = 0;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), 0);
    g_return_val_if_fail (dir != NULL, 0);

    /* Empty / missing directory is the common case (a user who has
     * never installed any plugin will not have created
     * ~/.local/share/pipnode/plugins) — silently return 0 instead of
     * warning. */
    d = g_dir_open (dir, 0, NULL);
    if (d == NULL)
        return 0;

    suffix_len = strlen (suffix);

    while ((name = g_dir_read_name (d)) != NULL)
    {
        gsize    len = strlen (name);
        gchar   *path;
        GError  *error = NULL;

        if (len <= suffix_len
            || g_strcmp0 (name + len - suffix_len, suffix) != 0)
            continue;

        /* Skip companion GUI modules (<base>-gui.<suffix>).  They export
         * pn_plugin_gui_init, not pn_plugin_init, and are loaded by the
         * editor's GUI loader keyed off their logic plugin — never as a
         * standalone plugin in their own right (TODO #23, Phase 6). */
        if (g_str_has_suffix (name, PN_PLUGIN_GUI_INFIX "." G_MODULE_SUFFIX))
            continue;

        path = g_build_filename (dir, name, NULL);
        if (pn_node_factory_load_plugin (self, path, &error))
            loaded++;
        else
        {
            g_warning ("pipnode: %s",
                       error != NULL ? error->message : "unknown error");
            g_clear_error (&error);
        }
        g_free (path);
    }

    g_dir_close (d);
    return loaded;
}

guint
pn_node_factory_load_plugins_default (PnNodeFactory *self)
{
    const gchar *env;
    guint        loaded = 0;
    gchar       *user_dir;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), 0);

    /* (1) PIPNODE_PLUGIN_PATH — colon-separated, front-to-back so
     * an in-tree plugin under test takes precedence over an older
     * copy installed system-wide.  G_SEARCHPATH_SEPARATOR_S would
     * be ":" on POSIX and ";" on Windows; using it keeps the env
     * var portable to whatever platform GModule itself supports. */
    env = g_getenv ("PIPNODE_PLUGIN_PATH");
    if (env != NULL && *env != '\0')
    {
        gchar **parts = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, -1);
        guint   i;

        for (i = 0; parts[i] != NULL; i++)
        {
            if (*parts[i] != '\0')
                loaded += pn_node_factory_load_plugins_in_dir (self,
                                                               parts[i]);
        }
        g_strfreev (parts);
    }

    /* (2) Per-user directory.  GLib's g_get_user_data_dir() already
     * honours $XDG_DATA_HOME with the canonical fallback to
     * ~/.local/share, so we do not have to spell that out. */
    user_dir = g_build_filename (g_get_user_data_dir (),
                                 "pipnode", "plugins", NULL);
    loaded += pn_node_factory_load_plugins_in_dir (self, user_dir);
    g_free (user_dir);

    /* (3) System pkglibdir/plugins, baked in at build time.  The
     * macro is only set when the build system passed -DPKGLIBDIR=…;
     * a configure step that did not (e.g. an out-of-tree consumer
     * that links libpipnode without using its Makefile) just skips
     * this directory. */
#ifdef PKGLIBDIR
    {
        gchar *sys_dir = g_build_filename (PKGLIBDIR, "plugins", NULL);
        loaded += pn_node_factory_load_plugins_in_dir (self, sys_dir);
        g_free (sys_dir);
    }
#endif

    /* (4) Build-tree fallback for the bundled plugins shipped with
     * pipnode itself (`plugins/network`, `plugins/tasmota`).
     * Mirrors the
     * SRCDIR fallback resolve_help_path() uses for the help pages:
     * the macro is baked into both the installed binary and the
     * in-tree binary, but the directory only exists on the host
     * that did the build, so an installed pipnode running on a
     * fresh machine sees the test fail and skips the path silently.
     * The benefit is that `src/pipnode --file flow.json` works the
     * moment the developer has run `make`, without their having to
     * remember to set PIPNODE_PLUGIN_PATH for first-party plugins
     * the project itself ships. */
#ifdef ABS_TOP_BUILDDIR
    {
        gchar *plugins_root = g_build_filename (ABS_TOP_BUILDDIR,
                                                "plugins", NULL);
        GDir  *root_dir;

        /* Each per-plugin subdirectory lands its loadable module in
         * the libtool-conventional .libs/ subdir, so we walk one
         * level of subdirs and scan the .libs of each. */
        root_dir = g_dir_open (plugins_root, 0, NULL);
        if (root_dir != NULL)
        {
            const gchar *child;
            while ((child = g_dir_read_name (root_dir)) != NULL)
            {
                gchar *libs = g_build_filename (plugins_root, child,
                                                ".libs", NULL);
                if (g_file_test (libs, G_FILE_TEST_IS_DIR))
                    loaded += pn_node_factory_load_plugins_in_dir (
                            self, libs);
                g_free (libs);
            }
            g_dir_close (root_dir);
        }

        g_free (plugins_root);
    }
#endif

    return loaded;
}

GList *
pn_node_factory_get_loaded_plugin_paths (PnNodeFactory *self)
{
    GHashTableIter iter;
    gpointer       key;
    GList         *result = NULL;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);

    /* loaded_paths is the set of canonical .so paths the loader actually
     * passed to g_module_open — i.e. exactly the logic plugins whose
     * #GTypes are now registered, after the policy filter and the path /
     * basename dedup.  The editor's GUI loader walks this to find each
     * one's sibling companion (TODO #23, Phase 6). */
    g_hash_table_iter_init (&iter, self->loaded_paths);
    while (g_hash_table_iter_next (&iter, &key, NULL))
        result = g_list_prepend (result, g_strdup ((const gchar *) key));

    return result;
}

/* ------------------------------------------------------------------ */
/*  Discovery                                                          */
/* ------------------------------------------------------------------ */

void
pn_plugin_descriptor_free (PnPluginDescriptor *desc)
{
    if (desc == NULL)
        return;
    g_free (desc->basename);
    g_free (desc->path);
    g_free (desc);
}

/** Add every .so in @dir to @basename_to_path if a file with that
 *  basename has not been recorded yet.  Matches the precedence order
 *  pn_node_factory_load_plugins_default() applies -- earlier scans
 *  win, so the descriptor list reflects which copy the loader would
 *  have picked at startup. */
static void
discover_dir (const gchar *dir, GHashTable *basename_to_path)
{
    GDir        *d;
    const gchar *name;
    const gchar *suffix = "." G_MODULE_SUFFIX;
    gsize        suffix_len;

    d = g_dir_open (dir, 0, NULL);
    if (d == NULL)
        return;

    suffix_len = strlen (suffix);

    while ((name = g_dir_read_name (d)) != NULL)
    {
        gsize len = strlen (name);
        if (len <= suffix_len
            || g_strcmp0 (name + len - suffix_len, suffix) != 0)
            continue;

        /* Companion GUI modules are not standalone plugins — keep them
         * out of the discovered list the Preferences dialog shows
         * (TODO #23, Phase 6). */
        if (g_str_has_suffix (name, PN_PLUGIN_GUI_INFIX "." G_MODULE_SUFFIX))
            continue;

        if (g_hash_table_contains (basename_to_path, name))
            continue;

        g_hash_table_insert (basename_to_path,
                             g_strdup (name),
                             g_build_filename (dir, name, NULL));
    }

    g_dir_close (d);
}

GList *
pn_node_factory_list_plugins (PnNodeFactory *self)
{
    GHashTable  *map;
    GHashTableIter iter;
    gpointer     k, v;
    GList       *result = NULL;
    const gchar *env;
    gchar       *user_dir;

    g_return_val_if_fail (PN_IS_NODE_FACTORY (self), NULL);

    map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    /* Mirror the order of pn_node_factory_load_plugins_default() so
     * the discovered path for each basename matches the .so the
     * loader would have picked. */
    env = g_getenv ("PIPNODE_PLUGIN_PATH");
    if (env != NULL && *env != '\0')
    {
        gchar **parts = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, -1);
        guint   i;
        for (i = 0; parts[i] != NULL; i++)
            if (*parts[i] != '\0')
                discover_dir (parts[i], map);
        g_strfreev (parts);
    }

    user_dir = g_build_filename (g_get_user_data_dir (),
                                 "pipnode", "plugins", NULL);
    discover_dir (user_dir, map);
    g_free (user_dir);

#ifdef PKGLIBDIR
    {
        gchar *sys_dir = g_build_filename (PKGLIBDIR, "plugins", NULL);
        discover_dir (sys_dir, map);
        g_free (sys_dir);
    }
#endif

#ifdef ABS_TOP_BUILDDIR
    {
        gchar *plugins_root = g_build_filename (ABS_TOP_BUILDDIR,
                                                "plugins", NULL);
        GDir  *root_dir;

        root_dir = g_dir_open (plugins_root, 0, NULL);
        if (root_dir != NULL)
        {
            const gchar *child;
            while ((child = g_dir_read_name (root_dir)) != NULL)
            {
                gchar *libs = g_build_filename (plugins_root, child,
                                                ".libs", NULL);
                if (g_file_test (libs, G_FILE_TEST_IS_DIR))
                    discover_dir (libs, map);
                g_free (libs);
            }
            g_dir_close (root_dir);
        }

        g_free (plugins_root);
    }
#endif

    /* Build a descriptor list from the map.  Stable order via sort on
     * basename so the dialog renders the same row order across opens. */
    {
        GList *keys = g_hash_table_get_keys (map);
        GList *l;

        keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);

        for (l = keys; l != NULL; l = l->next)
        {
            const gchar        *basename = l->data;
            const gchar        *path     = g_hash_table_lookup (map, basename);
            const PnPluginInfo *info     =
                    g_hash_table_lookup (self->loaded_plugins, basename);
            PnPluginDescriptor *desc;

            desc = g_new0 (PnPluginDescriptor, 1);
            desc->basename = g_strdup (basename);
            desc->path     = g_strdup (path);
            desc->loaded   = (info != NULL);
            if (info != NULL)
            {
                desc->name        = info->name;
                desc->version     = info->version;
                desc->description = info->description;
            }
            result = g_list_prepend (result, desc);
        }
        result = g_list_reverse (result);
        g_list_free (keys);
    }

    /* Loaded plugins whose source file no longer exists on disk (an
     * unusual but possible state -- e.g. the user uninstalled the
     * package mid-session) are still worth showing so the user knows
     * the running session has them resident.  Walk the loaded_plugins
     * map and append any basename that did not surface from the dir
     * scan. */
    g_hash_table_iter_init (&iter, self->loaded_plugins);
    while (g_hash_table_iter_next (&iter, &k, &v))
    {
        const gchar        *basename = k;
        const PnPluginInfo *info     = v;
        PnPluginDescriptor *desc;

        if (g_hash_table_contains (map, basename))
            continue;

        desc = g_new0 (PnPluginDescriptor, 1);
        desc->basename    = g_strdup (basename);
        desc->path        = NULL;
        desc->loaded      = TRUE;
        desc->name        = info->name;
        desc->version     = info->version;
        desc->description = info->description;
        result = g_list_append (result, desc);
    }

    g_hash_table_unref (map);
    return result;
}
