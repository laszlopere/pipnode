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

#ifndef PN_WIRE_STORE_H
#define PN_WIRE_STORE_H

#include "pn-wire.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWireStore                                                        */
/*                                                                     */
/*  Container that owns the worksheet's wires.  Mirrors PnNodeStore:  */
/*  refs each entry, exposes typed add/remove/clear/length/get, and   */
/*  emits "wire-added"/"wire-removed" signals on changes.             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WIRE_STORE (pn_wire_store_get_type ())

G_DECLARE_FINAL_TYPE (PnWireStore, pn_wire_store, PN, WIRE_STORE, GObject)

PnWireStore *pn_wire_store_new        (void);

/**
 * pn_wire_store_add:
 * @self: the store
 * @wire: (transfer none): wire to insert at the end
 *
 * Appends @wire, taking a reference.  Emits "wire-added".
 */
void         pn_wire_store_add        (PnWireStore *self, PnWire *wire);

/**
 * pn_wire_store_remove:
 * @self: the store
 * @wire: (transfer none): wire to drop
 *
 * Removes the first occurrence of @wire and releases the store's
 * reference.  Emits "wire-removed".  Returns %TRUE on hit.
 */
gboolean     pn_wire_store_remove     (PnWireStore *self, PnWire *wire);

void         pn_wire_store_clear      (PnWireStore *self);

guint        pn_wire_store_get_length (PnWireStore *self);

/**
 * Returns: (transfer none) (nullable): borrowed pointer to the wire
 *   at @index, or %NULL if out of range.
 */
PnWire      *pn_wire_store_get_wire   (PnWireStore *self, guint index);

G_END_DECLS

#endif /* PN_WIRE_STORE_H */
