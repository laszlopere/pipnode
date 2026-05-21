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

#ifndef PN_FLOW_H
#define PN_FLOW_H

#include <glib-object.h>

#include "pn-node-store.h"
#include "pn-wire-store.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFlow                                                             */
/*                                                                     */
/*  Headless model for a worksheet: owns the node and wire stores,    */
/*  knows how to load and save itself as JSON, and re-emits           */
/*  status-message text from the debug nodes it contains.  Carries no */
/*  GTK dependency, so a CLI front-end can drive it without a UI.    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FLOW (pn_flow_get_type ())

G_DECLARE_FINAL_TYPE (PnFlow, pn_flow, PN, FLOW, GObject)

/**
 * pn_flow_new:
 *
 * Creates a new, empty flow.
 */
PnFlow      *pn_flow_new            (void);

/**
 * pn_flow_get_nodes:
 *
 * Returns: (transfer none): the flow's #PnNodeStore.  The flow keeps
 *   ownership; callers must not unref.
 */
PnNodeStore *pn_flow_get_nodes      (PnFlow *self);

/**
 * pn_flow_get_wires:
 *
 * Returns: (transfer none): the flow's #PnWireStore.
 */
PnWireStore *pn_flow_get_wires      (PnFlow *self);

/**
 * pn_flow_clear:
 *
 * Removes every node and wire, leaving the flow empty.
 */
void         pn_flow_clear          (PnFlow *self);

/**
 * pn_flow_add_node:
 * @self: the flow
 * @node: (transfer none): node to append
 *
 * Appends @node to the flow.  If the node has no name yet, assigns
 * one of the form "<class> <N>" with the lowest free N, so saved
 * worksheets always have unique connection labels.
 */
void         pn_flow_add_node       (PnFlow *self, PnNode *node);

/**
 * pn_flow_load_from_file:
 * @self: the flow
 * @path: filesystem path of a JSON worksheet
 * @error: (out) (optional): location for a #GError
 *
 * Replaces the current contents with the JSON document at @path.
 * Returns %TRUE on success.  On failure the flow is left untouched.
 */
gboolean     pn_flow_load_from_file (PnFlow      *self,
                                     const gchar *path,
                                     GError     **error);

/**
 * pn_flow_save_to_file:
 * @self: the flow
 * @path: filesystem path to write to
 * @error: (out) (optional): location for a #GError
 *
 * Serialises the flow to JSON and writes it to @path.
 */
gboolean     pn_flow_save_to_file   (PnFlow      *self,
                                     const gchar *path,
                                     GError     **error);

/**
 * pn_flow_serialize_nodes:
 * @self: the flow
 * @subset: (element-type PnNode) (nullable): list of nodes to include;
 *   when %NULL every node in the flow is serialised
 *
 * Builds a JSON document with the same shape as the on-disk worksheet
 * format but tagged %"pipnode-clipboard", carrying the requested
 * @subset (or all of @self when @subset is %NULL) plus only those
 * wires whose source AND target are both in the subset.  Intended for
 * use as a clipboard payload.
 *
 * Returns: (transfer full): a freshly-allocated JSON string; the
 *   caller frees it with g_free().
 */
gchar       *pn_flow_serialize_nodes (PnFlow *self, GList *subset);

/**
 * pn_flow_get_sheets:
 * @self: the flow
 * @out_n: (out) (optional): location to store the number of sheets
 *
 * Returns: (transfer none) (array length=out_n): the ordered list of
 *   sheet names (UTF-8 strings) carried by this flow.  The flow keeps
 *   ownership.  The list is always non-empty: a freshly-constructed or
 *   freshly-cleared flow contains a single sheet named %"Worksheet".
 */
const gchar *const *pn_flow_get_sheets (PnFlow *self, guint *out_n);

/**
 * pn_flow_get_active_sheet:
 *
 * Returns: (transfer none) (not nullable): the name of the sheet
 *   currently designated as active.  Always a member of the
 *   pn_flow_get_sheets() array.
 */
const gchar *pn_flow_get_active_sheet (PnFlow *self);

/**
 * pn_flow_set_active_sheet:
 * @self: the flow
 * @name: (not nullable): name of the sheet to mark active
 *
 * No-op when @name is not a known sheet, or already active.  Emits
 * "active-sheet-changed" on a real transition.  Does NOT mark the flow
 * modified — the active-sheet pointer is purely a UX convenience.
 */
