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

#ifndef PN_FIAT_H
#define PN_FIAT_H

#include "pn-http.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFiat                                                             */
/*                                                                     */
/*  Fiat-to-fiat currency converter — the national-currency sibling of */
/*  the FX Converter (#PnRate).  Same shape: a #PnHttp subclass with   */
/*  an input port, whose every incoming message has its `data.value`   */
/*  multiplied by the most recently fetched rate (from → to) before    */
/*  being forwarded.  Only the source of the rate differs: #PnRate     */
/*  asks CoinGecko for a crypto market price, this one asks            */
/*  Frankfurter for the European Central Bank's daily reference rate.  */
/*                                                                     */
/*  The two are deliberately separate nodes rather than one node with  */
/*  a 53-entry currency list: the ECB set is authoritative for fiat    */
/*  and free of the market-data caveats that come with a crypto quote, */
/*  it needs no USD pivot (any of its currencies can be the base), and */
/*  it moves once a working day rather than by the second.             */
/*                                                                     */
/*  Like #PnRate the fetched rate and the timestamp of the fetch are   */
/*  properties, so they round-trip through the worksheet and reopening */
/*  a flow does not fire a fresh request.                              */
/* ------------------------------------------------------------------ */

/**
 * PnFiatCurrency:
 * @PN_FIAT_CURRENCY_AUD: Australian dollar
 * @PN_FIAT_CURRENCY_BRL: Brazilian real
 * @PN_FIAT_CURRENCY_CAD: Canadian dollar
 * @PN_FIAT_CURRENCY_CHF: Swiss franc
 * @PN_FIAT_CURRENCY_CNY: Chinese renminbi yuan
 * @PN_FIAT_CURRENCY_CZK: Czech koruna
 * @PN_FIAT_CURRENCY_DKK: Danish krone
 * @PN_FIAT_CURRENCY_EUR: Euro — the currency the ECB quotes against
 * @PN_FIAT_CURRENCY_GBP: British pound
 * @PN_FIAT_CURRENCY_HKD: Hong Kong dollar
 * @PN_FIAT_CURRENCY_HUF: Hungarian forint
 * @PN_FIAT_CURRENCY_IDR: Indonesian rupiah
 * @PN_FIAT_CURRENCY_ILS: Israeli new shekel
 * @PN_FIAT_CURRENCY_INR: Indian rupee
 * @PN_FIAT_CURRENCY_ISK: Icelandic króna
 * @PN_FIAT_CURRENCY_JPY: Japanese yen
 * @PN_FIAT_CURRENCY_KRW: South Korean won
 * @PN_FIAT_CURRENCY_MXN: Mexican peso
 * @PN_FIAT_CURRENCY_MYR: Malaysian ringgit
 * @PN_FIAT_CURRENCY_NOK: Norwegian krone
 * @PN_FIAT_CURRENCY_NZD: New Zealand dollar
 * @PN_FIAT_CURRENCY_PHP: Philippine peso
 * @PN_FIAT_CURRENCY_PLN: Polish złoty
 * @PN_FIAT_CURRENCY_RON: Romanian leu
 * @PN_FIAT_CURRENCY_SEK: Swedish krona
 * @PN_FIAT_CURRENCY_SGD: Singapore dollar
 * @PN_FIAT_CURRENCY_THB: Thai baht
 * @PN_FIAT_CURRENCY_TRY: Turkish lira
 * @PN_FIAT_CURRENCY_USD: United States dollar
 * @PN_FIAT_CURRENCY_ZAR: South African rand
 *
 * The currencies the European Central Bank publishes a daily euro
 * reference rate for, which is exactly the set the default endpoint
 * serves.  Alphabetical, so the settings-dialog combo is orderly and
 * a new member can be appended without renumbering the saved files.
 */
typedef enum
{
    PN_FIAT_CURRENCY_AUD,
    PN_FIAT_CURRENCY_BRL,
    PN_FIAT_CURRENCY_CAD,
    PN_FIAT_CURRENCY_CHF,
    PN_FIAT_CURRENCY_CNY,
    PN_FIAT_CURRENCY_CZK,
    PN_FIAT_CURRENCY_DKK,
    PN_FIAT_CURRENCY_EUR,
    PN_FIAT_CURRENCY_GBP,
    PN_FIAT_CURRENCY_HKD,
    PN_FIAT_CURRENCY_HUF,
    PN_FIAT_CURRENCY_IDR,
    PN_FIAT_CURRENCY_ILS,
    PN_FIAT_CURRENCY_INR,
    PN_FIAT_CURRENCY_ISK,
    PN_FIAT_CURRENCY_JPY,
    PN_FIAT_CURRENCY_KRW,
    PN_FIAT_CURRENCY_MXN,
    PN_FIAT_CURRENCY_MYR,
    PN_FIAT_CURRENCY_NOK,
    PN_FIAT_CURRENCY_NZD,
    PN_FIAT_CURRENCY_PHP,
    PN_FIAT_CURRENCY_PLN,
    PN_FIAT_CURRENCY_RON,
    PN_FIAT_CURRENCY_SEK,
    PN_FIAT_CURRENCY_SGD,
    PN_FIAT_CURRENCY_THB,
    PN_FIAT_CURRENCY_TRY,
    PN_FIAT_CURRENCY_USD,
    PN_FIAT_CURRENCY_ZAR,
} PnFiatCurrency;

#define PN_TYPE_FIAT_CURRENCY (pn_fiat_currency_get_type ())
GType pn_fiat_currency_get_type (void);

#define PN_TYPE_FIAT (pn_fiat_get_type ())

G_DECLARE_FINAL_TYPE (PnFiat, pn_fiat, PN, FIAT, PnHttp)

PnFiat *pn_fiat_new (void);

/**
 * pn_fiat_currency_get_code:
 * @cur: a #PnFiatCurrency value.
 *
 * Returns the ISO 4217 alphabetic code of @cur ("EUR", "HUF", …) —
 * the enum nick, the dialog label and the string the endpoint wants,
 * all one and the same.  Out-of-range values answer "EUR".  The
 * returned string is owned by the table and must not be freed.
 */
const gchar *pn_fiat_currency_get_code (PnFiatCurrency cur);

/**
 * pn_fiat_currency_get_name:
 * @cur: a #PnFiatCurrency value.
 *
 * Returns the currency's English name ("Hungarian forint").  Unlike
 * the crypto tickers of #PnCurrency — where BTC and ETH need no gloss
 * — few people read MYR, RON and ISK at a glance, so the settings
 * dialog spells each one out beside its code.  The returned string is
 * owned by the table and must not be freed.
 */
const gchar *pn_fiat_currency_get_name (PnFiatCurrency cur);

G_END_DECLS

#endif /* PN_FIAT_H */
