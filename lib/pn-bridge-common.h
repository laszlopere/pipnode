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

#ifndef PN_BRIDGE_COMMON_H
#define PN_BRIDGE_COMMON_H

#include "pn-rate.h"     /* PnCurrency / PN_TYPE_CURRENCY */

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Cross-chain bridge back-ends — shared, GTK-free, node-free.        */
/*                                                                     */
/*  Two nodes ask the same providers the same question and differ only */
/*  in what makes them ask: #PnBridgeQuote polls on a timer for a      */
/*  fixed reference amount, #PnBridgeConverter quotes the amount that  */
/*  arrives on its input port.  Everything between the URL and the     */
/*  numbers — the provider table, the per-provider ticker spellings,   */
/*  PulseLN's fee arithmetic, ChangeNOW's error vocabulary — is the    */
/*  same for both and lives here.                                      */
/*                                                                     */
/*  Nothing in this file touches a #PnNode, so the parse path can be   */
/*  driven straight from a unit test with a captured reply body.  The  */
/*  one function that does I/O (pn_bridge_fetch_range) is called out   */
/*  as such and is always optional.                                    */
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

/**
 * PnBridgeRequest:
 * @bridge: which provider to ask.
 * @from:   asset being sold.
 * @to:     asset being bought.
 * @amount: how much of @from to quote, in @from units.
 *
 * One question for one provider.  Callers snapshot their node state into
 * this struct under their own lock, so the parse path reads a coherent
 * set of settings rather than four separately-locked fields.
 */
typedef struct
{
    PnBridge    bridge;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     amount;
} PnBridgeRequest;

/**
 * PnBridgeResult:
 * @ok:             %TRUE when a usable quote was produced.
 * @error:          (nullable): why not, when @ok is %FALSE.  A bare cause
 *                  ("PulseLN returned HTTP 502"); callers prefix it.
 * @quote:          payout in @to units for the requested amount.
 * @fee:            provider fee as a fraction; 0 means "not published",
 *                  which is not the same as free.
 * @min_amount:     provider limits in @from units; 0 means unknown or
 * @max_amount:     uncapped.
 * @swappable:      %FALSE when the provider prices the route but would
 *                  not execute it right now — a closed payout pool, or an
 *                  amount outside the band.  @quote is still valid.
 * @warning:        (nullable): why it is not swappable, in one sentence.
 * @speed_forecast: (nullable): the provider's own ETA, when it offers one.
 *
 * Filled by pn_bridge_parse_reply().  Initialise with
 * pn_bridge_result_init() and release with pn_bridge_result_clear().
 */
typedef struct
{
    gboolean  ok;
    gchar    *error;
    gdouble   quote;
    gdouble   fee;
    gdouble   min_amount;
    gdouble   max_amount;
    gboolean  swappable;
    gchar    *warning;
    gchar    *speed_forecast;
} PnBridgeResult;

/**
 * pn_bridge_result_init:
 * @result: an uninitialised #PnBridgeResult.
 *
 * Zeroes @result and sets @swappable to %TRUE — a route is assumed open
 * until a reply says otherwise, so a parse that never reaches the
 * closure checks does not invent a closure.
 */
void pn_bridge_result_init (PnBridgeResult *result);

/**
 * pn_bridge_result_clear:
 * @result: a #PnBridgeResult.
 *
 * Frees the strings @result owns and re-initialises it, so the same
 * struct can be reused for another request.
 */
void pn_bridge_result_clear (PnBridgeResult *result);

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
 * Returns the provider's default endpoint, which a node installs on its
 * inherited #PnHttp:url whenever the selected bridge changes.  The string
 * is owned by the table and must not be freed.
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

/**
 * pn_bridge_currency_nick:
 * @cur: a #PnCurrency value.
 *
 * Returns the ticker nick of @cur as the FX Converter spells it ("PLS",
 * "BNB") — what the message's `from`/`to` fields and every human-readable
 * string use, where the provider's own spelling ("bnbbsc") would only
 * confuse.  Never %NULL; an unknown value reads "?".  The string is owned
 * by the enum class and must not be freed.
 */
const gchar *pn_bridge_currency_nick (PnCurrency cur);

/**
 * pn_bridge_pair_is_supported:
 * @bridge: a #PnBridge value.
 * @from:   asset being sold.
 * @to:     asset being bought.
 *
 * Returns %TRUE when @bridge lists both assets and they differ — the
 * provider half of a node's "am I configured?" test.
 */
gboolean pn_bridge_pair_is_supported (PnBridge   bridge,
                                      PnCurrency from,
                                      PnCurrency to);

/**
 * pn_bridge_build_url:
 * @request:  what is being quoted.
 * @base_url: the endpoint base, normally #PnHttp:url.
 *
 * Returns (transfer full): the full GET URL for one quote.  PulseLN takes
 * no parameters — one blob carries every pair — so its URL is @base_url
 * verbatim; ChangeNOW carries the amount and the pair in the path.  The
 * amount is formatted C-locale, so a comma decimal separator can never
 * corrupt the query.
 */
gchar *pn_bridge_build_url (const PnBridgeRequest *request,
                            const gchar           *base_url);

/**
 * pn_bridge_fetch_range:
 * @request:         what is being quoted.
 * @base_url:        the quote endpoint; the range endpoint is derived
 *                   from it, so a node re-pointed at a proxy keeps both
 *                   halves together.
 * @timeout_seconds: socket timeout for this one request.
 * @result:          (inout): receives @min_amount / @max_amount.
 *
 * Blocking, best-effort lookup of the provider's per-pair limits.  Only
 * ChangeNOW publishes them on a separate endpoint, so this is a no-op for
 * every other bridge; a failure is silent, because it must not sink an
 * otherwise good quote.  It is what turns a bare "max_amount_exceeded"
 * into a sentence naming the actual cap, so call it before
 * pn_bridge_parse_reply().  **Does network I/O** — worker threads only.
 */
void pn_bridge_fetch_range (const PnBridgeRequest *request,
                            const gchar           *base_url,
                            guint                  timeout_seconds,
                            PnBridgeResult        *result);

/**
 * pn_bridge_parse_reply:
 * @request:     what was asked.
 * @http_status: the HTTP status the reply arrived with.
 * @body:        (nullable): the reply body.
 * @result:      (inout): initialised by pn_bridge_result_init(), and
 *               optionally pre-filled by pn_bridge_fetch_range().
 *
 * Turns one provider reply into a quote.  On success @result.ok is %TRUE
 * and @quote carries the payout; otherwise @result.error explains why in
 * one line.  Note that a quote can succeed and still not be swappable —
 * both providers price routes they will not currently execute, and
 * blanking a monitor's history whenever a payout pool drains would be
 * worse than reporting the closure on @swappable and @warning.
 */
void pn_bridge_parse_reply (const PnBridgeRequest *request,
                            gint                   http_status,
                            const gchar           *body,
                            PnBridgeResult        *result);

G_END_DECLS

#endif /* PN_BRIDGE_COMMON_H */
