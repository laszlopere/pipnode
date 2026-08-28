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

/* ------------------------------------------------------------------ */
/*  Cross-chain bridge back-ends — shared logic tier.                  */
/*                                                                     */
/*  The provider tables and both parsers, with no #PnNode in sight:    */
/*  every function here takes a #PnBridgeRequest and fills a           */
/*  #PnBridgeResult.  #PnBridgeQuote and #PnBridgeConverter differ in  */
/*  what makes them ask — a timer versus an incoming message — and     */
/*  share everything that happens after the reply lands.               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-bridge-common.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

/* ------------------------------------------------------------------ */
/*  Provider table                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnBridge     id;
    const gchar *nick;         /* enum nick — also the dialog label */
    const gchar *display;      /* stamped on the message as `bridge` */
    const gchar *url;          /* default endpoint */
} BridgeInfo;

static const BridgeInfo bridge_table[] = {
    { PN_BRIDGE_PULSELN,   "PulseLN",   "PulseLN",
      "https://api.pulseln.com/c/prices" },
    { PN_BRIDGE_CHANGENOW, "ChangeNOW", "ChangeNOW",
      "https://api.changenow.io/v1/exchange-amount" },
};

/** ChangeNOW's range endpoint, a sibling of the quote endpoint above.
 *  Derived from the configured URL rather than hard-coded so re-pointing
 *  a node at a proxy moves both requests together. */
#define PN_CHANGENOW_RANGE_PATH "exchange-range"

/** Look up the table row for @bridge, falling back to the first row so
 *  callers can dereference the result unconditionally. */
static const BridgeInfo *
bridge_info (PnBridge bridge)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (bridge_table); i++)
        if (bridge_table[i].id == bridge)
            return &bridge_table[i];

    return &bridge_table[0];
}

GType
pn_bridge_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_BRIDGE_PULSELN,   "PN_BRIDGE_PULSELN",   "PulseLN"   },
            { PN_BRIDGE_CHANGENOW, "PN_BRIDGE_CHANGENOW", "ChangeNOW" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnBridge", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Ticker tables                                                      */
/*                                                                     */
/*  The bridge nodes reuse #PnCurrency so their settings dialogs get   */
/*  the same icon+ticker picker as the FX Converter, but neither       */
/*  bridge lists all 23 entries.  A %NULL cell means "this provider    */
/*  does not trade this asset", which is what marks a pair             */
/*  unsupported — better a legible "PulseLN does not list XMR" than a  */
/*  puzzling empty reply.                                             */
/*                                                                     */
/*  PulseLN symbols are uppercase and come from the `onlineList` its   */
/*  swap page ships; note MATIC maps to POL, Polygon's post-rebrand    */
/*  ticker.  ChangeNOW symbols are its lowercase *legacy* tickers,     */
/*  confirmed against /v1/currencies?active=true: mostly the obvious   */
/*  identity, but BNB is chain-qualified and DOT and USDT only exist   */
/*  under a chain suffix.  USD is a fiat pivot on the FX Converter,    */
/*  not a bridgeable asset, so it is %NULL on both.                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnCurrency   id;
    const gchar *pulseln;      /* NULL when not listed */
    const gchar *changenow;    /* NULL when not listed */
} TickerInfo;

static const TickerInfo ticker_table[] = {
    { PN_CURRENCY_ADA,   NULL,    "ada"        },
    { PN_CURRENCY_ATOM,  NULL,    "atom"       },
    { PN_CURRENCY_AVAX,  "AVAX",  "avaxc"      },
    { PN_CURRENCY_BCH,   NULL,    "bch"        },
    { PN_CURRENCY_BNB,   "BNB",   "bnbbsc"     },
    { PN_CURRENCY_BTC,   "BTC",   "btc"        },
    { PN_CURRENCY_CRO,   NULL,    "cro"        },
    { PN_CURRENCY_DOGE,  NULL,    "doge"       },
    { PN_CURRENCY_DOT,   NULL,    "dotbsc"     },
    { PN_CURRENCY_ETH,   "ETH",   "eth"        },
    { PN_CURRENCY_LINK,  NULL,    "link"       },
    { PN_CURRENCY_LTC,   NULL,    "ltc"        },
    { PN_CURRENCY_MATIC, "POL",   "matic"      },
    { PN_CURRENCY_PLS,   "PLS",   "pls"        },
    { PN_CURRENCY_SOL,   "SOL",   "sol"        },
    { PN_CURRENCY_TRX,   "TRX",   "trx"        },
    { PN_CURRENCY_UNI,   NULL,    "uni"        },
    { PN_CURRENCY_USD,   NULL,    NULL         },
    { PN_CURRENCY_USDC,  NULL,    "usdc"       },
    { PN_CURRENCY_USDT,  NULL,    "usdterc20"  },
    { PN_CURRENCY_XLM,   NULL,    "xlm"        },
    { PN_CURRENCY_XMR,   NULL,    "xmr"        },
    { PN_CURRENCY_XRP,   NULL,    "xrp"        },
};

