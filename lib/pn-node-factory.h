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

#ifndef PN_NODE_FACTORY_H
#define PN_NODE_FACTORY_H

#include <glib-object.h>

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNodeFactory                                                      */
/*                                                                     */
/*  Process-wide registry of #PnNode subclasses.  Holds the canonical  */
/*  list of node types the application knows about, in the order they  */
/*  should appear in the palette, and is the single point through      */
/*  which nodes are constructed at runtime — drag-from-palette, file   */
/*  load, and clipboard paste all funnel through                       */
/*  pn_node_factory_create_for_type() / pn_node_factory_create() so a  */
/*  newly-loaded plugin's types become reachable everywhere with one   */
/*  call to pn_node_factory_register().                                */
/*                                                                     */
/*  Built-in types are registered lazily on the first call to          */
/*  pn_node_factory_get_default().  Plugins (when supported) will      */
/*  call pn_node_factory_register() on their #GType after their shared */
/*  object is loaded; the factory holds a class ref on every           */
/*  registered type so the metadata pinned on the #PnNodeClass struct  */
/*  remains valid for the lifetime of the process.                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NODE_FACTORY (pn_node_factory_get_type ())

G_DECLARE_FINAL_TYPE (PnNodeFactory, pn_node_factory, PN, NODE_FACTORY, GObject)

/**
 * pn_node_factory_get_default:
 *
 * Returns the process-wide #PnNodeFactory.  The first call instantiates
 * the singleton and registers the application's built-in node types.
 * The returned object is owned by the factory module; do not unref.
 */
PnNodeFactory *pn_node_factory_get_default (void);

/**
 * pn_node_factory_register:
 * @self: the factory
 * @type: a #GType derived from #PN_TYPE_NODE
 *
 * Adds @type to the factory's registry.  Refs the class so the
 * metadata pinned on the #PnNodeClass struct survives until shutdown,
 * and returns %TRUE the first time a type is registered or %FALSE if
 * the type was already known (or is not a #PnNode descendant).
 */
gboolean       pn_node_factory_register (PnNodeFactory *self, GType type);

/**
 * pn_node_factory_has_type:
 * @self: the factory
 * @type: candidate #GType
 *
 * Returns %TRUE iff @type was previously registered with @self.
 */
gboolean       pn_node_factory_has_type (PnNodeFactory *self, GType type);

/**
 * pn_node_factory_get_n_types:
 * @self: the factory
 *
 * Number of registered node types.
 */
guint          pn_node_factory_get_n_types (PnNodeFactory *self);

/**
 * pn_node_factory_get_type_at:
 * @self:  the factory
 * @index: zero-based registration order
 *
 * Returns the #GType registered at @index, or %G_TYPE_INVALID when
 * @index is out of range.  Iteration order matches registration order
 * — which is also the order the palette displays the types in.
 */
GType          pn_node_factory_get_type_at (PnNodeFactory *self, guint index);

/**
 * pn_node_factory_lookup:
 * @self:      the factory
 * @type_name: registered #GType name (e.g. "PnRtc")
 *
 * Resolves @type_name to its #GType.  Returns %G_TYPE_INVALID when no
 * registered type carries that name, even if the type system itself
 * knows about a matching #GType — the factory is the source of truth
 * for "is this node class available right now".
 */
GType          pn_node_factory_lookup (PnNodeFactory *self,
                                       const gchar   *type_name);

/* ------------------------------------------------------------------ */
/*  Metadata convenience accessors                                     */
/*                                                                     */
/*  Thin wrappers around pn_node_class_get_* that take a #GType so     */
/*  callers do not need to spell out g_type_class_ref() / unref pairs. */
/*  All four return values borrowed from the registered class struct;  */
/*  the strings remain valid for the lifetime of the process.          */
/* ------------------------------------------------------------------ */

const gchar   *pn_node_factory_get_class_name   (PnNodeFactory *self, GType type);
const gchar   *pn_node_factory_get_category     (PnNodeFactory *self, GType type);
const gchar   *pn_node_factory_get_icon         (PnNodeFactory *self, GType type);
const gchar   *pn_node_factory_get_palette_icon (PnNodeFactory *self, GType type);
const PnColor *pn_node_factory_get_color        (PnNodeFactory *self, GType type);
gboolean       pn_node_factory_get_has_input    (PnNodeFactory *self, GType type);
gboolean       pn_node_factory_get_has_output   (PnNodeFactory *self, GType type);
const gchar   *pn_node_factory_get_plugin_name  (PnNodeFactory *self, GType type);

