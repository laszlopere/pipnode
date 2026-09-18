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

#ifndef PN_VALUE_TREND_H
#define PN_VALUE_TREND_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnValueTrend                                                       */
/*                                                                     */
/*  Rising / falling split.  One input, two outputs: a message whose   */
/*  data.value is greater than the reference leaves, unchanged, by     */
/*  "rising" (output 0), one that is smaller by "falling" (output 1).  */
/*  Equal values are dropped, and so is the very first message —       */
/*  there is nothing to compare it against, it only seeds the          */
/*  reference.                                                         */
/*                                                                     */
/*  #PnValueTrend:min-change is a deadband: moves smaller than it are  */
/*  treated as unchanged.  The reference is the last value the node    */
/*  FORWARDED, not the last one it saw, so a drift of many sub-band    */
/*  steps in one direction still eventually trips.                     */
/*                                                                     */
/*  #PnValueTrend:unchanged-output adds a third output, "unchanged",   */
/*  which collects what would otherwise be dropped as an equal value   */
/*  or a sub-band move.  The first message stays dropped either way.   */
/*                                                                     */
/*  Messages whose data.value is missing or not a number are dropped   */
/*  and leave the reference alone.  Booleans count as 1 and 0.         */
/* ------------------------------------------------------------------ */

#define PN_VALUE_TREND_OUT_RISING     0
#define PN_VALUE_TREND_OUT_FALLING    1
#define PN_VALUE_TREND_OUT_UNCHANGED  2

typedef enum
{
    PN_VALUE_TREND_UNCHANGED = 0,
    PN_VALUE_TREND_RISING,
    PN_VALUE_TREND_FALLING,
} PnValueTrendDirection;

#define PN_TYPE_VALUE_TREND (pn_value_trend_get_type ())

G_DECLARE_FINAL_TYPE (PnValueTrend, pn_value_trend, PN, VALUE_TREND, PnNode)

PnValueTrend *pn_value_trend_new (void);

/**
 * pn_value_trend_classify:
 * @prev: the reference — the last value forwarded
 * @cur:  the value just received
 * @band: deadband width; moves smaller than it are "unchanged".  A
 *        negative band is read as 0.
 *
 * Pure classification seam, no state: %PN_VALUE_TREND_RISING when @cur
 * is above @prev by at least @band, %PN_VALUE_TREND_FALLING when it is
 * below by at least @band, %PN_VALUE_TREND_UNCHANGED otherwise.  With
 * the default @band of 0 an exactly equal @cur is unchanged, since a
 * move of zero is not a move.
 */
PnValueTrendDirection pn_value_trend_classify (gdouble prev,
                                               gdouble cur,
                                               gdouble band);

G_END_DECLS

#endif /* PN_VALUE_TREND_H */
