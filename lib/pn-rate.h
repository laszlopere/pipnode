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

#ifndef PN_RATE_H
#define PN_RATE_H

#include "pn-http.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnRate                                                             */
/*                                                                     */
/*  Crypto/fiat exchange-rate converter.  Inherits the periodic fetch  */
/*  + HTTP plumbing from #PnHttp and adds an input port: every         */
/*  incoming message has its `data.value` multiplied by the most       */
/*  recently fetched rate (from → to) and is forwarded on the output. */
/*                                                                     */
/*  The rate itself is fetched via the inherited auto-trigger and      */
/*  cached in the #PnRate:rate / #PnRate:last-update properties, both  */
/*  of which round-trip through the worksheet on save/load — so        */
/*  reopening a flow does not fire a fresh request and risk a          */
/*  rate-limit ban from the upstream provider.                         */
/* ------------------------------------------------------------------ */

/**
 * PnCurrency:
 * @PN_CURRENCY_ADA:   Cardano (CoinGecko id "cardano")
 * @PN_CURRENCY_ATOM:  Cosmos (CoinGecko id "cosmos")
 * @PN_CURRENCY_AVAX:  Avalanche (CoinGecko id "avalanche-2")
 * @PN_CURRENCY_BCH:   Bitcoin Cash (CoinGecko id "bitcoin-cash")
 * @PN_CURRENCY_BNB:   BNB (CoinGecko id "binancecoin")
 * @PN_CURRENCY_BTC:   Bitcoin (CoinGecko id "bitcoin")
 * @PN_CURRENCY_CRO:   Cronos (CoinGecko id "crypto-com-chain")
 * @PN_CURRENCY_DOGE:  Dogecoin (CoinGecko id "dogecoin")
 * @PN_CURRENCY_DOT:   Polkadot (CoinGecko id "polkadot")
 * @PN_CURRENCY_ETH:   Ether (CoinGecko id "ethereum")
 * @PN_CURRENCY_LINK:  Chainlink (CoinGecko id "chainlink")
 * @PN_CURRENCY_LTC:   Litecoin (CoinGecko id "litecoin")
 * @PN_CURRENCY_MATIC: Polygon (CoinGecko id "matic-network")
 * @PN_CURRENCY_PLS:   Pulse, the native asset of PulseChain
 *                     (CoinGecko id "pulsechain")
 * @PN_CURRENCY_SOL:   Solana (CoinGecko id "solana")
 * @PN_CURRENCY_TRX:   TRON (CoinGecko id "tron")
 * @PN_CURRENCY_UNI:   Uniswap (CoinGecko id "uniswap")
 * @PN_CURRENCY_USD:   US dollar (pivot — every rate is quoted against USD)
 * @PN_CURRENCY_USDC:  USD Coin (CoinGecko id "usd-coin")
 * @PN_CURRENCY_USDT:  Tether (CoinGecko id "tether")
 * @PN_CURRENCY_XLM:   Stellar (CoinGecko id "stellar")
 * @PN_CURRENCY_XMR:   Monero (CoinGecko id "monero")
 * @PN_CURRENCY_XRP:   XRP (CoinGecko id "ripple")
 */
typedef enum
{
    PN_CURRENCY_ADA,
    PN_CURRENCY_ATOM,
    PN_CURRENCY_AVAX,
    PN_CURRENCY_BCH,
    PN_CURRENCY_BNB,
    PN_CURRENCY_BTC,
    PN_CURRENCY_CRO,
    PN_CURRENCY_DOGE,
    PN_CURRENCY_DOT,
    PN_CURRENCY_ETH,
    PN_CURRENCY_LINK,
    PN_CURRENCY_LTC,
    PN_CURRENCY_MATIC,
    PN_CURRENCY_PLS,
    PN_CURRENCY_SOL,
    PN_CURRENCY_TRX,
    PN_CURRENCY_UNI,
    PN_CURRENCY_USD,
    PN_CURRENCY_USDC,
    PN_CURRENCY_USDT,
    PN_CURRENCY_XLM,
    PN_CURRENCY_XMR,
    PN_CURRENCY_XRP,
} PnCurrency;

#define PN_TYPE_CURRENCY (pn_currency_get_type ())
GType pn_currency_get_type (void);

#define PN_TYPE_RATE (pn_rate_get_type ())

G_DECLARE_FINAL_TYPE (PnRate, pn_rate, PN, RATE, PnHttp)

PnRate *pn_rate_new (void);

/**
 * pn_currency_get_icon_name:
 * @cur: a #PnCurrency value.
 *
 * Returns the basename (no extension) of the bundled PNG icon for @cur,
 * suitable for resolving against `$datadir/pipnode/icons/<name>.png`.
 * The returned string is owned by the table and must not be freed.
 */
const gchar *pn_currency_get_icon_name (PnCurrency cur);

G_END_DECLS

#endif /* PN_RATE_H */
