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

#ifndef PN_VALUE_H
#define PN_VALUE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnValue                                                            */
/*                                                                     */
/*  Filter that overwrites a message's data.value with a fixed double  */
/*  set in the node's property dialog, regardless of what arrived.     */
/*  The rest of the message (topic, id, other data members) passes     */
/*  through untouched.                                                 */
/* ------------------------------------------------------------------ */

#define PN_TYPE_VALUE (pn_value_get_type ())

G_DECLARE_FINAL_TYPE (PnValue, pn_value, PN, VALUE, PnNode)

PnValue *pn_value_new (void);

G_END_DECLS

#endif /* PN_VALUE_H */
