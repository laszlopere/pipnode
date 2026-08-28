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

#ifndef PN_BRIDGE_QUOTE_H
#define PN_BRIDGE_QUOTE_H

#include "pn-bridge-common.h"
#include "pn-http.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnBridgeQuote                                                      */
/*                                                                     */
/*  Cross-chain swap-rate monitor.  Where the FX Converter (#PnRate)   */
/*  reports a mid-market reference price, this node asks a real        */
/*  bridge what it would actually pay out for a given amount — the     */
/*  quote therefore carries the provider's spread, its flat network    */
/*  fee and its per-pair min/max limits.                               */
/*                                                                     */
/*  It is a source: no input port.  Every #PnAutoTrigger:period        */
/*  seconds it fetches a quote for #PnBridgeQuote:amount units of      */
/*  #PnBridgeQuote:from and emits the result.  Point two of them at    */
/*  the same pair with different #PnBridgeQuote:bridge values to       */
/*  compare providers on one graph.  For the same question asked of a  */
/*  varying amount arriving on an input port, see #PnBridgeConverter.  */
/*                                                                     */
/*  The quote and the timestamp of the last successful fetch           */
/*  round-trip through the worksheet on save/load, so reopening a      */
/*  flow repaints downstream displays from cache instead of firing a   */
/*  fresh request at the provider.                                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_BRIDGE_QUOTE (pn_bridge_quote_get_type ())

G_DECLARE_FINAL_TYPE (PnBridgeQuote, pn_bridge_quote, PN, BRIDGE_QUOTE, PnHttp)

PnBridgeQuote *pn_bridge_quote_new (void);

G_END_DECLS

#endif /* PN_BRIDGE_QUOTE_H */
