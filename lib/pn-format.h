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

#ifndef PN_FORMAT_H
#define PN_FORMAT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFormat                                                           */
/*                                                                     */
/*  Filter that rewrites a message's data.output field from one of two */
/*  templated strings, chosen by the data.success boolean.  Each       */
/*  template may embed ${path/to/field} placeholders, which are        */
/*  expanded against the message JSON: the root carries "topic", "id"  */
/*  and "data" (the payload bag), so e.g. ${data/value} resolves to    */
/*  the data bag's "value" member.  Missing or non-boolean             */
/*  data.success is treated as false.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FORMAT (pn_format_get_type ())

G_DECLARE_FINAL_TYPE (PnFormat, pn_format, PN, FORMAT, PnNode)

PnFormat *pn_format_new (void);

G_END_DECLS

#endif /* PN_FORMAT_H */
