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

#ifndef PN_REDUCE_H
#define PN_REDUCE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnReduceMode                                                       */
/*                                                                     */
/*  The fold a #PnReduce applies to collapse a vector to one scalar.   */
/*  COUNT is the element count; RANGE is max - min; STDDEV is the      */
/*  population standard deviation; the rest read as named.             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_REDUCE_MODE (pn_reduce_mode_get_type ())

typedef enum
{
    PN_REDUCE_MODE_SUM,
    PN_REDUCE_MODE_MEAN,
    PN_REDUCE_MODE_MIN,
    PN_REDUCE_MODE_MAX,
    PN_REDUCE_MODE_RANGE,
    PN_REDUCE_MODE_COUNT,
    PN_REDUCE_MODE_PRODUCT,
    PN_REDUCE_MODE_STDDEV,
    PN_REDUCE_MODE_FIRST,
    PN_REDUCE_MODE_LAST,
} PnReduceMode;

GType pn_reduce_mode_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnReduce                                                           */
/*                                                                     */
/*  Vector → scalar fold and the worked-example consumer of the large- */
/*  numeric-vector machinery (TODO #43) — the dual of #PnRamp.  Reads  */
/*  data.value: when it is a $pnvector the node resolves it and folds  */
/*  the whole buffer down to a single number per #PnReduce:mode, then  */
/*  overwrites data.value with that scalar and forwards the message.   */
/*  A scalar data.value passes through as a one-element fold (sum =     */
/*  mean = min = max = first = last = the value; count = 1), so the    */
/*  node never errors on ordinary scalar traffic and re-enters the     */
/*  scalar world the rest of the flow lives in.                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_REDUCE (pn_reduce_get_type ())

G_DECLARE_FINAL_TYPE (PnReduce, pn_reduce, PN, REDUCE, PnNode)

PnReduce *pn_reduce_new (void);

G_END_DECLS

#endif /* PN_REDUCE_H */