/**
 * pn_node_factory_create_for_type:
 * @self: the factory
 * @type: a registered #GType
 *
 * Constructs a fresh instance of @type via g_object_new().  Returns
 * %NULL when @type is not registered with @self.  The caller owns the
 * returned reference.
 */
PnNode        *pn_node_factory_create_for_type (PnNodeFactory *self, GType type);

/**
 * pn_node_factory_create:
 * @self:      the factory
 * @type_name: registered #GType name
 *
 * Convenience wrapper around pn_node_factory_lookup() +
 * pn_node_factory_create_for_type().  Returns %NULL with a warning
 * when @type_name is unknown to the factory.
 */
PnNode        *pn_node_factory_create (PnNodeFactory *self, const gchar *type_name);

/* ------------------------------------------------------------------ */
/*  Plugin loading                                                     */
/*                                                                     */
/*  Third-party node types ship as shared objects that export the     */
/*  pn_plugin_init() entry point declared in <pn-plugin.h>.  Loading   */
/*  a plugin dlopens the .so via #GModule, looks up the entry point, */
/*  and calls it with @self.  On success the module is pinned in     */
/*  memory (g_module_make_resident) — registered #GType machinery     */
/*  cannot be unregistered, so the .so must outlive every #PnNode     */
/*  instance the user might still hold on screen.                     */
/*                                                                     */
/*  Plugins remain registered with the factory for the lifetime of    */
/*  the process; there is no unload path.  Reloading a plugin (e.g.   */
/*  during development) requires restarting the host.                 */
/* ------------------------------------------------------------------ */

/**
 * PN_NODE_FACTORY_PLUGIN_ERROR:
 *
 * #GError domain raised by pn_node_factory_load_plugin() and friends
 * when a plugin's .so cannot be opened, the entry point is missing,
 * the declared ABI version does not match, or the entry point
 * returned %NULL.
 */
#define PN_NODE_FACTORY_PLUGIN_ERROR (pn_node_factory_plugin_error_quark ())

GQuark pn_node_factory_plugin_error_quark (void);

typedef enum
{
    PN_NODE_FACTORY_PLUGIN_ERROR_OPEN,         /* dlopen failed */
    PN_NODE_FACTORY_PLUGIN_ERROR_NO_ENTRY,     /* missing pn_plugin_init */
    PN_NODE_FACTORY_PLUGIN_ERROR_ABI_MISMATCH, /* abi_version != current */
    PN_NODE_FACTORY_PLUGIN_ERROR_INIT_FAILED,  /* entry returned NULL */
} PnNodeFactoryPluginError;

/**
 * PnPluginFilterFunc:
 * @basename: the plugin module's filename (e.g. "pipnode_network.so")
 * @user_data: the pointer passed to pn_node_factory_set_plugin_filter()
 *
 * Predicate consulted by the loader for each discovered plugin.  Return
 * %TRUE to skip the plugin (it is not opened), %FALSE to load it.
 */
typedef gboolean (*PnPluginFilterFunc) (const gchar *basename,
                                        gpointer     user_data);

/**
 * pn_node_factory_set_plugin_filter:
 * @self: the factory
 * @filter: (nullable) (scope notified): policy predicate, or %NULL to clear
 * @user_data: (nullable): passed through to @filter
 * @user_data_destroy: (nullable): called on @user_data when the filter is
 *   replaced or the factory is finalized
 *
 * Installs a plugin-load policy hook.  pn_node_factory_load_plugin()
 * calls @filter with each discovered plugin's basename and silently skips
 * the plugin when it returns %TRUE.  This keeps the GTK-bound preferences
 * machinery out of the GTK-free core: the editor installs a filter that
 * consults #PnPreferences, while the headless runner installs none and so
 * loads every discovered plugin (TODO #23, Phase 5).
 */
void pn_node_factory_set_plugin_filter (PnNodeFactory      *self,
                                        PnPluginFilterFunc  filter,
                                        gpointer            user_data,
                                        GDestroyNotify      user_data_destroy);

/**
 * pn_node_factory_load_plugin:
 * @self:  the factory
 * @path:  filesystem path to a plugin .so / .dylib / .dll
 * @error: (out) (nullable): receives a #GError on failure
 *
 * Loads a single plugin from @path, calls its pn_plugin_init() entry
 * point with @self, and pins the module in memory on success.  Types
 * the plugin registers during init become reachable through the
 * normal pn_node_factory_lookup() / pn_node_factory_create() paths.
 *
 * Returns %TRUE on success, %FALSE on failure (with @error set).
 * Loading the same path twice is a no-op that returns %TRUE — the
 * factory keeps a deduplication set keyed on the resolved canonical
 * path so a plugin discovered via two search directories registers
 * its types only once.
 */
