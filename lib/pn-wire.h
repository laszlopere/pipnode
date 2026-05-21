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

#ifndef PN_WIRE_H
#define PN_WIRE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWire                                                             */
/*                                                                     */
/*  A single connection from one node's output to another node's       */
/*  input.  While the source and target are both set the wire         */
/*  actively routes messages: it listens for the source's "message"   */
/*  signal and forwards each emission into the target via             */
/*  pn_node_receive_message().                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WIRE (pn_wire_get_type ())

G_DECLARE_FINAL_TYPE (PnWire, pn_wire, PN, WIRE, GObject)

/**
 * pn_wire_new:
 * @source: (nullable) (transfer none): emitting node
 * @target: (nullable) (transfer none): receiving node
 */
PnWire  *pn_wire_new        (PnNode *source, PnNode *target);

/**
 * pn_wire_new_full:
 * @source:       (nullable) (transfer none): emitting node
 * @target:       (nullable) (transfer none): receiving node
 * @target_input: index of the target input port the wire feeds
 *
 * Like pn_wire_new() but addresses a specific input port on @target,
 * for wiring to a multi-input node.  pn_wire_new() is the @target_input
 * == 0 special case.
 */
PnWire  *pn_wire_new_full   (PnNode *source, PnNode *target,
                             gint target_input);

PnNode  *pn_wire_get_source (PnWire *self);
void     pn_wire_set_source (PnWire *self, PnNode *source);

PnNode  *pn_wire_get_target (PnWire *self);
void     pn_wire_set_target (PnWire *self, PnNode *target);

/**
 * pn_wire_get_target_input:
 *
 * Index of the target input port this wire delivers to (0 for an
 * ordinary single-input target).
 */
gint     pn_wire_get_target_input (PnWire *self);
void     pn_wire_set_target_input (PnWire *self, gint target_input);

/**
 * pn_wire_disconnect:
 *
 * Sever both endpoints so the wire stops routing immediately.  Used
 * when the wire is removed from its store: external refs (menus,
 * signal arguments) may keep the object alive for a while afterwards,
 * but it must not keep forwarding messages once it has been removed.
 */
void     pn_wire_disconnect (PnWire *self);

G_END_DECLS

#endif /* PN_WIRE_H */
