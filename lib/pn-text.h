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

#ifndef PN_TEXT_H
#define PN_TEXT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnText                                                             */
/*                                                                     */
/*  Filter that overwrites a message's data.output with a fixed,       */
/*  pre-configured string, regardless of what arrived.  The text is    */
/*  edited in a multi-line editor in the property dialog; leading and  */
/*  trailing newline characters are stripped before the value is       */
/*  written, so a block typed with a stray blank first/last line still */
/*  yields a clean payload.  The rest of the message (topic, id, other */
/*  data members) passes through untouched.                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TEXT (pn_text_get_type ())

G_DECLARE_FINAL_TYPE (PnText, pn_text, PN, TEXT, PnNode)

PnText *pn_text_new (void);

G_END_DECLS

#endif /* PN_TEXT_H */
