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

#ifndef PN_JUMP_OUT_H
#define PN_JUMP_OUT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnJumpOut                                                           */
/*                                                                     */
/*  The exit end of a named wireless connection: a right-pointing       */
/*  pennant with one output port at its tip and no input.  It has no    */
/*  receive vfunc — it never sits downstream of a wire.  A #PnJumpIn    */
/*  sharing its tag drives it directly through pn_jump_out_deliver(),   */
/*  which re-emits onto whatever wires leave the flag.                 */
/*                                                                     */
/*  See pn-jump.h for the full contract.                               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_JUMP_OUT (pn_jump_out_get_type ())

G_DECLARE_FINAL_TYPE (PnJumpOut, pn_jump_out, PN, JUMP_OUT, PnNode)

PnJumpOut   *pn_jump_out_new     (void);

const gchar *pn_jump_out_get_tag (PnJumpOut *self);
void         pn_jump_out_set_tag (PnJumpOut *self, const gchar *tag);

/**
 * pn_jump_out_deliver:
 * @self:    the receiving flag
 * @message: the message to re-emit; @self takes its own copy
 *
 * Emit @message onto @self's outgoing wires.  Called by #PnJumpIn for
 * each tag match.  A disabled flag drops the message.
 */
void         pn_jump_out_deliver (PnJumpOut *self, PnMessage *message);

G_END_DECLS

#endif /* PN_JUMP_OUT_H */
