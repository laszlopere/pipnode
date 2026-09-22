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

#ifndef PN_EXPR_BIND_H
#define PN_EXPR_BIND_H

#include <glib.h>

#include "pn-message.h"
#include "pn-node.h"
#include "pn-vector.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Message -> variables                                               */
/*                                                                     */
/*  The rule by which an arriving #PnMessage becomes the variables an   */
/*  expression may name.  It used to live twice — once in              */
/*  pn-expression.c and once, with the collation pass on top, in       */
/*  pn-expression2.c — and #PnFigure would have made a third copy      */
/*  (TODO 80.8c), so it lives here instead.                            */
/*                                                                     */
/*  Two rules, because the two calculators genuinely differ:           */
/*                                                                     */
/*  FLAT (Calculator): every numeric member of the data bag is bound    */
/*  under its own name, `value` included.  One input, no suffixes,     */
/*  nothing remembered between messages.                               */
/*                                                                     */
/*  COLLATED (Calculator 2, and the figure): two passes over the bag.  */
/*  First the per-input headline values the core's input collation has  */
/*  already injected under each input's DISPLAY NAME — latched, so     */
/*  `angle` still holds its last value when `load` is the input that   */
/*  just fired.  Then this message's other numeric members, suffixed   */
/*  with the 1-based arriving input number (a sibling data.temp binds  */
/*  as `temp1`), which are NOT latched across inputs.  A node using    */
/*  this rule must have called pn_node_set_collate_inputs().           */
/*                                                                     */
/*  Neither rule binds strings: a message's text never becomes a       */
/*  variable, in either calculator or in a figure (80.8f).             */
/* ------------------------------------------------------------------ */

/**
 * PnExprBindFunc:
 * @name:   the variable name to bind
 * @scalar: the value, when @vec is %NULL
 * @vec:    (nullable): the vector value, or %NULL for a scalar
 * @user_data: what the caller passed
 *
 * Receives one binding.  The vector, when there is one, is borrowed for
 * the duration of the call; a callback that keeps it must take its own
 * reference.
 */
typedef void (*PnExprBindFunc) (const gchar *name,
                                gdouble      scalar,
                                PnVector    *vec,
                                gpointer     user_data);

/**
 * pn_expr_bind_flat:
 * @message: the arriving message
 * @bind:    (scope call): called once per binding
 * @user_data: passed through to @bind
 *
 * The FLAT rule: every numeric (or `$pnvector`) member of @message's
 * data bag, under its own name.
 */
void pn_expr_bind_flat (PnMessage      *message,
                        PnExprBindFunc  bind,
                        gpointer        user_data);

/**
 * pn_expr_bind_collated:
 * @node:    the receiving node, whose input names and count the rule
 *           reads; the arriving input comes from
 *           pn_node_current_input()
 * @message: the arriving message, already collated by the core
 * @bind:    (scope call): called once per binding
 * @user_data: passed through to @bind
 *
 * The COLLATED rule: the latched per-input headline values under the
 * input names, then this message's other numeric members suffixed with
 * the 1-based arriving input number.
 *
 * Call it only from inside the node's receive(), where
 * pn_node_current_input() means something.
 */
void pn_expr_bind_collated (PnNode         *node,
                            PnMessage      *message,
                            PnExprBindFunc  bind,
                            gpointer        user_data);

/**
 * pn_expr_bind_to_store:
 * @name:   the variable name
 * @scalar: the value, when @vec is %NULL
 * @vec:    (nullable): the vector value
 * @user_data: (type PnVarStore): the store to bind into
 *
 * A ready-made #PnExprBindFunc that writes each binding into a
 * #PnVarStore — what both calculators want, and what a figure wants for
 * everything except its own between-frames snapshot.  The store is not
 * cleared first; the caller decides that.
 */
void pn_expr_bind_to_store (const gchar *name,
                            gdouble      scalar,
                            PnVector    *vec,
                            gpointer     user_data);

G_END_DECLS

#endif /* PN_EXPR_BIND_H */
