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

#ifndef PN_SET_H
#define PN_SET_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSet                                                              */
/*                                                                     */
/*  Pass-through transformer: forwards every incoming message after    */
/*  setting (or adding) a configured list of data-bag members on it.   */
/*  The complement to PnFilter — that node decides whether a message   */
/*  goes through, this one decides which members the downstream node   */
/*  sees.                                                              */
/*                                                                     */
/*  The property list is exposed as a single string GObject property   */
/*  ("props") holding a JSON array of {"path": ..., "literal": ...}    */
/*  objects.  Persisting it through a single string keeps it round-    */
/*  tripping cleanly through the worksheet save/load path which only   */
/*  knows scalar GValue types.                                         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SET (pn_set_get_type ())

G_DECLARE_FINAL_TYPE (PnSet, pn_set, PN, SET, PnNode)

PnSet *pn_set_new (void);

G_END_DECLS

#endif /* PN_SET_H */
