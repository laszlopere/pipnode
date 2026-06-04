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

#ifndef PN_WORKSHEET_H
#define PN_WORKSHEET_H

#include <gtk/gtk.h>

#include "pn-flow.h"
#include "pn-node-store.h"
#include "pn-wire-store.h"

G_BEGIN_DECLS

#define PN_TYPE_WORKSHEET (pn_worksheet_get_type ())

G_DECLARE_FINAL_TYPE (PnWorksheet,
                      pn_worksheet,
                      PN, WORKSHEET,
                      GtkDrawingArea)

/**
 * pn_worksheet_new:
 *
 * Creates a new, empty worksheet widget.  The widget is the
 * canvas on which the user will eventually wire visual flow
 * nodes together.  At this point it draws an empty grid only;
 * editing and painting are not yet implemented.
 *
 * Returns: (transfer floating): the new worksheet widget
 */
GtkWidget *pn_worksheet_new (void);

/**
 * pn_worksheet_new_for_flow:
 * @flow: (transfer none) (not nullable): shared #PnFlow that owns the
 *   nodes and wires this worksheet renders.  The widget keeps a
 *   borrowed pointer plus its own reference; the caller still owns
 *   the flow and is responsible for the final unref.
 * @sheet_name: (not nullable): name of the spreadsheet-style sheet
 *   this widget represents.  Only nodes whose %"worksheet" property
 *   equals @sheet_name are drawn, hit-tested, copied, or accept drops
 *   from the palette; new nodes dropped onto this widget inherit the
 *   name automatically.
 *
 * Use this constructor (instead of pn_worksheet_new()) when several
 * widgets share one model — typically one widget per #GtkNotebook
 * page in a multi-sheet UI.  The flow's "modified-changed" signal is
 * the single source of truth for the host's Save UI; per-widget
 * modified state does not exist.
 *
 * Returns: (transfer floating): the new worksheet widget
 */
GtkWidget *pn_worksheet_new_for_flow (PnFlow      *flow,
                                      const gchar *sheet_name);

/**
 * pn_worksheet_get_flow:
 *
 * Returns: (transfer none): the worksheet's #PnFlow.
 */
PnFlow      *pn_worksheet_get_flow  (PnWorksheet *self);

/**
 * pn_worksheet_get_sheet_name:
 *
 * Returns: (transfer none): the sheet name this widget is filtering
 *   on.  Always non-empty.
 */
const gchar *pn_worksheet_get_sheet_name (PnWorksheet *self);

/**
 * pn_worksheet_set_sheet_name:
 * @self: a #PnWorksheet
 * @name: the new sheet name; must not be %NULL or empty
 *
 * Updates the sheet tag this widget filters on and queues a redraw.
 * Used by the multi-sheet host when a sheet rename moves every node
 * to a new sheet name — without re-pointing the widget the canvas
 * would silently empty out, since the filter would no longer match
 * any node.  No-op when @name equals the current value.
 */
void         pn_worksheet_set_sheet_name (PnWorksheet *self,
                                          const gchar *name);

/**
 * pn_worksheet_clear:
 *
 * Removes all nodes and connections from the worksheet, leaving
 * it in the same state as a freshly created one.
 */
void       pn_worksheet_clear (PnWorksheet *self);

/**
 * pn_worksheet_load_from_file:
 * @path: filesystem path of a JSON worksheet
 * @error: (out) (optional): location for a #GError
 *
 * Replaces the current worksheet contents with the JSON document
 * at @path.  Returns %TRUE on success.
 */
gboolean   pn_worksheet_load_from_file (PnWorksheet *self,
                                        const gchar *path,
                                        GError     **error);

/**
 * pn_worksheet_save_to_file:
 * @path: filesystem path to write to
 * @error: (out) (optional): location for a #GError
 *
 * Serialises the worksheet to JSON and writes it to @path.
 * Returns %TRUE on success.
 */
gboolean   pn_worksheet_save_to_file   (PnWorksheet *self,
                                        const gchar *path,
                                        GError     **error);

/**
 * pn_worksheet_get_nodes:
 *
 * Returns: (transfer none): the worksheet's #PnNodeStore.  Useful for
 *   tests and tooling that need to inspect or enumerate nodes; do not
 *   take a reference, as the store is owned by the worksheet.
 */