const gchar *
pn_bridge_get_ticker (
        PnBridge   bridge,
        PnCurrency cur)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (ticker_table); i++)
    {
        if (ticker_table[i].id != cur)
            continue;

        return (bridge == PN_BRIDGE_CHANGENOW)
                ? ticker_table[i].changenow
                : ticker_table[i].pulseln;
    }

    /* An out-of-range currency is not listed anywhere, which is exactly
     * what a NULL means to every caller. */
    return NULL;
}

const gchar *
pn_bridge_get_display_name (PnBridge bridge)
{
    return bridge_info (bridge)->display;
}

const gchar *
pn_bridge_get_default_url (PnBridge bridge)
{
    return bridge_info (bridge)->url;
}

const gchar *
pn_bridge_currency_nick (PnCurrency cur)
{
    static GEnumClass *eclass = NULL;
    static gsize       once   = 0;
    GEnumValue        *value;

    /* The enum class ref is taken once and never released: the returned
     * string is owned by the class, so dropping the last ref would free
     * the nicks out from under every caller.  One held ref on a
     * process-wide singleton class, the same bargain
     * pn_node_factory_register makes. */
    if (g_once_init_enter (&once))
    {
        eclass = g_type_class_ref (PN_TYPE_CURRENCY);
        g_once_init_leave (&once, 1);
    }

    value = g_enum_get_value (eclass, (gint) cur);
    return (value != NULL && value->value_nick != NULL)
            ? value->value_nick : "?";
}

gboolean
pn_bridge_pair_is_supported (
        PnBridge   bridge,
        PnCurrency from,
        PnCurrency to)
{
    return (from != to) &&
           (pn_bridge_get_ticker (bridge, from) != NULL) &&
           (pn_bridge_get_ticker (bridge, to)   != NULL);
}

/* ------------------------------------------------------------------ */
/*  Result lifecycle                                                   */
/* ------------------------------------------------------------------ */

void
pn_bridge_result_init (PnBridgeResult *result)
{
    g_return_if_fail (result != NULL);

    memset (result, 0, sizeof *result);
    /* A route is open until a reply says otherwise: a parse that fails
     * before it reaches the closure checks must not invent a closure. */
    result->swappable = TRUE;
}

void
pn_bridge_result_clear (PnBridgeResult *result)
{
    g_return_if_fail (result != NULL);

    g_clear_pointer (&result->error, g_free);
    g_clear_pointer (&result->warning, g_free);
    g_clear_pointer (&result->speed_forecast, g_free);
    pn_bridge_result_init (result);
}

/** Record a failure on @result, replacing any earlier one. */
static void G_GNUC_PRINTF (2, 3)
result_fail (
        PnBridgeResult *result,
        const gchar    *fmt,
        ...)
{
    va_list ap;

    va_start (ap, fmt);
    g_free (result->error);
    result->error = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    result->ok = FALSE;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/*                                                                     */
/*  PulseLN quotes most numbers as strings and a few as JSON numbers    */
/*  in the same object, so every read goes through a reader that        */
/*  accepts either and reports a miss as NaN.  Callers test isfinite(). */
/* ------------------------------------------------------------------ */

static gdouble
json_number (
        JsonObject  *obj,
        const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || member == NULL ||
        !json_object_has_member (obj, member))
        return (gdouble) NAN;

    node = json_object_get_member (obj, member);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return (gdouble) NAN;

    if (json_node_get_value_type (node) == G_TYPE_STRING)
    {
        const gchar *text = json_node_get_string (node);
        gchar       *end  = NULL;
        gdouble      v;

        if (text == NULL || *text == '\0')
            return (gdouble) NAN;

        v = g_ascii_strtod (text, &end);
        return (end != NULL && *end == '\0') ? v : (gdouble) NAN;
    }

    return json_node_get_double (node);
}

