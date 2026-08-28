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

#ifndef PN_BRIDGE_CONVERTER_H
#define PN_BRIDGE_CONVERTER_H

#include "pn-bridge-common.h"
#include "pn-http.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnBridgeConverter                                                  */
/*                                                                     */
/*  Cross-chain swap converter.  It asks the same providers the same   */
/*  question as #PnBridgeQuote — "what would you actually pay out for  */
/*  this?" — but for the amount that arrives on its input port rather  */
/*  than a fixed one, which makes it the dealable counterpart of the   */
/*  FX Converter (#PnRate): send it a number in #PnBridgeConverter:from */
/*  units and it emits what the bridge would deliver in                */
/*  #PnBridgeConverter:to units, the provider's spread, flat network   */
/*  fee and limits already taken out.                                  */
/*                                                                     */
/*  There is no periodic fetch.  Bridge pricing is amount-sensitive —  */
/*  a flat network fee dominates a small swap — so a cached rate could */
/*  not be re-scaled to a new amount honestly; the node has to ask     */
/*  again for every amount, and it only has an amount when a message   */
/*  arrives.  The request therefore rides the inherited auto-trigger   */
/*  worker as a one-shot rather than on a schedule, which also means   */
/*  the reply is emitted when it lands, not from receive(): one        */
/*  message in, one message out, a network round-trip apart.           */
/*                                                                     */
/*  Only one request is in flight at a time.  An amount arriving while */
/*  the provider is still answering replaces the one waiting rather    */
/*  than queueing behind it — a converter fed faster than the network  */
/*  answers should quote the newest amount, not work through a         */
/*  backlog of stale ones.                                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_BRIDGE_CONVERTER (pn_bridge_converter_get_type ())

G_DECLARE_FINAL_TYPE (PnBridgeConverter, pn_bridge_converter,
                      PN, BRIDGE_CONVERTER, PnHttp)

PnBridgeConverter *pn_bridge_converter_new (void);

G_END_DECLS

#endif /* PN_BRIDGE_CONVERTER_H */