PnNodeStore *pn_worksheet_get_nodes (PnWorksheet *self);

/**
 * pn_worksheet_get_wires:
 *
 * Returns: (transfer none): the worksheet's #PnWireStore.
 */
PnWireStore *pn_worksheet_get_wires (PnWorksheet *self);

/**
 * pn_worksheet_has_selection:
 *
 * Returns: %TRUE iff at least one node is currently selected.  Used
 *   by the main window to toggle Cut/Copy sensitivity in response to
 *   the "selection-changed" signal.
 */
gboolean   pn_worksheet_has_selection      (PnWorksheet *self);

/**
 * pn_worksheet_copy_selection:
 *
 * Serialises the currently-selected nodes (and only those wires whose
 * endpoints are both selected) to a JSON string and writes it to the
 * default GTK clipboard.  No-op when nothing is selected.
 */
void       pn_worksheet_copy_selection     (PnWorksheet *self);

/**
 * pn_worksheet_cut_selection:
 *
 * Equivalent to pn_worksheet_copy_selection() followed by deleting
 * every selected node.
 */
void       pn_worksheet_cut_selection      (PnWorksheet *self);

/**
 * pn_worksheet_delete_node:
 * @self: a #PnWorksheet
 * @node: a node currently in @self
 *
 * Removes @node from the worksheet together with every wire that
 * references it as a source or a target, so the graph is never left
 * with a dangling endpoint.  This is the single delete path shared by
 * the context-menu Delete, the keyboard delete-selection handler, and
 * the D-Bus DeleteNode automation method (TODO #40.4).
 */
void       pn_worksheet_delete_node        (PnWorksheet *self,
                                            PnNode      *node);

/**
 * pn_worksheet_paste_from_clipboard:
 *
 * Asynchronously fetches text from the default GTK clipboard and, if
 * it parses as a Pipnode JSON document, adds its nodes and wires to
 * the worksheet with a small grid-aligned offset.  The selection is
 * replaced with the newly-pasted nodes on success.
 */
void       pn_worksheet_paste_from_clipboard (PnWorksheet *self);

/**
 * pn_worksheet_focus_node_by_uuid:
 * @self: a #PnWorksheet
 * @uuid: per-node UUID to focus, as it appears in #PnMessage's
 *        envelope `from_id` field (see also #pn_node_get_uuid)
 *
 * Locates the node carrying @uuid, scrolls the enclosing
 * #GtkScrolledWindow so it lands roughly centred in the viewport,
 * and triggers a brief fading outline pulse around the node so the
 * user can see at a glance which node was just referenced.  The
 * current selection is left untouched — the pulse is purely visual
 * feedback.  No-op when @uuid is empty or no node carries it.
 */
void       pn_worksheet_focus_node_by_uuid   (PnWorksheet *self,
                                              const gchar *uuid);

/**
 * pn_worksheet_get_focus_pulse_uuid:
 * @self: a #PnWorksheet
 *
 * Returns: (transfer none) (nullable): UUID of the node currently
 *   carrying a focus pulse, or %NULL when no pulse is active.  The
 *   returned string is owned by the node and remains valid for its
 *   lifetime.  Intended for the DBus test surface.
 */
const gchar *pn_worksheet_get_focus_pulse_uuid (PnWorksheet *self);

/**
 * pn_worksheet_is_modified:
 * @self: a #PnWorksheet
 *
 * Returns: %TRUE iff the worksheet has user-visible changes that
 *   have not yet been written back to disk.  A freshly-created or
 *   freshly-loaded worksheet reports %FALSE; the flag flips to
 *   %TRUE on any structural edit (node or wire added/removed),
 *   drag, disable toggle, or property change driven through
 *   g_object_set, and back to %FALSE the moment a successful save,
 *   load, or pn_worksheet_clear() reaches the file system.
 *
 *   Subscribe to the "modified-changed" signal to track the flag
 *   live — the main window uses it to gate Save toolbar/menu
 *   sensitivity.
 */
gboolean   pn_worksheet_is_modified           (PnWorksheet *self);

G_END_DECLS

#endif /* PN_WORKSHEET_H */