static JsonObject *
json_object_member (
        JsonObject  *obj,
        const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member (obj, member))
        return NULL;

    node = json_object_get_member (obj, member);
    return (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
            ? json_node_get_object (node) : NULL;
}

static const gchar *
json_string (
        JsonObject  *obj,
        const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member (obj, member))
        return NULL;

    node = json_object_get_member (obj, member);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
        json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

/** TRUE when the string array at @member contains @needle.  Used for
 *  PulseLN's DLN_LIST, the set of payout assets that carry an extra flat
 *  fee on top of the percentage one. */
static gboolean
json_array_contains (
        JsonObject  *obj,
        const gchar *member,
        const gchar *needle)
{
    JsonNode  *node;
    JsonArray *array;
    guint      i, n;

    if (obj == NULL || needle == NULL ||
        !json_object_has_member (obj, member))
        return FALSE;

    node = json_object_get_member (obj, member);
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node))
        return FALSE;

    array = json_node_get_array (node);
    n     = json_array_get_length (array);
    for (i = 0; i < n; i++)
        if (g_strcmp0 (json_array_get_string_element (array, i), needle) == 0)
            return TRUE;

    return FALSE;
}

/** Store a published limit, treating anything that is not a positive
 *  finite number as "not published". */
static gdouble
sane_limit (gdouble value)
{
    return (isfinite (value) && value > 0.0) ? value : 0.0;
}

/* ------------------------------------------------------------------ */
/*  URL building                                                       */
/* ------------------------------------------------------------------ */

gchar *
pn_bridge_build_url (
        const PnBridgeRequest *request,
        const gchar           *base_url)
{
    const gchar *base = (base_url != NULL) ? base_url : "";

    g_return_val_if_fail (request != NULL, NULL);

    if (request->bridge == PN_BRIDGE_CHANGENOW)
    {
        const gchar *from_sym = pn_bridge_get_ticker (request->bridge,
                                                      request->from);
        const gchar *to_sym   = pn_bridge_get_ticker (request->bridge,
                                                      request->to);
        gchar        amount[G_ASCII_DTOSTR_BUF_SIZE];

        /* The amount rides in the path, so it must be formatted C-locale
         * — a comma decimal separator would silently corrupt the query. */
        g_ascii_formatd (amount, sizeof amount, "%.8g", request->amount);
        return g_strdup_printf ("%s/%s/%s_%s", base, amount,
                                from_sym, to_sym);
    }

    /* PulseLN takes no parameters: one blob carries every pair. */
    return g_strdup (base);
}

/* ------------------------------------------------------------------ */
/*  PulseLN back-end                                                   */
/*                                                                     */
/*  One keyless GET returns the entire cross-rate matrix, the fee       */
/*  table and the per-asset limits; there is no per-amount quote        */
/*  endpoint, so the payout is computed here exactly the way the swap   */
/*  page computes it in the browser:                                    */
/*                                                                     */
/*      rate = P[TO]["per"FROM]                                         */
/*             - P[FROM].diff * P[FROM].perUSD                          */
/*               / (o * P[FROM]["per"TO])                               */
/*      out  = amount * rate                                            */
/*      if (TO in DLN_LIST) out -= DLN_FEE * P[TO].perUSD               */
/*      out -= out * fee                                                */
/*                                                                     */
/*  where `o` is 1e8 for a BTC input (satoshi denomination) and 1       */
/*  otherwise, and `fee` is the pair override PF[FROM TO] or the global */
/*  default F.  The site additionally scales the network term by        */
/*  grn(DIFF) = |random()*DIFF - 100| / 100, which is 0.9999…1.0 —      */
/*  cosmetic jitter, so this uses 1.0 and stays deterministic.          */
/* ------------------------------------------------------------------ */