gboolean pn_node_factory_load_plugin (PnNodeFactory *self,
                                      const gchar    *path,
                                      GError        **error);

/**
 * pn_node_factory_load_plugins_in_dir:
 * @self: the factory
 * @dir:  directory to scan
 *
 * Scans @dir non-recursively for shared-object files (matched by the
 * platform's loadable-module suffix as reported by GModule) and
 * loads each one through pn_node_factory_load_plugin().  Failures on
 * individual plugins are logged via g_warning() and do not abort the
 * scan.  A non-existent or unreadable @dir is treated as "no plugins
 * here" and returns 0 silently — having an optional plugin
 * directory missing on disk is the common case, not an error.
 *
 * Returns the number of plugins successfully loaded.
 */
guint    pn_node_factory_load_plugins_in_dir (PnNodeFactory *self,
                                              const gchar    *dir);

/**
 * pn_node_factory_load_plugins_default:
 * @self: the factory
 *
 * Loads plugins from the default search path:
 *
 *   1. Every directory in the colon-separated #PIPNODE_PLUGIN_PATH
 *      environment variable (front-most takes precedence — useful
 *      for testing an in-tree plugin against an installed pipnode);
 *   2. <literal>$XDG_DATA_HOME/pipnode/plugins</literal> (falling
 *      back to <literal>~/.local/share/pipnode/plugins</literal>);
 *   3. The system plugin directory baked in at build time
 *      (<literal>$pkglibdir/plugins</literal>, typically
 *      <literal>/usr/lib/pipnode/plugins</literal>).
 *
 * Returns the total number of plugins successfully loaded across
 * every directory.
 */
guint    pn_node_factory_load_plugins_default (PnNodeFactory *self);

/* ------------------------------------------------------------------ */
/*  Plugin discovery (for the Preferences dialog)                      */
/* ------------------------------------------------------------------ */

/**
 * PnPluginDescriptor:
 * @basename: filename of the plugin .so (e.g. "pipnode_network.so").
 *            Used as the stable identity for the disabled-plugins
 *            preference -- both fields are case-sensitive strings.
 * @path:     full path to the .so on disk.  Owned by the descriptor.
 * @loaded:   %TRUE iff the plugin's pn_plugin_init() ran successfully
 *            during this process's startup.  A plugin file present on
 *            disk but listed in the disabled-plugins preference has
 *            @loaded == %FALSE here.
 * @name:     #PnPluginInfo.name from the loaded plugin (e.g.
 *            "EchoPlugin").  %NULL when @loaded is %FALSE.
 * @version:  #PnPluginInfo.version from the loaded plugin.  %NULL
 *            when @loaded is %FALSE.
 * @description: optional #PnPluginInfo.description.  May be %NULL.
 *
 * Per-plugin record returned by pn_node_factory_list_plugins().  The
 * caller frees a descriptor with pn_plugin_descriptor_free() and the
 * whole list with `g_list_free_full (list, (GDestroyNotify) pn_plugin_descriptor_free)`.
 */
typedef struct
{
    gchar       *basename;
    gchar       *path;
    gboolean     loaded;
    const gchar *name;        /* borrowed: lifetime = process */
    const gchar *version;     /* borrowed: lifetime = process */
    const gchar *description; /* borrowed: lifetime = process */
} PnPluginDescriptor;

/**
 * pn_plugin_descriptor_free:
 * @desc: descriptor returned by pn_node_factory_list_plugins().
 *
 * Frees @desc and its owned strings.
 */
void pn_plugin_descriptor_free (PnPluginDescriptor *desc);

/**
 * pn_node_factory_list_plugins:
 * @self: the factory.
 *
 * Walks the same set of search directories pn_node_factory_load_plugins_default()
 * does and returns a #GList of #PnPluginDescriptor entries, one per
 * unique .so basename found on disk.  For plugins that were loaded
 * this session the descriptor carries the live #PnPluginInfo
 * metadata; for plugins present on disk but currently skipped (by
 * the disabled-plugins preference) the loaded flag is %FALSE and the
 * metadata fields are %NULL.
 *
 * The list is intended for the "Plugins" page of the Preferences
 * dialog -- the canonical view of "what plugins does pipnode know
 * about on this machine".  The caller frees the list with
 * `g_list_free_full (list, (GDestroyNotify) pn_plugin_descriptor_free)`.
 *
 * Returns: (transfer full) (element-type PnPluginDescriptor): per-plugin descriptors.
 */
GList *pn_node_factory_list_plugins (PnNodeFactory *self);

G_END_DECLS

#endif /* PN_NODE_FACTORY_H */
