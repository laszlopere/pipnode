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

#include "pn-wire-store.h"

struct _PnWireStore
{
    GObject    parent_instance;

    /** Strong references to the contained wires. */
    GPtrArray *wires;
};

G_DEFINE_TYPE (PnWireStore, pn_wire_store, G_TYPE_OBJECT)

enum {
    SIG_WIRE_ADDED,
    SIG_WIRE_REMOVED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_wire_store_finalize (GObject *object)
{
    PnWireStore *self = PN_WIRE_STORE (object);

    g_clear_pointer (&self->wires, g_ptr_array_unref);

    G_OBJECT_CLASS (pn_wire_store_parent_class)->finalize (object);
}

static void
pn_wire_store_class_init (PnWireStoreClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_wire_store_finalize;

    signals[SIG_WIRE_ADDED] = g_signal_new (
            "wire-added",
            PN_TYPE_WIRE_STORE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            2,
            PN_TYPE_WIRE,
            G_TYPE_UINT);

    signals[SIG_WIRE_REMOVED] = g_signal_new (
            "wire-removed",
            PN_TYPE_WIRE_STORE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            2,
            PN_TYPE_WIRE,
            G_TYPE_UINT);
}

static void
pn_wire_store_init (PnWireStore *self)
{
    self->wires = g_ptr_array_new_with_free_func (g_object_unref);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWireStore *
pn_wire_store_new (void)
{
    return g_object_new (PN_TYPE_WIRE_STORE, NULL);
}

void
pn_wire_store_add (
        PnWireStore *self,
        PnWire      *wire)
{
    guint index;

    g_return_if_fail (PN_IS_WIRE_STORE (self));
    g_return_if_fail (PN_IS_WIRE (wire));

    g_ptr_array_add (self->wires, g_object_ref (wire));
    index = self->wires->len - 1;

    g_signal_emit (self, signals[SIG_WIRE_ADDED], 0, wire, index);
}

gboolean
pn_wire_store_remove (
        PnWireStore *self,
        PnWire      *wire)
{
    guint i;

    g_return_val_if_fail (PN_IS_WIRE_STORE (self), FALSE);
    g_return_val_if_fail (PN_IS_WIRE (wire), FALSE);

    for (i = 0; i < self->wires->len; i++)
    {
        if (self->wires->pdata[i] == wire)
        {
            /* Hold a temporary ref so handlers see a live wire even
             * if the array's was the last reference. */
            g_object_ref (wire);
            g_ptr_array_remove_index (self->wires, i);
            pn_wire_disconnect (wire);
            g_signal_emit (self, signals[SIG_WIRE_REMOVED], 0, wire, i);
            g_object_unref (wire);
            return TRUE;
        }
    }

    return FALSE;
}

void
pn_wire_store_clear (PnWireStore *self)
{
    g_return_if_fail (PN_IS_WIRE_STORE (self));

    while (self->wires->len > 0)
    {
        guint   i    = self->wires->len - 1;
        PnWire *wire = g_object_ref (self->wires->pdata[i]);

        g_ptr_array_remove_index (self->wires, i);
        pn_wire_disconnect (wire);
        g_signal_emit (self, signals[SIG_WIRE_REMOVED], 0, wire, i);
        g_object_unref (wire);
    }
}

guint
pn_wire_store_get_length (PnWireStore *self)
{
    g_return_val_if_fail (PN_IS_WIRE_STORE (self), 0);
    return self->wires->len;
}

PnWire *
pn_wire_store_get_wire (
        PnWireStore *self,
        guint        index)
{
    g_return_val_if_fail (PN_IS_WIRE_STORE (self), NULL);

    if (index >= self->wires->len)
        return NULL;

    return self->wires->pdata[index];
}
