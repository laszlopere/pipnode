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

#include "pn-node-store.h"

struct _PnNodeStore
{
    GObject    parent_instance;

    /** Strong references to the contained nodes. */
    GPtrArray *nodes;
};

G_DEFINE_TYPE (PnNodeStore, pn_node_store, G_TYPE_OBJECT)

enum {
    SIG_NODE_ADDED,
    SIG_NODE_REMOVED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_node_store_finalize (GObject *object)
{
    PnNodeStore *self = PN_NODE_STORE (object);

    g_clear_pointer (&self->nodes, g_ptr_array_unref);

    G_OBJECT_CLASS (pn_node_store_parent_class)->finalize (object);
}

static void
pn_node_store_class_init (PnNodeStoreClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_node_store_finalize;

    /* Emitted after a node has been inserted at the given index. */
    signals[SIG_NODE_ADDED] = g_signal_new (
            "node-added",
            PN_TYPE_NODE_STORE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            2,
            PN_TYPE_NODE,
            G_TYPE_UINT);

    /* Emitted after a node has been removed.  The node pointer is
     * still valid for the duration of the signal handler; the store
     * releases its own reference once all handlers return. */
    signals[SIG_NODE_REMOVED] = g_signal_new (
            "node-removed",
            PN_TYPE_NODE_STORE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            2,
            PN_TYPE_NODE,
            G_TYPE_UINT);
}

static void
pn_node_store_init (PnNodeStore *self)
{
    self->nodes = g_ptr_array_new_with_free_func (g_object_unref);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNodeStore *
pn_node_store_new (void)
{
    return g_object_new (PN_TYPE_NODE_STORE, NULL);
}

void
pn_node_store_add (
        PnNodeStore *self,
        PnNode      *node)
{
    guint index;

    g_return_if_fail (PN_IS_NODE_STORE (self));
    g_return_if_fail (PN_IS_NODE (node));

    g_ptr_array_add (self->nodes, g_object_ref (node));
    index = self->nodes->len - 1;

    g_signal_emit (self, signals[SIG_NODE_ADDED], 0, node, index);
}

gboolean
pn_node_store_remove (
        PnNodeStore *self,
        PnNode      *node)
{
    guint i;

    g_return_val_if_fail (PN_IS_NODE_STORE (self), FALSE);
    g_return_val_if_fail (PN_IS_NODE (node), FALSE);

    for (i = 0; i < self->nodes->len; i++)
    {
        if (self->nodes->pdata[i] == node)
        {
            /* Hold a temporary ref so handlers can safely access the
             * node, then drop the array's ref via g_ptr_array_remove
             * (which calls the free func). */
            g_object_ref (node);
            g_ptr_array_remove_index (self->nodes, i);
            g_signal_emit (self, signals[SIG_NODE_REMOVED], 0, node, i);
            g_object_unref (node);
            return TRUE;
        }
    }

    return FALSE;
}

void
pn_node_store_clear (PnNodeStore *self)
{
    g_return_if_fail (PN_IS_NODE_STORE (self));

    /* Iterate from the tail so the emitted index stays valid for
     * each removal. */
    while (self->nodes->len > 0)
    {
        guint   i    = self->nodes->len - 1;
        PnNode *node = g_object_ref (self->nodes->pdata[i]);

        g_ptr_array_remove_index (self->nodes, i);
        g_signal_emit (self, signals[SIG_NODE_REMOVED], 0, node, i);
        g_object_unref (node);
    }
}

guint
pn_node_store_get_length (PnNodeStore *self)
{
    g_return_val_if_fail (PN_IS_NODE_STORE (self), 0);
    return self->nodes->len;
}

PnNode *
pn_node_store_get_node (
        PnNodeStore *self,
        guint        index)
{
    g_return_val_if_fail (PN_IS_NODE_STORE (self), NULL);

    if (index >= self->nodes->len)
        return NULL;

    return self->nodes->pdata[index];
}
