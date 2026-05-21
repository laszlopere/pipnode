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

#ifndef PN_EDGE_H
#define PN_EDGE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnEdge                                                             */
/*                                                                     */
/*  Edge-detector filter.  Inspects the boolean "success" field of    */
/*  every incoming #PnMessage and forwards only those messages that  */
/*  change the value relative to the previously seen one.  Messages   */
/*  whose "success" member is missing or not a boolean are dropped   */
/*  without updating state, and the very first observed message is   */
/*  never forwarded since there is nothing to compare against.        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_EDGE (pn_edge_get_type ())

G_DECLARE_FINAL_TYPE (PnEdge, pn_edge, PN, EDGE, PnNode)

PnEdge *pn_edge_new (void);

G_END_DECLS

#endif /* PN_EDGE_H */
