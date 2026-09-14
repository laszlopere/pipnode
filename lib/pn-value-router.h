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

#ifndef PN_VALUE_ROUTER_H
#define PN_VALUE_ROUTER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnValueRouter                                                      */
/*                                                                     */
/*  One input, N outputs (the "outputs" property, 2..16), one rule per */
/*  output (the "rules" property, a JSON array).  Every arriving       */
/*  message reads one number from its data bag — data.value, or the    */
/*  member named by "path" (a dotted path reaches nested objects) —    */
/*  and leaves, unchanged, by the FIRST output whose rule matches.  A   */
/*  message no rule matches is dropped.                                */
/*                                                                     */
/*  A rule is one of:                                                  */
/*    { "op": "<" | "<=" | "==" | "!=" | ">=" | ">", "value": N }      */
/*    { "op": "range", "low": A, "high": B }   A <= number <= B        */
/*    { "op": "else" }                         matches every message   */
/*    {} or null                               unused, matches nothing */
/*                                                                     */
/*  Booleans count as 1.0 / 0.0.  A message whose member is missing or */
/*  not a number matches only an "else" rule.  Because the first match */
/*  wins, an "else" on the last output collects everything the earlier */
/*  outputs did not claim.                                             */
/*                                                                     */
/*  "rules" always describes 16 slots, so lowering the output count    */
/*  keeps the rules of removed outputs.  Each output is labelled on    */
/*  the worksheet with its rule ("== 3", "10..20", "else").            */
/* ------------------------------------------------------------------ */

#define PN_VALUE_ROUTER_MIN_OUTPUTS 2
#define PN_VALUE_ROUTER_MAX_OUTPUTS 16
#define PN_VALUE_ROUTER_DEF_OUTPUTS 3

typedef enum
{
    PN_VALUE_ROUTER_OP_UNUSED = 0,
    PN_VALUE_ROUTER_OP_LT,
    PN_VALUE_ROUTER_OP_LE,
    PN_VALUE_ROUTER_OP_EQ,
    PN_VALUE_ROUTER_OP_NE,
    PN_VALUE_ROUTER_OP_GE,
    PN_VALUE_ROUTER_OP_GT,
    PN_VALUE_ROUTER_OP_RANGE,
    PN_VALUE_ROUTER_OP_ELSE,
    PN_VALUE_ROUTER_N_OPS
} PnValueRouterOp;

#define PN_TYPE_VALUE_ROUTER (pn_value_router_get_type ())

G_DECLARE_FINAL_TYPE (PnValueRouter, pn_value_router,
                      PN, VALUE_ROUTER, PnNode)

PnValueRouter  *pn_value_router_new (void);

/**
 * pn_value_router_op_to_string:
 *
 * The JSON spelling of @op ("" for unused, "<", ..., "range", "else").
 */
const gchar    *pn_value_router_op_to_string (PnValueRouterOp op);

/**
 * pn_value_router_op_from_string:
 *
 * Parses a JSON spelling; anything unknown is PN_VALUE_ROUTER_OP_UNUSED.
 */
PnValueRouterOp pn_value_router_op_from_string (const gchar *text);

/**
 * pn_value_router_get_rule:
 * @index: 0-based output index, 0 .. PN_VALUE_ROUTER_MAX_OUTPUTS-1
 * @a:     (out) (optional): the comparison value, or the range's low end
 * @b:     (out) (optional): the range's high end (0 for other ops)
 */
PnValueRouterOp pn_value_router_get_rule (PnValueRouter *self,
                                          gint           index,
                                          gdouble       *a,
                                          gdouble       *b);

/**
 * pn_value_router_set_rule:
 * @index: 0-based output index, 0 .. PN_VALUE_ROUTER_MAX_OUTPUTS-1
 * @a:     the comparison value, or the range's low end (ignored for
 *         unused / else)
 * @b:     the range's high end (ignored for every other op)
 *
 * Sets one output's rule; notifies "rules" when it changed.
 */
void            pn_value_router_set_rule (PnValueRouter  *self,
                                          gint            index,
                                          PnValueRouterOp op,
                                          gdouble         a,
                                          gdouble         b);

/**
 * pn_value_router_route_number:
 * @has_number: whether the message carried a number at "path"
 * @number:     that number (ignored when @has_number is %FALSE)
 *
 * Pure routing seam, for tests: the 0-based output a message with this
 * number would leave by, or -1 when it would be dropped.
 */
gint            pn_value_router_route_number (PnValueRouter *self,
                                              gboolean       has_number,
                                              gdouble        number);

G_END_DECLS

#endif /* PN_VALUE_ROUTER_H */
