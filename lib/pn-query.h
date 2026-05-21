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

#ifndef PN_QUERY_H
#define PN_QUERY_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnQuery                                                            */
/*                                                                     */
/*  Generic JSON-query node.  Every received message is fed through a  */
/*  configurable JMESPath expression and the result placed onto the    */
/*  outgoing message under #PnQuery:result-field.  The output message  */
/*  is always well formed: a JSON-null result, a parse error, or an    */
/*  unexpected type still produces a forwarded message — failure       */
/*  modes only show up as a non-empty data.error string and a null     */
/*  result field, never as a dropped message.                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_QUERY (pn_query_get_type ())

G_DECLARE_FINAL_TYPE (PnQuery, pn_query, PN, QUERY, PnNode)

PnQuery *pn_query_new (void);

G_END_DECLS

#endif /* PN_QUERY_H */
