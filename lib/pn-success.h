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

#ifndef PN_SUCCESS_H
#define PN_SUCCESS_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSuccess                                                          */
/*                                                                     */
/*  Pass-through filter that forwards a #PnMessage only when its       */
/*  mandatory boolean data.success member is %TRUE.  Messages whose    */
/*  "success" is missing or not a boolean are dropped, so a malformed  */
/*  upstream cannot leak through.  The dedicated counterpart of        */
/*  #PnFailure: rather than configuring a generic #PnFilter with a     */
/*  one-rule predicate on the contract field, drop in one of these two */
/*  nodes for the common "branch only on success / only on failure"    */
/*  shape.                                                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SUCCESS (pn_success_get_type ())

G_DECLARE_FINAL_TYPE (PnSuccess, pn_success, PN, SUCCESS, PnNode)

PnSuccess *pn_success_new (void);

G_END_DECLS

#endif /* PN_SUCCESS_H */
