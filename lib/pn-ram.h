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

#ifndef PN_RAM_H
#define PN_RAM_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRam                                                              */
/*                                                                     */
/*  Addressable read/write memory — a #PnProm you can write at         */
/*  runtime.  It shares PROM's addressing so the two are drop-in on    */
/*  the read path: the incoming data.value is the address (rounded to  */
/*  the nearest whole cell).                                           */
/*                                                                     */
/*    read  (data.write absent or false):                             */
/*        emit the stored word on data.value (unwritten cells read 0), */
/*        with the decoded cell echoed on data.address — exactly what  */
/*        #PnProm emits.                                               */
/*    write (data.write true / > 0.5):                                 */
/*        store data.word at the address.  Emits NOTHING (a store      */
/*        cannot re-trigger the graph, so RAM can sit in a feedback     */
/*        loop without forming a synchronous cycle).                   */
/*                                                                     */
/*  #PnRam:contents optionally seeds the memory with an image in the   */
/*  same "<address> <word>" text format as #PnProm:contents.           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_RAM (pn_ram_get_type ())

G_DECLARE_FINAL_TYPE (PnRam, pn_ram, PN, RAM, PnNode)

PnRam *pn_ram_new (void);

G_END_DECLS

#endif /* PN_RAM_H */