static void
pulseln_compute (
        const PnBridgeRequest *req,
        JsonObject            *root,
        PnBridgeResult        *result)
{
    const gchar *from_sym = pn_bridge_get_ticker (req->bridge, req->from);
    const gchar *to_sym   = pn_bridge_get_ticker (req->bridge, req->to);
    JsonObject  *from_obj;
    JsonObject  *to_obj;
    JsonObject  *status_obj;
    JsonObject  *pool_obj;
    JsonObject  *fee_obj;
    const gchar *deposit_state;
    const gchar *payout_state;
    gchar       *closed = NULL;   /* why the route is shut, or NULL */
    gchar       *per_from;
    gchar       *per_to;
    gchar       *pair_key;
    gdouble      to_per_from;
    gdouble      from_per_to;
    gdouble      from_diff;
    gdouble      from_per_usd;
    gdouble      fee;
    gdouble      denom;
    gdouble      rate;
    gdouble      out;

    from_obj = json_object_member (root, from_sym);
    to_obj   = json_object_member (root, to_sym);
    if (from_obj == NULL || to_obj == NULL)
    {
        result_fail (result, "PulseLN did not quote %s or %s",
                     from_sym, to_sym);
        return;
    }

    /* Whether the route is open for business.  STATUS covers the deposit
     * side, PS the payout pool; the swap page gates only its *button* on
     * these (`outPool`), and still prices the swap while showing "Swaps
     * To X Temporarily Offline".  A monitor should do the same: the rate
     * is well-defined and worth recording either way, and dropping it
     * would blank the history exactly when a pool is drained.  So the
     * quote is computed regardless and the closure is reported through
     * `swappable` and `warning` instead. */
    status_obj    = json_object_member (root, "STATUS");
    pool_obj      = json_object_member (root, "PS");
    deposit_state = json_string (status_obj, from_sym);
    payout_state  = json_string (pool_obj, to_sym);

    if (deposit_state != NULL && g_strcmp0 (deposit_state, "online") != 0)
        closed = g_strdup_printf ("PulseLN deposits in %s are %s",
                                  from_sym, deposit_state);
    else if (payout_state != NULL && g_strcmp0 (payout_state, "online") != 0)
        closed = g_strdup_printf ("PulseLN swaps to %s are temporarily "
                                  "offline", to_sym);

    per_from = g_strconcat ("per", from_sym, NULL);
    per_to   = g_strconcat ("per", to_sym, NULL);

    to_per_from  = json_number (to_obj, per_from);
    from_per_to  = json_number (from_obj, per_to);
    from_diff    = json_number (from_obj, "diff");
    from_per_usd = json_number (from_obj, "perUSD");

    /* The flat network fee is published per asset as diff x perUSD; when
     * either half is missing, fall back to the global DIFF, which the two
     * are defined to multiply out to. */
    if (!isfinite (from_diff) || !isfinite (from_per_usd))
    {
        from_diff    = json_number (root, "DIFF");
        from_per_usd = 1.0;
    }

    /* The pair override wins over the global default fee. */
    pair_key = g_strconcat (from_sym, to_sym, NULL);
    fee_obj  = json_object_member (root, "PF");
    fee      = json_number (fee_obj, pair_key);
    if (!isfinite (fee))
        fee = json_number (root, "F");
    if (!isfinite (fee))
        fee = 0.0;
    g_free (pair_key);

    /* A BTC input is denominated in satoshi in the network-fee term. */
    denom = (req->from == PN_CURRENCY_BTC) ? 1e8 : 1.0;

    if (!isfinite (to_per_from) || !isfinite (from_per_to) ||
        from_per_to == 0.0)
    {
        result_fail (result,
                     "no usable %s/%s cross-rate in the PulseLN reply",
                     from_sym, to_sym);
        g_free (per_from);
        g_free (per_to);
        g_free (closed);
        return;
    }

    rate = to_per_from;
    if (isfinite (from_diff) && isfinite (from_per_usd))
        rate -= from_diff * from_per_usd / (denom * from_per_to);

    out = req->amount * rate;

    /* Assets routed over the DLN carry an extra flat fee, quoted in USD
     * and converted into payout units. */
    if (json_array_contains (root, "DLN_LIST", to_sym))
    {
        gdouble dln_fee    = json_number (root, "DLN_FEE");
        gdouble to_per_usd = json_number (to_obj, "perUSD");

        if (isfinite (dln_fee) && isfinite (to_per_usd))
            out -= dln_fee * to_per_usd;
    }

    out -= out * fee;

    /* The site rounds to six decimals, and floors a BTC payout to whole
     * satoshi.  Mirror both so the number matches what the user sees. */
    out = round (out * 1e6) / 1e6;
    if (req->to == PN_CURRENCY_BTC)
        out = floor (out);
    if (!(out > 0.0))
        out = 0.0;

    result->fee = sane_limit (fee);

    /* Limits float: PulseLN derives them from a USD band (currently
     * "$1 MIN, $1000 MAX" on the swap page) divided by the live price,
     * so the same node can drift in and out of range without anything
     * being reconfigured.  An amount outside the band is priced exactly
     * the same way, but the provider will not execute it — the swap page
     * says "Max Limit Exceeded, Decrease Swap Amount" and greys its
     * button — so it lands on `swappable` alongside a closed route. */
    {
        gdouble min_amount = json_number (from_obj, "min");
        gdouble max_amount = json_number (from_obj, "max");

        result->min_amount = sane_limit (min_amount);
        result->max_amount = sane_limit (max_amount);

        if (closed == NULL && result->min_amount > 0.0 &&
            req->amount < result->min_amount)
            closed = g_strdup_printf ("below PulseLN's minimum of %.10g %s",
                                      result->min_amount, from_sym);
        else if (closed == NULL && result->max_amount > 0.0 &&
                 req->amount > result->max_amount)
            closed = g_strdup_printf ("above PulseLN's maximum of %.10g %s",
                                      result->max_amount, from_sym);
    }

    result->warning   = closed;          /* takes ownership */
    result->swappable = (closed == NULL);

    g_free (per_from);
    g_free (per_to);

    /* Limits are advisory here: PulseLN still returns a rate outside
     * them, so report the quote and let the caller carry the range. */
    if (out <= 0.0)
    {
        result_fail (result,
                     "PulseLN quoted nothing for %.10g %s — below the "
                     "network fee?", req->amount, from_sym);
        return;
    }

    result->quote = out;
    result->ok    = TRUE;
}

