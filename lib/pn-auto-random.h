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

#ifndef PN_AUTO_RANDOM_H
#define PN_AUTO_RANDOM_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAutoRandomDistribution                                           */
/*                                                                     */
/*  Selects how #PnAutoRandom samples each tick's value out of the     */
/*  configured [min, max] interval.  Uniform spreads the mass evenly,  */
/*  Normal concentrates around the midpoint with stddev = range / 6,   */
/*  Triangular peaks at the midpoint and falls linearly to zero at     */
/*  the bounds, Exponential is heavily biased toward @min.  Every     */
/*  draw is clamped to [min, max] so downstream nodes can rely on the */
/*  emitted value sitting inside the configured range regardless of    */
/*  which distribution is in use.                                      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AUTO_RANDOM_DISTRIBUTION (pn_auto_random_distribution_get_type ())

typedef enum
{
    PN_AUTO_RANDOM_UNIFORM,
    PN_AUTO_RANDOM_NORMAL,
    PN_AUTO_RANDOM_TRIANGULAR,
    PN_AUTO_RANDOM_EXPONENTIAL,
} PnAutoRandomDistribution;

GType pn_auto_random_distribution_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnAutoRandom                                                       */
/*                                                                     */
/*  Auto-trigger source node that emits a freshly-sampled random       */
/*  number on its output port once per #PnAutoTrigger:period seconds.  */
/*  Sibling of #PnAutoInjector — the message envelope and data-bag    */
/*  are shaped the same way (data.value, data.success, data.output) so */
/*  filters and sinks wired downstream do not care which of the two   */
/*  is feeding them.  The sampled number lives at data.value; the     */
/*  range and distribution function are configured via the node's     */
/*  properties.                                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AUTO_RANDOM (pn_auto_random_get_type ())

G_DECLARE_FINAL_TYPE (PnAutoRandom, pn_auto_random,
                      PN, AUTO_RANDOM, PnAutoTrigger)

PnAutoRandom *pn_auto_random_new (void);

G_END_DECLS

#endif /* PN_AUTO_RANDOM_H */
