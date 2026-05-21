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

#ifndef PN_REWRITE_H
#define PN_REWRITE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRewrite                                                          */
/*                                                                     */
/*  Filter that replaces an incoming message with a JSON template      */
/*  authored in the settings dialog.  The template is parsed as JSON   */
/*  and may embed ${path/to/field} placeholders that resolve against   */
/*  the incoming message JSON (the same lookup root used by PnFormat   */
/*  and PnDial): the root carries `topic`, `id`, `created`, and the    */
/*  `data` payload bag, so `${data/value}` expands to the data bag's   */
/*  `value` member and `${topic}` to the incoming topic.               */
/*                                                                     */
/*  When the rendered JSON is an object that carries a top-level       */
/*  `topic` string it overrides the outgoing topic; the object's       */
/*  `data` member (when present) replaces the outgoing data bag.       */
/*  An object with neither of those is treated as the new data bag    */
/*  in its entirety.                                                   */
/*                                                                     */
/*  The dialog renders the template in a #GtkSourceView editor with    */
/*  JSON syntax highlighting.                                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_REWRITE (pn_rewrite_get_type ())

G_DECLARE_FINAL_TYPE (PnRewrite, pn_rewrite, PN, REWRITE, PnNode)

PnRewrite *pn_rewrite_new (void);

G_END_DECLS

#endif /* PN_REWRITE_H */
