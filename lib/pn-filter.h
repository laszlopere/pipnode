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

#ifndef PN_FILTER_H
#define PN_FILTER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFilter                                                           */
/*                                                                     */
/*  Generic gating node: forwards a message iff every configured       */
/*  rule matches the corresponding member of the message's data bag.   */
/*  Rules are ANDed; an empty rule list is vacuously true and lets     */
/*  every message through.                                             */
/*                                                                     */
/*  The rule list is exposed as a single string GObject property       */
/*  ("rules") holding a JSON array of                                  */
/*  {"path": ..., "op": ..., "literal": ...} objects.  Persisting it   */
/*  through a single string keeps it round-tripping cleanly through    */
/*  the worksheet save/load path which only knows scalar GValue types. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FILTER (pn_filter_get_type ())

G_DECLARE_FINAL_TYPE (PnFilter, pn_filter, PN, FILTER, PnNode)

PnFilter *pn_filter_new (void);

G_END_DECLS

#endif /* PN_FILTER_H */