/* ------------------------------------------------------------------ */
/*  ChangeNOW back-end                                                 */
/*                                                                     */
/*  v1 is the whole story: its quote and range endpoints need no API    */
/*  key, while the v2 equivalents answer 401 without one.  The quote    */
/*  arrives on the caller's transport; the range needs a second GET,    */
/*  issued from pn_bridge_fetch_range on the same worker thread,        */
/*  because it is what turns a bare "max_amount_exceeded" into a        */
/*  sentence naming the actual cap.                                     */
/* ------------------------------------------------------------------ */

void
pn_bridge_fetch_range (
        const PnBridgeRequest *request,
        const gchar           *base_url,
        guint                  timeout_seconds,
        PnBridgeResult        *result)
{
    SoupSession *session;
    SoupMessage *msg;
    GBytes      *bytes;
    gchar       *parent;
    gchar       *pair;
    gchar       *url;
    JsonParser  *parser;
    JsonNode    *root;

    g_return_if_fail (request != NULL);
    g_return_if_fail (result != NULL);

    /* Only ChangeNOW publishes its limits on a separate endpoint. */
    if (request->bridge != PN_BRIDGE_CHANGENOW)
        return;
    if (base_url == NULL || *base_url == '\0')
        return;

    /* The range endpoint is a sibling of the quote endpoint, so derive it
     * from the configured URL rather than hard-coding the host — a node
     * re-pointed at a proxy keeps both halves together. */
    pair   = g_strdup_printf ("%s_%s",
                              pn_bridge_get_ticker (request->bridge,
                                                    request->from),
                              pn_bridge_get_ticker (request->bridge,
                                                    request->to));
    parent = g_path_get_dirname (base_url);
    url    = g_strdup_printf ("%s/%s/%s", parent,
                              PN_CHANGENOW_RANGE_PATH, pair);
    g_free (parent);
    g_free (pair);

    msg = soup_message_new (SOUP_METHOD_GET, url);
    g_free (url);
    if (msg == NULL)
        return;

    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "Accept", "application/json");

    session = soup_session_new ();
    soup_session_set_timeout (session, timeout_seconds);
    bytes = soup_session_send_and_read (session, msg, NULL, NULL);

    if (bytes != NULL)
    {
        gsize        len  = 0;
        const gchar *data = g_bytes_get_data (bytes, &len);
        gchar       *body = g_strndup (data ? data : "", len);

        parser = json_parser_new ();
        if (json_parser_load_from_data (parser, body, -1, NULL))
        {
            root = json_parser_get_root (parser);
            if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            {
                JsonObject *obj = json_node_get_object (root);

                result->min_amount =
                        sane_limit (json_number (obj, "minAmount"));
                result->max_amount =
                        sane_limit (json_number (obj, "maxAmount"));
            }
        }
        g_object_unref (parser);
        g_free (body);
        g_bytes_unref (bytes);
    }

    g_object_unref (msg);
    g_object_unref (session);
}

