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

#ifndef PN_JUMP_IN_H
#define PN_JUMP_IN_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnJumpIn                                                            */
/*                                                                     */
/*  The entry end of a named wireless connection: a left-pointing       */
/*  pennant with one input port at its tip and no output.  Whatever it  */
/*  receives is cloned to every #PnJumpOut sharing its tag, anywhere in */
/*  the document.  See pn-jump.h for the full contract.                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_JUMP_IN (pn_jump_in_get_type ())

G_DECLARE_FINAL_TYPE (PnJumpIn, pn_jump_in, PN, JUMP_IN, PnNode)

PnJumpIn    *pn_jump_in_new     (void);

const gchar *pn_jump_in_get_tag (PnJumpIn *self);
void         pn_jump_in_set_tag (PnJumpIn *self, const gchar *tag);

G_END_DECLS

#endif /* PN_JUMP_IN_H */
