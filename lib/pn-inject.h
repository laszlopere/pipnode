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

#ifndef PN_INJECT_H
#define PN_INJECT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnInject                                                           */
/*                                                                     */
/*  Manual source node.  Carries a configurable text payload and      */
/*  emits a #PnMessage with that text in its "output" data member    */
/*  whenever pn_inject_fire() is called.  While the text is empty    */
/*  the node enters a "configuration required" red state and         */
/*  pn_inject_fire() becomes a no-op.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_INJECT (pn_inject_get_type ())

G_DECLARE_FINAL_TYPE (PnInject, pn_inject, PN, INJECT, PnNode)

PnInject *pn_inject_new  (void);

/**
 * pn_inject_fire:
 * @self: the inject node
 *
 * Emit a single message carrying the configured text in the data
 * bag's "output" member, the configured number in "value", and the
 * configured flag in "success".  Silently does nothing while the
 * text is unset.  Must be called from the main thread.
 */
void      pn_inject_fire (PnInject *self);

G_END_DECLS

#endif /* PN_INJECT_H */