/** Turn one of ChangeNOW's error codes into a sentence.  The codes are
 *  terse and machine-shaped; the range already in @result lets the two
 *  limit errors name the number the user has to move to. */
static gchar *
changenow_error_text (
        const PnBridgeResult *result,
        const gchar          *code,
        const gchar          *message)
{
    if (g_strcmp0 (code, "max_amount_exceeded") == 0 &&
        result->max_amount > 0.0)
        return g_strdup_printf ("ChangeNOW caps this pair at %.10g; "
                                "lower the amount", result->max_amount);
    if (g_strcmp0 (code, "deposit_too_small") == 0 &&
        result->min_amount > 0.0)
        return g_strdup_printf ("ChangeNOW needs at least %.10g; "
                                "raise the amount", result->min_amount);
    if (g_strcmp0 (code, "pair_is_inactive") == 0)
        return g_strdup ("ChangeNOW is not trading this pair right now");

    if (message != NULL && *message != '\0')
        return g_strdup_printf ("ChangeNOW: %s", message);

    return g_strdup_printf ("ChangeNOW: %s", code ? code : "unknown error");
}

static void
changenow_compute (
        const PnBridgeRequest *req,
        JsonObject            *root,
        PnBridgeResult        *result)
{
    const gchar *code = json_string (root, "error");
    gdouble      estimated;

    (void) req;

    if (code != NULL)
    {
        gchar *text = changenow_error_text (result, code,
                                            json_string (root, "message"));

        result_fail (result, "%s", text);
        g_free (text);
        return;
    }

    estimated = json_number (root, "estimatedAmount");
    if (!isfinite (estimated) || estimated <= 0.0)
    {
        result_fail (result, "no usable estimate in the ChangeNOW reply");
        return;
    }

    result->speed_forecast =
            g_strdup (json_string (root, "transactionSpeedForecast"));
    result->warning = g_strdup (json_string (root, "warningMessage"));

    /* v1 does not publish the provider's cut separately; leaving the fee
     * at zero keeps it off the message rather than claiming it is free. */
    result->fee = 0.0;

    /* ChangeNOW simply refuses to quote a pair it is not trading
     * (`pair_is_inactive`, handled above), so a number here means the
     * route is open. */
    result->swappable = TRUE;

    result->quote = estimated;
    result->ok    = TRUE;
}

/* ------------------------------------------------------------------ */
/*  Reply dispatch                                                     */
/* ------------------------------------------------------------------ */

void
pn_bridge_parse_reply (
        const PnBridgeRequest *request,
        gint                   http_status,
        const gchar           *body,
        PnBridgeResult        *result)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    GError     *error = NULL;

    g_return_if_fail (request != NULL);
    g_return_if_fail (result != NULL);

    if (body == NULL || *body == '\0')
    {
        result_fail (result, "empty reply from %s",
                     pn_bridge_get_display_name (request->bridge));
        return;
    }

    /* ChangeNOW answers a rejected amount with HTTP 400 and a JSON error
     * body, so its status is judged by the parse, not by the code — a
     * bare "HTTP 400" would hide the actual reason.  PulseLN has no error
     * body worth parsing, so for it a non-2xx is the whole message. */
    if (request->bridge != PN_BRIDGE_CHANGENOW &&
        (http_status < 200 || http_status >= 300))
    {
        result_fail (result, "PulseLN returned HTTP %d", http_status);
        return;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body, -1, &error))
    {
        result_fail (result, "could not parse the %s reply: %s",
                     pn_bridge_get_display_name (request->bridge),
                     error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        result_fail (result, "unexpected shape in the %s reply",
                     pn_bridge_get_display_name (request->bridge));
        g_object_unref (parser);
        return;
    }
    obj = json_node_get_object (root);

    if (request->bridge == PN_BRIDGE_CHANGENOW)
        changenow_compute (request, obj, result);
    else
        pulseln_compute (request, obj, result);

    g_object_unref (parser);
}
