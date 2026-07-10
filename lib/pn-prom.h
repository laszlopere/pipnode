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

#ifndef PN_PROM_H
#define PN_PROM_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnProm                                                             */
/*                                                                     */
/*  A programmable read-only memory.  The incoming data.value is the   */
/*  address: it is rounded to the nearest integer, looked up in the    */
/*  #PnProm:contents table, and the stored word replaces data.value    */
/*  on the outgoing message.  Addresses that were never programmed     */
/*  read back as 0.0, the way an unburnt PROM cell reads as 0x00.      */
/*                                                                     */
/*  #PnProm:contents is the whole memory image in textual form — one   */
/*  "<address> <word>" pair per line, either column written in hex     */
/*  (0x…) or decimal:                                                  */
/*                                                                     */
/*      0x0000 0xff                                                    */
/*      0x0001 0x1a                                                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PROM (pn_prom_get_type ())

G_DECLARE_FINAL_TYPE (PnProm, pn_prom, PN, PROM, PnNode)

PnProm *pn_prom_new (void);

G_END_DECLS

#endif /* PN_PROM_H */
