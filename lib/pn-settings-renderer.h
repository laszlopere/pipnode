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

#ifndef PN_SETTINGS_RENDERER_H
#define PN_SETTINGS_RENDERER_H

#include <gtk/gtk.h>

#include "pn-node.h"
#include "pn-settings-schema.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSettingsSchema → GtkWidget renderer (GUI tier)                    */
/*                                                                     */
/*  The GUI half of the Phase-7 declarative settings schema: it turns  */
/*  the GTK-free #PnSettingsSchema data held by a node's class into the */
/*  same GtkWidgets the introspection-driven #PnNodeDialog builds by    */
/*  hand, so a node or plugin that only needs the common dialog         */
/*  customisations (tab grouping, an explicit editor kind, a choice     */
/*  list, a row greyed out by another property) never has to link GTK   */
/*  or ship a `-gui.so` companion.                                      */
/*                                                                     */
/*  Two entry points, both designed to drop into #PnNodeDialog's        */
/*  populate_notebook() in Phase 7.3 with the documented precedence     */
/*  (build_class_tabs > build_class_tab > schema-tabs > auto; and       */
/*  build_property_editor > schema-row > default):                      */
/*                                                                     */
/*    - pn_settings_schema_render_class() emits one notebook page per   */
/*      named tab — the declarative counterpart of                      */
/*      #PnNodeClass.build_class_tabs.                                  */
/*                                                                     */
/*    - pn_settings_render_editor() builds (and binds) the editor for   */
/*      a single property, honouring the schema row's editor kind,      */
/*      choice list, layout flags and conditional sensitivity, and      */
/*      delegating to pn_node_dialog_default_editor() for an            */
/*      %PN_EDITOR_AUTO row or a property the schema does not mention.  */
/*                                                                     */
/*  Both are no-ops on a node whose class declares no schema, so        */
/*  inserting the calls leaves every existing dialog byte-identical     */
/*  until a type opts in (Phase 7.4+).                                  */
/* ------------------------------------------------------------------ */

/**
 * pn_settings_schema_render_class:
 * @schema:   the schema attached to @node's class
 * @node:     the node being edited; editors bind to it bidirectionally
 * @notebook: the dialog notebook to append pages to
 * @parent:   (nullable): the dialog window, threaded through for any
 *            child dialog an editor spawns (file chooser, …)
 *
 * Append one page per named tab in @schema to @notebook, each a property
 * grid laid out like the introspection-driven auto tabs (see
 * pn_node_dialog_new_property_grid()).  Each row is built through
 * pn_settings_render_editor(), so editor kind / choices / flags /
 * conditional sensitivity all apply.  A row whose property does not
 * resolve to a writable, non-construct-only property on @node is
 * silently skipped, exactly as the auto tab skips them.
 *
 * Intended for the schema-tabs precedence slot: call it only when
 * pn_settings_schema_has_tabs() is %TRUE (a schema with no named tab
 * customises individual rows of the auto tab instead, via
 * pn_settings_render_editor()).
 *
 * Returns: the number of pages appended (0 if @schema declares no tab
 *   that yields at least one row — the caller then falls through to the
 *   auto tab).
 */
guint pn_settings_schema_render_class (PnSettingsSchema *schema,
                                       PnNode           *node,
                                       GtkNotebook      *notebook,
                                       GtkWindow        *parent);

/**
 * pn_settings_render_editor:
 * @target: the #GObject the editor binds to (the node, cast to #GObject)
 * @pspec:  the property the row edits
 * @schema: (nullable): the schema to consult for @pspec->name
 *
 * Build the editor widget for @pspec, bound bidirectionally to @target.
 * If @schema declares a row for @pspec->name, the row's editor kind,
 * choice list and layout/sensitivity flags shape the widget; otherwise
 * — and for an explicit %PN_EDITOR_AUTO row — the widget is exactly what
 * pn_node_dialog_default_editor() would build, so the schema only ever
 * *overrides*, never regresses, the type-driven default (the lesson of
 * the Phase-5 colour-editor regression).  When the row carries an
 * enable-when condition the editor's sensitivity is wired to the
 * controller property via g_signal_connect_object(), so it tracks the
 * controller live and auto-disconnects when the editor is destroyed.
 *
 * The returned widget carries the same `pn-prop-<name>` widget name as
 * pn_node_dialog_default_editor()'s, so functional tests locate it the
 * same way regardless of which path built it.
 *
 * Returns: (transfer floating): the editor widget, already bound.
 */
GtkWidget *pn_settings_render_editor (GObject          *target,
                                      GParamSpec       *pspec,
                                      PnSettingsSchema *schema);

G_END_DECLS

#endif /* PN_SETTINGS_RENDERER_H */