void pn_flow_set_active_sheet (PnFlow *self, const gchar *name);

/**
 * pn_flow_add_sheet:
 * @self: the flow
 * @name: (nullable): proposed name; when %NULL or empty a default of
 *   "Sheet N" with the lowest free N is used
 *
 * Appends a new sheet to the tab order.  Returns %FALSE when @name
 * collides with an existing sheet (and the flow is left untouched).
 * Marks the flow modified and emits "sheet-added".
 */
gboolean pn_flow_add_sheet (PnFlow *self, const gchar *name);

/**
 * pn_flow_remove_sheet:
 * @self: the flow
 * @name: name of the sheet to drop
 *
 * Removes every node tagged with @name (which transitively removes any
 * incident wires via the existing node-store signals) and then drops
 * the sheet entry itself.  Returns %FALSE when @name is the only sheet
 * left or is not a known sheet — a flow always carries at least one
 * sheet.  When the removed sheet was active the active sheet shifts to
 * the neighbour on its right (or left, when removing the last one).
 * Marks the flow modified and emits "sheet-removed".
 */
gboolean pn_flow_remove_sheet (PnFlow *self, const gchar *name);

/**
 * pn_flow_rename_sheet:
 * @self: the flow
 * @from: current sheet name
 * @to: replacement name (must not be empty)
 *
 * Renames the sheet and rewrites every node whose worksheet property
 * equals @from to point at @to.  Returns %FALSE when @to collides with
 * a different existing sheet, when @from is not a known sheet, or when
 * @to is %NULL or empty.  Marks the flow modified and emits
 * "sheet-renamed".
 */
gboolean pn_flow_rename_sheet (PnFlow      *self,
                               const gchar *from,
                               const gchar *to);

/**
 * pn_flow_reorder_sheet:
 * @self: the flow
 * @name: sheet to move
 * @new_index: zero-based destination position; clamped to the valid
 *   range
 *
 * Re-positions @name within the tab order without changing any node's
 * worksheet property.  Marks the flow modified and emits
 * "sheets-reordered" when the index actually changes.
 */
void pn_flow_reorder_sheet (PnFlow      *self,
                            const gchar *name,
                            guint        new_index);

/**
 * pn_flow_is_modified:
 *
 * Returns: %TRUE iff the flow has user-visible changes that have not
 *   yet been written back to disk.  Hosts subscribe to the
 *   "modified-changed" signal to track the flag live.  A freshly
 *   constructed, freshly cleared, or freshly loaded/saved flow reports
 *   %FALSE.
 */
gboolean pn_flow_is_modified (PnFlow *self);

/**
 * pn_flow_set_modified:
 *
 * Sets the modified flag explicitly.  Hosts call this with %TRUE after
 * any structural edit they perform on the stores; pn_flow_save_to_file
 * and pn_flow_load_from_file already flip it back to %FALSE on
 * success.  Emits "modified-changed" on a real transition.
 */
void pn_flow_set_modified (PnFlow *self, gboolean modified);

/**
 * pn_flow_paste_from_string:
 * @self: the flow
 * @json: a JSON document produced by pn_flow_serialize_nodes() or
 *   pn_flow_save_to_file()
 * @dx: horizontal offset added to every pasted node's position
 * @dy: vertical offset added to every pasted node's position
 * @error: (out) (optional): location for a #GError
 *
 * Adds the nodes and wires described by @json to @self without
 * disturbing the flow's existing contents.  Pasted nodes whose name
 * collides with a live one are renamed to "<original> (copy)" or
 * "<original> (copy N)" so every label stays unique; wires are
 * remapped to the new pointers via their original source/target
 * names.
 *
 * Returns: (transfer container) (element-type PnNode): a list of
 *   borrowed pointers to the newly-pasted nodes (in JSON order); the
 *   caller frees the list shell with g_list_free().  Returns %NULL
 *   with @error set on parse failure.
 */
GList       *pn_flow_paste_from_string (PnFlow      *self,
                                        const gchar *json,
                                        double       dx,
                                        double       dy,
                                        GError     **error);

G_END_DECLS

#endif /* PN_FLOW_H */
