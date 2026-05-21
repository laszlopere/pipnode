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

#ifndef PN_EXPRESSION_H
#define PN_EXPRESSION_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnExpression                                                       */
/*                                                                     */
/*  Filter node that computes an algebraic expression over the         */
/*  numeric members of each incoming message.  The expression is an    */
/*  editable string property; on every message the node binds the      */
/*  message's numeric data-bag members as variables (so `value` and    */
/*  any other numeric field are addressable by name), evaluates the    */
/*  expression, and writes the result to data.value before forwarding. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_EXPRESSION (pn_expression_get_type ())

G_DECLARE_FINAL_TYPE (PnExpression, pn_expression, PN, EXPRESSION, PnNode)

PnExpression *pn_expression_new (void);

G_END_DECLS

#endif /* PN_EXPRESSION_H */
