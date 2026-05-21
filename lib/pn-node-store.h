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

#ifndef PN_NODE_STORE_H
#define PN_NODE_STORE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNodeStore                                                        */
/*                                                                     */
/*  Container that owns the worksheet's nodes.  Wraps a #GPtrArray of  */
/*  #PnNode instances, takes a reference per node, and emits signals   */
/*  when the contents change so views can repaint without polling.    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NODE_STORE (pn_node_store_get_type ())

G_DECLARE_FINAL_TYPE (PnNodeStore, pn_node_store, PN, NODE_STORE, GObject)

PnNodeStore *pn_node_store_new        (void);

/**
 * pn_node_store_add:
 * @self: the store
 * @node: (transfer none): node to insert at the end
 *
 * Appends @node, taking a reference.  Emits "node-added".
 */
void         pn_node_store_add        (PnNodeStore *self, PnNode *node);

/**
 * pn_node_store_remove:
 * @self: the store
 * @node: (transfer none): node to drop
 *
 * Removes the first occurrence of @node and releases the reference
 * the store held.  Emits "node-removed".  Returns %TRUE on hit.
 */
gboolean     pn_node_store_remove     (PnNodeStore *self, PnNode *node);

/**
 * pn_node_store_clear:
 *
 * Removes all nodes.  Emits one "node-removed" per dropped entry.
 */
void         pn_node_store_clear      (PnNodeStore *self);

guint        pn_node_store_get_length (PnNodeStore *self);

/**
 * pn_node_store_get_node:
 *
 * Returns: (transfer none) (nullable): borrowed pointer to the node
 *   at @index, or %NULL if out of range.
 */
PnNode      *pn_node_store_get_node   (PnNodeStore *self, guint index);

G_END_DECLS

#endif /* PN_NODE_STORE_H */
