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

#ifndef PN_NODE_DIALOG_HELPERS_H
#define PN_NODE_DIALOG_HELPERS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNodeDialog helper API                                            */
/*                                                                     */
/*  Public surface a #PnNodeClass override                             */
/*  (#PnNodeClass.build_property_editor / #PnNodeClass.build_class_tab */
/*  / #PnNodeClass.build_extra_pages) can call when assembling the     */
/*  widgets it contributes to the settings dialog.  Pulled out into    */
/*  a dedicated header so plugin-shipped node types — whose .so        */
/*  links against libpipnode the same way the host does — can match    */
/*  the host's grid spacing / margins / default editor look without    */
/*  duplicating the logic, and so the host's own subclass migrations  */
/*  in libpipnode go through the same surface a third-party plugin    */
/*  would.                                                             */
/*                                                                     */
/*  All four entry points are safe to call at any point during a      */
/*  build_property_editor / build_class_tab / build_extra_pages       */
/*  call: they construct widgets only, do not block on external      */
/*  resources, and do not retain pointers to their inputs beyond the */
/*  call.                                                              */
/* ------------------------------------------------------------------ */

/**
 * pn_node_dialog_new_property_grid:
 *
 * Construct a fresh, empty #GtkGrid pre-configured with the host's
 * standard column / row spacing and outer margins — the same shape
 * the introspection-driven default class tab uses.  Suitable as the
 * top-level container of a #PnNodeClass.build_class_tab override
 * that wants its custom rows to line up vertically with the
 * auto-generated tabs of sibling classes in the type chain.
 *
 * Returns: (transfer floating): a new floating #GtkGrid
 */
GtkWidget *pn_node_dialog_new_property_grid (void);

/**
 * pn_node_dialog_attach_row:
 * @grid:    a grid built with pn_node_dialog_new_property_grid()
 *           (or any #GtkGrid laid out with the same two-column
 *           "label : editor" convention)
 * @row:     zero-based row index to attach at
 * @label:   left-column text for the row (typically the property
 *           nick, but free-form for custom rows)
 * @editor:  editor widget for the right column; #GtkWidget.hexpand
 *           is set to %TRUE on the editor so multi-character entries
 *           consume the remaining horizontal space the way the
 *           auto-generated rows do
 *
 * Attach the standard "label : editor" pair at @row.  Mirrors what
 * the dialog's own row builder does for an auto-generated row, so
 * a partially-custom tab (e.g. one row replaced by a custom widget,
 * the rest delegated back to pn_node_dialog_default_editor()) ends
 * up visually indistinguishable from a fully-default tab.
 */
void       pn_node_dialog_attach_row        (GtkGrid     *grid,
                                             guint        row,
                                             const gchar *label,
                                             GtkWidget   *editor);

/**
 * pn_node_dialog_default_editor:
 * @target: the #GObject the editor must bind to (typically the node
 *          itself, cast to #GObject)
 * @pspec:  the property the editor row is being built for
 *
 * Build the host's default editor widget for @pspec — the same widget
 * the introspection-driven auto-generated tab would attach for the
 * row.  Picks based on the property's #GType: a #GtkEntry for plain
 * strings (or a #GtkTextView in a scrolled window when the pspec
 * carries the pn_param_spec_set_multiline() hint), a #GtkCheckButton
 * for booleans, a #GtkColorButton for #GdkRGBA, a #GtkComboBoxText
 * for enums, a #GtkSpinButton pair for #PnPoint, a #GtkSpinButton for
 * numerics, and a read-only #GtkLabel as a final fallback.  Read-only
 * properties (#G_PARAM_WRITABLE not set) come back as a disabled
 * editor of the same shape.
 *
 * Used from #PnNodeClass.build_property_editor when the override
 * wants to delegate certain properties back to the host and only
 * customise the rest, and from #PnNodeClass.build_class_tab when the
 * custom tab is mostly hand-rolled but needs a few rows to look like
 * what the user is used to.
 *
 * Returns: (transfer floating): the editor widget, already bound to
 *   @target via the standard GObject machinery
 */
GtkWidget *pn_node_dialog_default_editor    (GObject    *target,
                                             GParamSpec *pspec);

/**
 * pn_node_dialog_append_page:
 * @notebook: the dialog's #GtkNotebook (the @notebook argument to
 *            #PnNodeClass.build_extra_pages)
 * @page:     the widget to host on the new tab
 * @title:    plain-text title for the tab label
 *
 * Convenience wrapper around gtk_notebook_append_page() that wraps
 * @title in a fresh #GtkLabel — the same label widget the host uses
 * for its auto-generated tabs, so a plugin-contributed extra page
 * blends in.  Equivalent to:
 *
 * |[
 *     gtk_notebook_append_page (notebook, page, gtk_label_new (title));
 * ]|
 *
 * but spelled the same way the in-tree subclass migrations do for
 * grep-friendliness.
 */
void       pn_node_dialog_append_page       (GtkNotebook *notebook,
                                             GtkWidget   *page,
                                             const gchar *title);

G_END_DECLS

#endif /* PN_NODE_DIALOG_HELPERS_H */
