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

#ifndef PN_PARSE_H
#define PN_PARSE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnParse                                                            */
/*                                                                     */
/*  Parse a JSON *string* held in one member of the message into real  */
/*  structure, so the rest of the pipeline can address it.  Http       */
/*  Client hands its response body back as an opaque string in         */
/*  data.output; JMESPath runs against the message tree, where that    */
/*  body is still one long string.  This node is the bridge.           */
/*                                                                     */
/*  A message is never dropped.  On failure the bag is left exactly    */
/*  as it arrived — the unparsed text survives for inspection — and    */
/*  the reason appears in data.error.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PARSE (pn_parse_get_type ())

G_DECLARE_FINAL_TYPE (PnParse, pn_parse, PN, PARSE, PnNode)

PnParse *pn_parse_new (void);

G_END_DECLS

#endif /* PN_PARSE_H */
