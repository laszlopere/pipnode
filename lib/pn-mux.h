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

#ifndef PN_MUX_H
#define PN_MUX_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMux                                                              */
/*                                                                     */
/*  A value-selected multiplexer — the CPU bus / operand-source        */
/*  selector.  It has one `select` input followed by #PnMux:inputs     */
/*  data inputs (2..8), and forwards the latched data.value of the     */
/*  currently-selected data input:                                     */
/*                                                                     */
/*    input 0        select  — latch the selector k (0-based over the  */
/*                             data inputs, clamped into range).        */
/*    input 1..N     in1..inN — data lines; the core latches each       */
/*                             line's last data.value.                  */
/*                                                                     */
/*  A message emits whenever the selector changes or the *selected*    */
/*  data line updates, carrying that line's value on data.value.  A    */
/*  data line that is not currently selected updates silently.         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MUX (pn_mux_get_type ())

G_DECLARE_FINAL_TYPE (PnMux, pn_mux, PN, MUX, PnNode)

PnMux *pn_mux_new (void);

G_END_DECLS

#endif /* PN_MUX_H */
