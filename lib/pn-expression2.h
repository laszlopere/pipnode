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

#ifndef PN_EXPRESSION2_H
#define PN_EXPRESSION2_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnExpression2                                                      */
/*                                                                     */
/*  Two-input variant of #PnExpression.  It evaluates an algebraic     */
/*  expression over the numeric members of the latest message seen on  */
/*  each of its two inputs.  Members from input 1 are bound with a     */
/*  "1" suffix and members from input 2 with a "2" suffix, so the two  */
/*  headline values are addressable as `value1` / `value2` and any     */
/*  sibling numeric field as e.g. `temp1` / `temp2`.  The result is    */
/*  written to data.value of the just-arrived message before it is     */
/*  forwarded; the node recomputes on every incoming message using the */
/*  most recent values held for both inputs.                           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_EXPRESSION2 (pn_expression2_get_type ())

G_DECLARE_FINAL_TYPE (PnExpression2, pn_expression2, PN, EXPRESSION2, PnNode)

PnExpression2 *pn_expression2_new (void);

G_END_DECLS

#endif /* PN_EXPRESSION2_H */
