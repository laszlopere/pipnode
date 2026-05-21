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

#ifndef PN_PLUGIN_H
#define PN_PLUGIN_H

#include <glib.h>

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Pipnode plugin ABI                                                 */
/*                                                                     */
/*  A pipnode plugin is a shared object that registers one or more     */
/*  #PnNode subclasses with the process-wide #PnNodeFactory at load    */
/*  time.  The host dlopens the file, looks up the well-known symbol  */
/*  pn_plugin_init(), and calls it once with the factory it should    */
/*  register against.  The plugin returns a borrowed pointer to a     */
/*  statically-allocated #PnPluginInfo describing itself; on success  */
/*  the host pins the module in memory (g_module_make_resident) so    */
/*  the registered #GType machinery stays valid for the lifetime of   */
/*  the process — GTypes cannot be unregistered, so the .so must     */
/*  outlive every node instance the user might still hold on screen.  */
/*                                                                     */
/*  The minimal plugin therefore looks like:                           */
/*                                                                     */
/*  ```c                                                               */
/*  G_MODULE_EXPORT const PnPluginInfo *                               */
/*  pn_plugin_init (PnNodeFactory *factory)                            */
/*  {                                                                  */
/*      static const PnPluginInfo info = {                             */
/*          .abi_version = PN_PLUGIN_ABI_VERSION,                      */
/*          .name        = "EchoPlugin",                               */
/*          .version     = "1.0.0",                                    */
/*          .description = "Adds a no-op echo node",                   */
/*      };                                                             */
/*      pn_node_factory_register (factory, MY_TYPE_ECHO);              */
/*      return &info;                                                  */
/*  }                                                                  */
/*  ```                                                                */
/*                                                                     */
/*  ABI versioning                                                     */
/*  --------------                                                     */
/*  The host refuses to load a plugin whose declared abi_version does  */
/*  not match #PN_PLUGIN_ABI_VERSION.  Bump this constant whenever an  */
/*  incompatible change to either #PnNode, #PnNodeClass, the factory   */
/*  registration contract, or the plugin entry-point signature lands  */
/*  in a release.  Plugins ship as shared objects against a specific   */
/*  ABI; the version check exists so a stale plugin sitting in the    */
/*  user's plugin directory cannot crash the host with a silent       */
/*  layout mismatch.                                                   */
/* ------------------------------------------------------------------ */

/**
 * PN_PLUGIN_ABI_VERSION:
 *
 * Current pipnode plugin ABI version.  Bumped whenever an
 * incompatible change to #PnNode / #PnNodeClass / the factory
 * registration contract ships.  Plugins copy this constant into
 * their #PnPluginInfo at build time; the host refuses to load a
 * plugin whose declared version differs.
 *
 * History:
 *   1 — initial release; minimum #PnNodeClass with #receive vfunc
 *       plus the four optional rendering vfuncs
 *       (#get_size, #get_header_height, #paint_plot, #scroll).
 *   2 — appended three optional dialog-extension vfuncs to
 *       #PnNodeClass (#build_property_editor, #build_class_tab,
 *       #build_extra_pages) so plugin-shipped node types can
 *       contribute custom widgets, custom tabs, and additional
 *       notebook pages to the node settings dialog the same way
 *       the in-tree #PnMeshtastic / #PnFilter / #PnSet types do.
 *       Adds a new public helper header
 *       <literal>&lt;pipnode/pn-node-dialog-helpers.h&gt;</literal>
 *       exposing the host's grid / row / default-editor / append-
 *       page builders so a partially-custom tab matches the host
 *       look-and-feel without reimplementing it.
 */
#define PN_PLUGIN_ABI_VERSION 2

/**
 * PN_PLUGIN_INIT_SYMBOL:
 *
 * Name of the C symbol the host looks up in every plugin .so.  Kept
 * as a macro (rather than hard-coded in two places) so a future
 * rename only needs to touch this header.
 */
#define PN_PLUGIN_INIT_SYMBOL "pn_plugin_init"

/**
 * PnPluginInfo:
 * @abi_version: must equal #PN_PLUGIN_ABI_VERSION; the host refuses
 *               the plugin otherwise.
 * @name:        short identifier shown in the inspector / palette
 *               attribution column ("EchoPlugin", "AcmeCrypto", …).
 *               This is the value the host copies into
 *               #PnNodeClass.plugin_name for every type the plugin
 *               registers, unless the plugin's own _class_init
 *               already pinned a more specific value.  Required.
 * @version:    free-form version string ("1.0.0", "2025-04-12").
 *               Required; surfaced to the user in error messages
 *               and the optional "loaded plugins" inspector pane.
 * @description: one-sentence human-readable description.  Optional.
 *
 * Identity record returned by every plugin's #pn_plugin_init.  The
 * host treats every field as a borrowed pointer with static lifetime;
 * the plugin must keep them alive for the duration of the process,
 * which in practice means returning a pointer to a static const
 * struct holding string literals.
 */
typedef struct
{
    int          abi_version;
    const gchar *name;
    const gchar *version;
    const gchar *description;
} PnPluginInfo;

/**
 * PnPluginInitFunc:
 * @factory: the process-wide #PnNodeFactory the plugin should
 *           register its node types with.
 *
 * Signature of the entry point (#PN_PLUGIN_INIT_SYMBOL) every plugin
 * must export.  Implementations call pn_node_factory_register() for
 * each #PnNode subclass they provide and return a borrowed pointer
 * to a static #PnPluginInfo describing themselves.  Returning %NULL
 * is treated as a failed load and causes the host to unregister the
 * .so without pinning it in memory.
 */
typedef const PnPluginInfo *(*PnPluginInitFunc) (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_PLUGIN_H */
