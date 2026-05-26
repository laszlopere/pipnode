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
 *   3 — split a plugin's GTK-touching code into an optional companion
 *       GUI module (TODO #23, Phase 6).  The logic .so stays GTK-free
 *       and exports only #pn_plugin_init; a sibling
 *       <literal>&lt;name&gt;-gui.&lt;suffix&gt;</literal> (see
 *       #PN_PLUGIN_GUI_INFIX) exports #pn_plugin_gui_init, which the
 *       editor loads after the logic .so to install the drawing /
 *       dialog vfunc slots onto the already-registered classes.  The
 *       headless host loads only the logic .so, so a server-installable
 *       plugin pulls no GTK.  No #PnNode / #PnNodeClass layout change —
 *       the bump exists so a host and a plugin pair agree on the
 *       companion contract (a v2 monolithic plugin that set the dialog
 *       vfuncs from #pn_plugin_init still works, but is rebuilt against
 *       v3 to load).
 *   4 — appended one optional rendering vfunc to #PnNodeClass,
 *       #get_client_area, reporting the node's body rectangle below the
 *       header (a table grid, a graph plot) so the worksheet's palette
 *       drag-preview can outline the full footprint a drop would occupy.
 *       Appended at the end of the vtable, so existing field offsets are
 *       unchanged; the bump exists only so a stale plugin built against
 *       the smaller v3 #PnNodeClass is rebuilt rather than loaded against
 *       the grown class.
 *   5 — host-provisioned profiles (credentials, settings, permissions).
 *       The factory registration contract grew a companion to
 *       pn_node_factory_register() — pn_node_factory_register_profile_type()
 *       — through which a plugin declares the credential / settings /
 *       permission *types* it needs (a #PnProfileSchema).  The host stores
 *       provisioned instances in the #PnVault (a 0600 file outside the
 *       workflow), a node references one through a property tagged with
 *       pn_param_spec_set_profile_ref(), and resolves it at runtime with
 *       pn_node_get_profile().  No #PnNode / #PnNodeClass layout change —
 *       the bump exists so a plugin that declares profile types is rebuilt
 *       against a host that understands them rather than silently registering
 *       none against an older host.
 *   6 — appended one optional vfunc to #PnWebsocketClass, #profile_resolved,
 *       so a subclass that calls pn_ws_bind_profile() can drive its
 *       connection target off a host-provisioned credentials profile (the
 *       default impl assigns the profile's "url" field to #PnWebsocket:url;
 *       overrides also pull secrets or compose multi-field endpoints).
 *       Appended at the end of the class struct so existing field offsets
 *       are unchanged; the bump exists so a stale plugin built against the
 *       smaller v5 #PnWebsocketClass is rebuilt rather than loaded against
 *       the grown class.
 */
#define PN_PLUGIN_ABI_VERSION 6

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

/* ------------------------------------------------------------------ */
/*  Companion GUI module (ABI v3, TODO #23 Phase 6)                     */
/*                                                                     */
/*  A plugin that wants bespoke drawing or a custom settings dialog    */
/*  splits in two, exactly the way the in-tree dual-nature nodes do:   */
/*                                                                     */
/*    - the logic <name>.so links only the GTK-free core, exports      */
/*      pn_plugin_init(), and registers its #PnNode subclasses;        */
/*    - a companion <name>-gui.<suffix> (the same basename with the    */
/*      PN_PLUGIN_GUI_INFIX inserted before the module suffix) links   */
/*      the GUI tier, exports pn_plugin_gui_init(), and installs the    */
/*      cairo painter / settings-dialog vfunc slots onto the classes   */
/*      the logic .so already registered.                              */
/*                                                                     */
/*  The headless host (pipnode-run) loads only the logic .so, so a     */
/*  server-installable plugin pulls no GTK.  The editor loads the      */
/*  logic .so first and then, for each loaded plugin, looks for the    */
/*  sibling companion and calls its pn_plugin_gui_init().  A missing   */
/*  companion is not an error — the node simply renders with the host  */
/*  default painter and the auto-generated settings tab.               */
/*                                                                     */
/*  The companion's pn_plugin_gui_init() typically does, per type:      */
/*                                                                     */
/*  ```c                                                               */
/*  G_MODULE_EXPORT const PnPluginInfo *                               */
/*  pn_plugin_gui_init (PnNodeFactory *factory)                        */
/*  {                                                                  */
/*      static const PnPluginInfo info = {                             */
/*          .abi_version = PN_PLUGIN_ABI_VERSION,                      */
/*          .name        = "AcmeCrypto (GUI)",                         */
/*          .version     = "1.0.0",                                    */
/*      };                                                             */
/*      GType t = g_type_from_name ("MyGauge");                        */
/*      PnNodeClass *klass = PN_NODE_CLASS (g_type_class_ref (t));     */
/*      klass->paint_header_overlay  = my_gauge_paint;                 */
/*      klass->build_property_editor = my_gauge_build_editor;          */
/*      // class ref intentionally held for the process lifetime       */
/*      return &info;                                                  */
/*  }                                                                  */
/*  ```                                                                */
/*                                                                     */
/*  IMPORTANT — resolve the type by NAME, not by linking the logic      */
/*  header's MY_TYPE_GAUGE / my_gauge_get_type().  The host loads the   */
/*  logic .so with G_MODULE_BIND_LOCAL, so its symbols (the type        */
/*  getter, any read-seam accessors) are not visible to a separately-   */
/*  dlopened companion; a header reference would fail with "undefined   */
/*  symbol" at companion-load time.  g_type_from_name() needs no        */
/*  symbol — the type is already in the global GType registry once the  */
/*  logic half ran.  A companion therefore drives its node through      */
/*  public API only: GObject properties (g_object_get/bind) and the     */
/*  PnNode vfunc table.  State a painter needs that is not already a    */
/*  readable property should be exposed as one by the logic half (which */
/*  is also how the headless side stays inspectable).                  */
/*                                                                     */
/*  The @factory argument is the same process-wide factory the logic   */
/*  half registered against, passed for symmetry and future use.        */
/* ------------------------------------------------------------------ */

/**
 * PN_PLUGIN_GUI_INIT_SYMBOL:
 *
 * Name of the C symbol the editor looks up in a plugin's companion
 * GUI module.  Only the companion <name>-gui.<suffix> exports it; the
 * logic .so exports #PN_PLUGIN_INIT_SYMBOL alone.
 */
#define PN_PLUGIN_GUI_INIT_SYMBOL "pn_plugin_gui_init"

/**
 * PN_PLUGIN_GUI_INFIX:
 *
 * The fragment inserted before the module suffix to name a plugin's
 * companion GUI module: a logic plugin <literal>foo.so</literal> has
 * companion <literal>foo-gui.so</literal>.  The core loader uses this
 * to skip companions during a logic-plugin directory scan, and the
 * editor's GUI loader uses it to derive the companion path from each
 * loaded logic plugin.  A logic plugin must therefore not itself be
 * named with this infix.
 */
#define PN_PLUGIN_GUI_INFIX "-gui"

/**
 * PnPluginGuiInitFunc:
 * @factory: the same process-wide #PnNodeFactory the logic half
 *           registered its node types with.
 *
 * Signature of the companion entry point (#PN_PLUGIN_GUI_INIT_SYMBOL).
 * Implementations install the GUI-tier vfunc slots (cairo painters,
 * settings-dialog customisations) onto the already-registered classes
 * and return a borrowed pointer to a static #PnPluginInfo describing
 * the companion.  The editor checks the returned @abi_version against
 * #PN_PLUGIN_ABI_VERSION and refuses the companion on mismatch;
 * returning %NULL is treated as a failed companion load.  As with the
 * logic entry point the host pins the module resident on success.
 */
typedef const PnPluginInfo *(*PnPluginGuiInitFunc) (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_PLUGIN_H */
