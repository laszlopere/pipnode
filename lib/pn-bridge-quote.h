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

#include "pn-http.h"
#include "pn-rate.h"     /* PnCurrency / PN_TYPE_CURRENCY */

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
/*  compare providers on one graph.                                    */
/*                                                                     */
/*  The quote and the timestamp of the last successful fetch           */
/*  round-trip through the worksheet on save/load, so reopening a      */
/*  flow repaints downstream displays from cache instead of firing a   */
/*  fresh request at the provider.                                     */
/* ------------------------------------------------------------------ */

/**
 * PnBridge:
 * @PN_BRIDGE_PULSELN:   PulseLN (app.pulseln.com).  One keyless request
 *                       returns the whole cross-rate matrix; the quote is
 *                       computed locally from it, fees included.
 * @PN_BRIDGE_CHANGENOW: ChangeNOW (api.changenow.io), v1 endpoints, which
 *                       need no API key.  The provider quotes the amount
 *                       directly and publishes its own min/max range.
 */
typedef enum
{
    PN_BRIDGE_PULSELN,
    PN_BRIDGE_CHANGENOW,
} PnBridge;

#define PN_TYPE_BRIDGE (pn_bridge_get_type ())
GType pn_bridge_get_type (void);

#define PN_TYPE_BRIDGE_QUOTE (pn_bridge_quote_get_type ())

G_DECLARE_FINAL_TYPE (PnBridgeQuote, pn_bridge_quote, PN, BRIDGE_QUOTE, PnHttp)

PnBridgeQuote *pn_bridge_quote_new (void);

/**
 * pn_bridge_get_display_name:
 * @bridge: a #PnBridge value.
 *
 * Returns the provider's human-readable name ("PulseLN", "ChangeNOW"), as
 * stamped on the emitted message's `bridge` field.  Out-of-range values
 * fall back to the first bridge.  The string is owned by the table and
 * must not be freed.
 */
const gchar *pn_bridge_get_display_name (PnBridge bridge);

/**
 * pn_bridge_get_default_url:
 * @bridge: a #PnBridge value.
 *
 * Returns the provider's default endpoint, which the node installs on the
 * inherited #PnHttp:url whenever #PnBridgeQuote:bridge changes.  The
 * string is owned by the table and must not be freed.
 */
const gchar *pn_bridge_get_default_url (PnBridge bridge);

/**
 * pn_bridge_get_ticker:
 * @bridge: a #PnBridge value.
 * @cur:    a #PnCurrency value.
 *
 * Returns the symbol @bridge uses for @cur — uppercase for PulseLN
 * ("PLS", "BNB"), ChangeNOW's lowercase legacy ticker otherwise ("pls",
 * "bnbbsc").  Returns %NULL when the provider does not list the currency,
 * which is what marks a pair unsupported.  The string is owned by the
 * table and must not be freed.
 */
const gchar *pn_bridge_get_ticker (PnBridge bridge, PnCurrency cur);

G_END_DECLS

#endif /* PN_BRIDGE_QUOTE_H */
