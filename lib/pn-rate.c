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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rate.h"
#include "pn-message.h"

#include <math.h>

#include <json-glib/json-glib.h>

/* Visual identity.  fa-exchange (U+F0EC) — two-way arrows for the
 * conversion theme. */
#define PN_RATE_NORMAL_ICON "\xef\x83\xac"

/* Rate refreshes are gated to a 10-minute floor.  CoinGecko's free
 * tier rate-limits to ~10–30 req/min globally per IP; with several
 * Rate nodes on a worksheet (one per pair the user is tracking) a
 * tighter polling interval starts to draw 429s and short bans, and
 * the conversion factors we deal in (USD/ETH, USD/BTC, USD/PLS) do
 * not move fast enough on a sub-10-minute window for a tighter
 * cadence to be useful. */
#define PN_RATE_PERIOD_MIN     600u
#define PN_RATE_PERIOD_DEFAULT 600u

/* CoinGecko's free `simple/price` endpoint — comma-separated coin
 * IDs in `ids`, comma-separated quote currencies in `vs_currencies`.
 * The default is exposed via the inherited PnHttp:url so the user
 * can re-point at a self-hosted proxy if they hit rate-limits. */
#define PN_RATE_DEFAULT_ENDPOINT \
    "https://api.coingecko.com/api/v3/simple/price"

/* ------------------------------------------------------------------ */
/*  Currency table                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnCurrency   id;
    const gchar *nick;       /* enum nick — also the dialog label */
    const gchar *coingecko;  /* CoinGecko coin id; NULL for USD pivot */
    const gchar *icon;       /* basename (no extension) of the bundled
                              * 128×128 PNG under $datadir/icons/ */
} CurrencyInfo;

static const CurrencyInfo currency_table[] = {
    { PN_CURRENCY_ADA,   "ADA",   "cardano",          "ada"   },
    { PN_CURRENCY_ATOM,  "ATOM",  "cosmos",           "atom"  },
    { PN_CURRENCY_AVAX,  "AVAX",  "avalanche-2",      "avax"  },
    { PN_CURRENCY_BCH,   "BCH",   "bitcoin-cash",     "bch"   },
    { PN_CURRENCY_BNB,   "BNB",   "binancecoin",      "bnb"   },
    { PN_CURRENCY_BTC,   "BTC",   "bitcoin",          "btc"   },
    { PN_CURRENCY_CRO,   "CRO",   "crypto-com-chain", "cro"   },
    { PN_CURRENCY_DOGE,  "DOGE",  "dogecoin",         "doge"  },
    { PN_CURRENCY_DOT,   "DOT",   "polkadot",         "dot"   },
    { PN_CURRENCY_ETH,   "ETH",   "ethereum",         "eth"   },
    { PN_CURRENCY_LINK,  "LINK",  "chainlink",        "link"  },
    { PN_CURRENCY_LTC,   "LTC",   "litecoin",         "ltc"   },
    { PN_CURRENCY_MATIC, "MATIC", "matic-network",    "matic" },
    { PN_CURRENCY_PLS,   "PLS",   "pulsechain",       "pls"   },
    { PN_CURRENCY_SOL,   "SOL",   "solana",           "sol"   },
    { PN_CURRENCY_TRX,   "TRX",   "tron",             "trx"   },
    { PN_CURRENCY_UNI,   "UNI",   "uniswap",          "uni"   },
    { PN_CURRENCY_USD,   "USD",   NULL,               "usd"   },
    { PN_CURRENCY_USDC,  "USDC",  "usd-coin",         "usdc"  },
    { PN_CURRENCY_USDT,  "USDT",  "tether",           "usdt"  },
    { PN_CURRENCY_XLM,   "XLM",   "stellar",          "xlm"   },
    { PN_CURRENCY_XMR,   "XMR",   "monero",           "xmr"   },
    { PN_CURRENCY_XRP,   "XRP",   "ripple",           "xrp"   },
};

/** Look up the table entry for @cur.  Falls back to the USD row when
 *  asked about an out-of-range value so callers can dereference the
 *  return value unconditionally — USD's NULL `coingecko` field is the
 *  one row that build_command and friends already special-case (it is
 *  the pivot, not a fetched coin), which makes it the safest miss
 *  value across every consumer. */
static const CurrencyInfo *
currency_info (PnCurrency cur)
{
    guint i;
    const CurrencyInfo *fallback = &currency_table[0];

    for (i = 0; i < G_N_ELEMENTS (currency_table); i++)
    {
        if (currency_table[i].id == cur)
            return &currency_table[i];
        if (currency_table[i].id == PN_CURRENCY_USD)
            fallback = &currency_table[i];
    }

    return fallback;
}

GType
pn_currency_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_CURRENCY_ADA,   "PN_CURRENCY_ADA",   "ADA"   },
            { PN_CURRENCY_ATOM,  "PN_CURRENCY_ATOM",  "ATOM"  },
            { PN_CURRENCY_AVAX,  "PN_CURRENCY_AVAX",  "AVAX"  },
            { PN_CURRENCY_BCH,   "PN_CURRENCY_BCH",   "BCH"   },
            { PN_CURRENCY_BNB,   "PN_CURRENCY_BNB",   "BNB"   },
            { PN_CURRENCY_BTC,   "PN_CURRENCY_BTC",   "BTC"   },
            { PN_CURRENCY_CRO,   "PN_CURRENCY_CRO",   "CRO"   },
            { PN_CURRENCY_DOGE,  "PN_CURRENCY_DOGE",  "DOGE"  },
            { PN_CURRENCY_DOT,   "PN_CURRENCY_DOT",   "DOT"   },
            { PN_CURRENCY_ETH,   "PN_CURRENCY_ETH",   "ETH"   },
            { PN_CURRENCY_LINK,  "PN_CURRENCY_LINK",  "LINK"  },
            { PN_CURRENCY_LTC,   "PN_CURRENCY_LTC",   "LTC"   },
            { PN_CURRENCY_MATIC, "PN_CURRENCY_MATIC", "MATIC" },
            { PN_CURRENCY_PLS,   "PN_CURRENCY_PLS",   "PLS"   },
            { PN_CURRENCY_SOL,   "PN_CURRENCY_SOL",   "SOL"   },
            { PN_CURRENCY_TRX,   "PN_CURRENCY_TRX",   "TRX"   },
            { PN_CURRENCY_UNI,   "PN_CURRENCY_UNI",   "UNI"   },
            { PN_CURRENCY_USD,   "PN_CURRENCY_USD",   "USD"   },
            { PN_CURRENCY_USDC,  "PN_CURRENCY_USDC",  "USDC"  },
            { PN_CURRENCY_USDT,  "PN_CURRENCY_USDT",  "USDT"  },
            { PN_CURRENCY_XLM,   "PN_CURRENCY_XLM",   "XLM"   },
            { PN_CURRENCY_XMR,   "PN_CURRENCY_XMR",   "XMR"   },
            { PN_CURRENCY_XRP,   "PN_CURRENCY_XRP",   "XRP"   },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnCurrency", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnRate
{
    PnHttp parent_instance;

    /* @from / @to are read on the worker thread (build_command,
     * emit_message) and written by the main-thread property setter,
     * so accesses are guarded by @mutex.  @rate / @last_update are
     * read by the main thread on receive and written by the worker
     * after each successful fetch, so they ride the same mutex. */
    GMutex      mutex;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     rate;          /* multiplier from FROM units to TO units */
    gchar      *last_update;   /* ISO-8601 string; NULL until first save */

    /* Re-entrancy guard for the period clamp.  When a notify::period
     * fires below the floor, we re-set the property; that re-set
     * fires another notify, which we have to ignore to avoid an
     * infinite recursion. */
    gboolean    period_clamping;
};

G_DEFINE_TYPE (PnRate, pn_rate, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_FROM,
    PROP_TO,
    PROP_RATE,
    PROP_LAST_UPDATE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  State accessors (thread-safe)                                      */
/* ------------------------------------------------------------------ */

static PnCurrency
rate_get_from_locked (PnRate *self)
{
    PnCurrency v;
    g_mutex_lock (&self->mutex);
    v = self->from;
    g_mutex_unlock (&self->mutex);
    return v;
}

static PnCurrency
rate_get_to_locked (PnRate *self)
{
    PnCurrency v;
    g_mutex_lock (&self->mutex);
    v = self->to;
    g_mutex_unlock (&self->mutex);
    return v;
}

static gdouble
rate_get_rate_locked (PnRate *self)
{
    gdouble v;
    g_mutex_lock (&self->mutex);
    v = self->rate;
    g_mutex_unlock (&self->mutex);
    return v;
}

/* ------------------------------------------------------------------ */
/*  Trigger override: cache-aware fetch gate                          */
/*                                                                     */
/*  The base #PnAutoTrigger now fires its first tick ~1s after        */
/*  construction (so long-period nodes do not stay blank after a      */
/*  worksheet load), and then every #PnAutoTrigger:period seconds.    */
/*  Without a gate, every worksheet reopen would burn a CoinGecko    */
/*  request even when the persisted `last-update` is still well        */
/*  within @period — defeating the whole point of caching the rate    */
/*  in the worksheet (and a fast route to a 429 rate-limit ban on a    */
/*  worksheet with several Rate nodes).                               */
/*                                                                     */
/*  This override inspects the cached `last-update` against @period   */
/*  and short-circuits the tick when the cache is still fresh; only   */
/*  a missing, unparseable or genuinely stale timestamp chains to      */
/*  PnHttp::trigger to actually run curl.  Pair changes (FROM / TO)   */
/*  clear `last-update` in the property setter so the kick that       */
/*  follows correctly falls through to a fetch.                       */
/* ------------------------------------------------------------------ */

static gboolean
rate_cache_is_fresh (PnRate *self,
                     guint   period)
{
    gchar     *iso = NULL;
    GDateTime *then;
    GDateTime *now;
    GTimeSpan  elapsed;
    gint64     elapsed_s;
    gboolean   fresh;

    g_mutex_lock (&self->mutex);
    if (self->last_update != NULL)
        iso = g_strdup (self->last_update);
    g_mutex_unlock (&self->mutex);

    if (iso == NULL || *iso == '\0')
        return FALSE;

    then = g_date_time_new_from_iso8601 (iso, NULL);
    g_free (iso);
    /* Unparseable timestamp: treat as stale so we recover from a
     * hand-edited save file with a typo'd date. */
    if (then == NULL)
        return FALSE;

    now       = g_date_time_new_now_utc ();
    elapsed   = g_date_time_difference (now, then);
    elapsed_s = elapsed / G_TIME_SPAN_SECOND;
    fresh     = (elapsed_s >= 0) && ((guint64) elapsed_s < period);

    g_date_time_unref (now);
    g_date_time_unref (then);
    return fresh;
}

static void
pn_rate_trigger (PnAutoTrigger *trigger)
{
    PnRate                   *self   = PN_RATE (trigger);
    PnAutoTriggerClass       *parent = PN_AUTO_TRIGGER_CLASS (pn_rate_parent_class);
    guint                     period = pn_auto_trigger_get_period (trigger);

    if (rate_cache_is_fresh (self, period))
        return;

    if (parent->trigger != NULL)
        parent->trigger (trigger);
}

/* ------------------------------------------------------------------ */
/*  Period floor enforcement                                           */
/* ------------------------------------------------------------------ */

/** Re-clamp #PnAutoTrigger:period to the 10-minute floor every time
 *  it changes.  The base property's pspec carries the global 1 s
 *  minimum, so a deserialiser or a user dialing the spinner down can
 *  drop it below our floor; this notify hook catches both. */
static void
on_period_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnRate *self = PN_RATE (object);
    guint   period;

    (void) pspec;
    (void) user_data;

    if (self->period_clamping)
        return;

    g_object_get (object, "period", &period, NULL);
    if (period < PN_RATE_PERIOD_MIN)
    {
        self->period_clamping = TRUE;
        g_object_set (object, "period", PN_RATE_PERIOD_MIN, NULL);
        self->period_clamping = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  PnHttpClass overrides                                              */
/* ------------------------------------------------------------------ */

/** Configured iff the URL is non-empty AND the from/to pair is not
 *  the degenerate same-currency case (which would otherwise burn a
 *  request per period to learn that 1 USD is 1 USD). */
static gboolean
pn_rate_is_configured (PnHttp *http)
{
    PnRate   *self = PN_RATE (http);
    gchar    *url  = pn_http_dup_url (http);
    gboolean  ok   = (url != NULL && *url != '\0' &&
                      rate_get_from_locked (self) !=
                      rate_get_to_locked   (self));

    g_free (url);
    return ok;
}

/** Build the curl command.  CoinGecko's `simple/price` returns one
 *  USD price per requested coin id; we always pivot through USD so a
 *  single request is enough no matter which two of our currencies
 *  the user picked.  The helper appends each non-USD currency in
 *  {from, to} to the comma-separated `ids=` list. */
static gchar *
pn_rate_build_command (
        PnHttp *http,
        guint   timeout)
{
    PnRate              *self  = PN_RATE (http);
    gchar               *url   = pn_http_dup_url (http);
    PnCurrency           from  = rate_get_from_locked (self);
    PnCurrency           to    = rate_get_to_locked   (self);
    const CurrencyInfo  *finfo = currency_info (from);
    const CurrencyInfo  *tinfo = currency_info (to);
    GString             *ids   = g_string_new (NULL);
    gchar               *full_url;
    gchar               *quoted_url;
    gchar               *cmd;

    if (finfo->coingecko != NULL)
        g_string_append (ids, finfo->coingecko);

    if (tinfo->coingecko != NULL && tinfo->id != finfo->id)
    {
        if (ids->len > 0)
            g_string_append_c (ids, ',');
        g_string_append (ids, tinfo->coingecko);
    }

    /* is_configured already rules out from == to and an empty URL,
     * so we always have at least one id to query here. */
    full_url = g_strdup_printf ("%s?ids=%s&vs_currencies=usd",
                                url ? url : "",
                                ids->str);

    quoted_url = g_shell_quote (full_url);
    cmd = g_strdup_printf (
            "curl --silent --show-error --location "
            "--max-time %u "
            "-H 'Accept: application/json' "
            "%s",
            timeout, quoted_url);

    g_free (quoted_url);
    g_free (full_url);
    g_string_free (ids, TRUE);
    g_free (url);
    return cmd;
}

/** Pull the USD price for @cur out of CoinGecko's `simple/price`
 *  reply object.  Returns %NaN when the value is missing or the
 *  shape is unexpected. */
static gdouble
extract_usd_price (
        JsonObject *root,
        PnCurrency  cur)
{
    const CurrencyInfo *info = currency_info (cur);
    JsonObject         *coin;

    /* USD against itself is the pivot constant. */
    if (info->coingecko == NULL)
        return 1.0;

    if (!json_object_has_member (root, info->coingecko))
        return (gdouble) NAN;

    {
        JsonNode *node = json_object_get_member (root, info->coingecko);
        if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
            return (gdouble) NAN;
        coin = json_node_get_object (node);
    }

    if (!json_object_has_member (coin, "usd"))
        return (gdouble) NAN;

    return json_object_get_double_member (coin, "usd");
}

/** Parse the JSON-RPC reply, recompute the rate, and stash it under
 *  the mutex.  We deliberately do not emit a downstream message: the
 *  rate update is internal state, the conversion happens on the
 *  receive path, and emitting a "rate ticked" event each period
 *  would surprise downstream sinks that expect a 1:1 relationship
 *  between input and output messages on this node. */
static void
pn_rate_emit_message (
        PnHttp      *http,
        gboolean     spawned,
        gint         exit_status,
        const gchar *stdout_text,
        const gchar *stderr_text)
{
    PnRate     *self = PN_RATE (http);
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    GError     *error    = NULL;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     price_from;
    gdouble     price_to;
    gdouble     new_rate;

    (void) stderr_text;

    if (!spawned || !g_spawn_check_wait_status (exit_status, NULL))
        return;
    if (stdout_text == NULL || *stdout_text == '\0')
        return;

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, stdout_text, -1, &error))
    {
        g_warning ("pn-rate: parse error: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_object_unref (parser);
        return;
    }
    obj = json_node_get_object (root);

    from       = rate_get_from_locked (self);
    to         = rate_get_to_locked   (self);
    price_from = extract_usd_price (obj, from);
    price_to   = extract_usd_price (obj, to);

    if (!isfinite (price_from) || !isfinite (price_to) || price_to == 0.0)
    {
        g_object_unref (parser);
        return;
    }

    new_rate = price_from / price_to;

    {
        GDateTime *now = g_date_time_new_now_local ();
        gchar     *iso = g_date_time_format_iso8601 (now);

        g_mutex_lock (&self->mutex);
        self->rate = new_rate;
        g_free (self->last_update);
        self->last_update = iso;     /* takes ownership */
        g_mutex_unlock (&self->mutex);

        g_date_time_unref (now);
    }

    /* Notifications are intentionally NOT fired here: they would run
     * on the worker thread and could trip GTK from inside any
     * dialog binding currently watching these properties.  The
     * worksheet save path reads the new state through the
     * mutex-guarded getter, and the dialog re-reads on next open. */

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass.receive                                                */
/* ------------------------------------------------------------------ */

/** Multiply the incoming `data.value` by the cached rate and forward.
 *  A non-numeric or missing value is left alone — the message still
 *  passes through so chains downstream of an unconfigured-yet rate
 *  node do not stall.  We also stamp the resulting message with the
 *  conversion details (`rate`, `currency`) so downstream Format /
 *  Debug nodes can render a properly-labelled value without re-
 *  reaching into this node's settings. */
static void
pn_rate_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRate              *self = PN_RATE (node);
    JsonNode            *value_node;
    PnCurrency           to;
    const CurrencyInfo  *tinfo;
    gdouble              rate;

    g_mutex_lock (&self->mutex);
    rate = self->rate;
    to   = self->to;
    g_mutex_unlock (&self->mutex);

    tinfo = currency_info (to);

    value_node = pn_message_get_member (message, "value");
    if (value_node != NULL && JSON_NODE_HOLDS_VALUE (value_node))
    {
        GType vt = json_node_get_value_type (value_node);

        if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
        {
            gdouble v = json_node_get_double (value_node);
            pn_message_set_double (message, "value", v * rate);
        }
    }

    pn_message_set_double (message, "rate",     rate);
    pn_message_set_string (message, "currency", tinfo->nick);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_rate_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnRate *self = PN_RATE (object);

    switch (prop_id)
    {
    case PROP_FROM:
        g_value_set_enum (value, rate_get_from_locked (self));
        break;
    case PROP_TO:
        g_value_set_enum (value, rate_get_to_locked (self));
        break;
    case PROP_RATE:
        g_value_set_double (value, rate_get_rate_locked (self));
        break;
    case PROP_LAST_UPDATE:
        g_mutex_lock (&self->mutex);
        g_value_set_string (value,
                            self->last_update ? self->last_update : "");
        g_mutex_unlock (&self->mutex);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_rate_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnRate  *self = PN_RATE (object);
    PnHttp  *http = PN_HTTP (self);

    switch (prop_id)
    {
    case PROP_FROM:
    case PROP_TO:
    {
        PnCurrency  new_value = (PnCurrency) g_value_get_enum (value);
        gboolean    changed;

        g_mutex_lock (&self->mutex);
        if (prop_id == PROP_FROM)
        {
            changed = (self->from != new_value);
            self->from = new_value;
        }
        else
        {
            changed = (self->to != new_value);
            self->to = new_value;
        }
        if (changed)
        {
            /* Cache is keyed on the (from, to) pair; the rate we stored
             * a moment ago is the answer to a different question now.
             * Drop the timestamp so the cache-freshness gate in
             * pn_rate_trigger falls through to a real fetch — the
             * kick below would otherwise be silently swallowed when
             * the previous fetch happened within @period seconds. */
            g_free (self->last_update);
            self->last_update = NULL;
        }
        g_mutex_unlock (&self->mutex);

        pn_http_apply_visual_state (
                http,
                PN_HTTP_GET_CLASS (self)->is_configured (http));

        /* Picking a different pair invalidates the cached rate,
         * which is for the previous (from, to).  Kick the worker so
         * a fresh fetch lands within seconds rather than waiting
         * out the full 10-minute period — the user almost always
         * wants to see the new conversion immediately, and the
         * 10-minute floor is there to protect against polling
         * spam, not against deliberate user-driven refreshes. */
        if (changed && PN_HTTP_GET_CLASS (self)->is_configured (http))
            pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
        break;
    }
    case PROP_RATE:
        g_mutex_lock (&self->mutex);
        self->rate = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_LAST_UPDATE:
        g_mutex_lock (&self->mutex);
        g_free (self->last_update);
        self->last_update = g_value_dup_string (value);
        g_mutex_unlock (&self->mutex);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_rate_finalize (GObject *object)
{
    PnRate *self = PN_RATE (object);

    g_clear_pointer (&self->last_update, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_rate_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_property_editor override        */
/*                                                                     */
/*  Three of #PnRate's four properties want a non-default editor:      */
/*                                                                     */
/*    * `rate` and `last-update` are written by the node itself        */
/*      (cached fetch state) and the user is meant to read, not edit   */
/*      them.  A spinbutton with the default 2 decimal digits renders  */
/*      the PLS→USD rate of ~7.5e-06 as a misleading "0.00" and a      */
/*      string entry is no protection against accidentally clobbering  */
/*      the timestamp; route both through a read-only label renderer   */
/*      (%g for the double, verbatim for the string).                  */
/*                                                                     */
/*    * `from` and `to` are #PnCurrency enums; the default enum combo  */
/*      shows just the ticker, but a richer combo with the currency's  */
/*      icon next to the ticker is faster for the eye to scan when     */
/*      picking from 23 entries.  Icons live as PNGs under             */
/*      $datadir/pipnode/icons/<nick>.png (or in the in-tree           */
/*      data/icons/ when running uninstalled); a missing PNG is shown  */
/*      as a blank gap so the rest of the rows still line up.          */
/*                                                                     */
/*  Pre-2.0 all three lived as PN_IS_RATE branches in the host's       */
/*  pn-node-dialog.c.                                                  */
/* ------------------------------------------------------------------ */

/* --- Read-only label editor for `rate` / `last-update` ----------- */

typedef struct
{
    GObject     *target;        /* borrowed */
    const gchar *property;      /* static, from pspec->name */
    GType        ptype;         /* property's value type */
    GtkLabel    *label;
    gulong       notify_handler;
} PnReadOnlyBinding;

static void
readonly_binding_free (gpointer data)
{
    PnReadOnlyBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
readonly_binding_pull (PnReadOnlyBinding *bind)
{
    if (bind->ptype == G_TYPE_DOUBLE || bind->ptype == G_TYPE_FLOAT)
    {
        gdouble v = 0.0;
        gchar   buf[G_ASCII_DTOSTR_BUF_SIZE];

        g_object_get (bind->target, bind->property, &v, NULL);
        g_ascii_formatd (buf, sizeof buf, "%g", v);
        gtk_label_set_text (bind->label, buf);
    }
    else if (bind->ptype == G_TYPE_STRING)
    {
        gchar *v = NULL;

        g_object_get (bind->target, bind->property, &v, NULL);
        gtk_label_set_text (bind->label, v != NULL ? v : "");
        g_free (v);
    }
    else
    {
        GValue value = G_VALUE_INIT;
        gchar *text;

        g_value_init (&value, bind->ptype);
        g_object_get_property (bind->target, bind->property, &value);
        text = g_strdup_value_contents (&value);
        gtk_label_set_text (bind->label, text != NULL ? text : "");
        g_free (text);
        g_value_unset (&value);
    }
}

static void
on_readonly_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    readonly_binding_pull (user_data);
}

static GtkWidget *
build_readonly_label_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    GtkWidget         *label = gtk_label_new ("");
    PnReadOnlyBinding *bind;
    gchar             *signal_name;

    gtk_label_set_xalign     (GTK_LABEL (label), 0.0);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    /* The mono font matches the look of an editor, makes wide
     * scientific-notation values line up across rows, and visually
     * distinguishes read-only displays from editable entries. */
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_family_new ("Monospace"));
        gtk_label_set_attributes (GTK_LABEL (label), attrs);
        pango_attr_list_unref (attrs);
    }

    bind = g_new0 (PnReadOnlyBinding, 1);
    bind->target   = target;
    bind->property = pspec->name;
    bind->ptype    = G_PARAM_SPEC_VALUE_TYPE (pspec);
    bind->label    = GTK_LABEL (label);

    readonly_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", pspec->name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_readonly_target_notify), bind);
    g_free (signal_name);

    g_object_set_data_full (G_OBJECT (label),
                            "pn-readonly-binding",
                            bind, readonly_binding_free);

    return label;
}

/* --- Currency editor for `from` / `to` --------------------------- */

typedef struct
{
    GObject     *target;        /* borrowed */
    const gchar *property;      /* static: pspec->name */
    GtkComboBox *combo;
    gulong       notify_handler;
    gboolean     updating;
} PnCurrencyBinding;

static void
currency_binding_free (gpointer data)
{
    PnCurrencyBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
currency_binding_pull (PnCurrencyBinding *bind)
{
    gint   current = 0;
    gchar *id;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &current, NULL);
    id = g_strdup_printf ("%d", current);

    bind->updating = TRUE;
    gtk_combo_box_set_active_id (bind->combo, id);
    bind->updating = FALSE;

    g_free (id);
}

static void
on_currency_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    currency_binding_pull (user_data);
}

static void
on_currency_combo_changed (
        GtkComboBox *combo,
        gpointer     user_data)
{
    PnCurrencyBinding *bind = user_data;
    const gchar       *id;

    if (bind->updating)
        return;

    id = gtk_combo_box_get_active_id (combo);
    if (id == NULL)
        return;

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, atoi (id), NULL);
    bind->updating = FALSE;
}

static gchar *
resolve_currency_icon_path (const gchar *basename)
{
    gchar *path;

#ifdef SRCDIR
    path = g_build_filename (SRCDIR, "data", "icons", basename, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
#endif

#ifdef PKGDATADIR
    path = g_build_filename (PKGDATADIR, "icons", basename, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
#endif

    return NULL;
}

static GdkPixbuf *
load_currency_icon (const gchar *nick)
{
    gchar     *filename;
    gchar     *path;
    GdkPixbuf *pix = NULL;

    if (nick == NULL || *nick == '\0')
        return NULL;

    filename = g_strdup_printf ("%s.png", nick);
    path = resolve_currency_icon_path (filename);
    g_free (filename);

    if (path != NULL)
    {
        pix = gdk_pixbuf_new_from_file_at_size (path, 20, 20, NULL);
        g_free (path);
    }

    return pix;
}

static GtkWidget *
build_currency_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    enum { COL_ID, COL_LABEL, COL_PIX, N_COLS };
    GType              ptype    = G_PARAM_SPEC_VALUE_TYPE (pspec);
    const gchar       *name     = pspec->name;
    gboolean           writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkListStore      *store    = gtk_list_store_new (
            N_COLS, G_TYPE_STRING, G_TYPE_STRING, GDK_TYPE_PIXBUF);
    GtkWidget         *combo    = gtk_combo_box_new_with_model (
            GTK_TREE_MODEL (store));
    GtkCellRenderer   *pix_r    = gtk_cell_renderer_pixbuf_new ();
    GtkCellRenderer   *txt_r    = gtk_cell_renderer_text_new ();
    GEnumClass        *eclass   = g_type_class_ref (ptype);
    PnCurrencyBinding *bind;
    gchar             *signal_name;
    guint              i;

    gtk_combo_box_set_id_column (GTK_COMBO_BOX (combo), COL_ID);

    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), pix_r, FALSE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo),
                                   pix_r, "pixbuf", COL_PIX);

    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), txt_r, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo),
                                   txt_r, "text", COL_LABEL);

    for (i = 0; i < eclass->n_values; i++)
    {
        const GEnumValue *v   = &eclass->values[i];
        const gchar      *lbl = (v->value_nick != NULL && *v->value_nick != '\0')
                                    ? v->value_nick : v->value_name;
        gchar            *id  = g_strdup_printf ("%d", v->value);
        GdkPixbuf        *pix = load_currency_icon (
                pn_currency_get_icon_name ((PnCurrency) v->value));
        GtkTreeIter       iter;

        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter,
                            COL_ID,    id,
                            COL_LABEL, lbl,
                            COL_PIX,   pix,
                            -1);
        if (pix != NULL)
            g_object_unref (pix);
        g_free (id);
    }

    g_type_class_unref (eclass);
    g_object_unref (store);

    bind = g_new0 (PnCurrencyBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->combo    = GTK_COMBO_BOX (combo);

    currency_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_currency_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (combo, "changed",
                          G_CALLBACK (on_currency_combo_changed), bind);

    gtk_widget_set_sensitive (combo, writable);

    g_object_set_data_full (G_OBJECT (combo),
                            "pn-currency-binding",
                            bind, currency_binding_free);

    return combo;
}

static GtkWidget *
pn_rate_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                               GParamSpec  *pspec,
                               GObject     *target,
                               GtkWindow   *parent    G_GNUC_UNUSED)
{
    const gchar *name  = pspec->name;
    GType        ptype = G_PARAM_SPEC_VALUE_TYPE (pspec);

    if (g_strcmp0 (name, "rate")        == 0 ||
        g_strcmp0 (name, "last-update") == 0)
        return build_readonly_label_editor (target, pspec);

    if ((g_strcmp0 (name, "from") == 0 || g_strcmp0 (name, "to") == 0) &&
        ptype == PN_TYPE_CURRENCY)
        return build_currency_editor (target, pspec);

    return NULL;
}

static void
pn_rate_class_init (PnRateClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnHttpClass        *http_class    = PN_HTTP_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_rate_get_property;
    object_class->set_property = pn_rate_set_property;
    object_class->finalize     = pn_rate_finalize;
    node_class->receive        = pn_rate_receive;
    trigger_class->trigger     = pn_rate_trigger;

    node_class->build_property_editor = pn_rate_build_property_editor;

    /* Visual identity. */
    node_class->palette_icon = PN_RATE_NORMAL_ICON;
    node_class->class_name   = "Rate";
    node_class->icon         = PN_RATE_NORMAL_ICON;
    node_class->color        = (GdkRGBA){ 0.85, 0.65, 0.20, 1.0 };
    node_class->category     = "Filters";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_RATE_NORMAL_ICON;
    http_class->normal_color = (GdkRGBA){ 0.85, 0.65, 0.20, 1.0 };

    http_class->is_configured = pn_rate_is_configured;
    http_class->build_command = pn_rate_build_command;
    http_class->emit_message  = pn_rate_emit_message;

    props[PROP_FROM] = g_param_spec_enum (
            "from", "From",
            "Source currency.  The incoming `data.value` is treated "
            "as a quantity in this currency before the rate is "
            "applied.",
            PN_TYPE_CURRENCY, PN_CURRENCY_ETH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TO] = g_param_spec_enum (
            "to", "To",
            "Destination currency.  The outgoing `data.value` is the "
            "input expressed in this currency.",
            PN_TYPE_CURRENCY, PN_CURRENCY_USD,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* `rate` and `last-update` are READABLE+WRITABLE so the worksheet
     * (de)serialisation in pn-flow.c picks them up automatically and
     * the cached value survives save/load — that is the whole point
     * of caching it, since hammering CoinGecko on every workspace
     * reopen is what gets the IP banned. */
    props[PROP_RATE] = g_param_spec_double (
            "rate", "Rate",
            "Cached conversion factor (1 unit of `from` equals this "
            "many units of `to`).  Updated periodically by the auto-"
            "trigger and persisted in the worksheet so re-opening "
            "the file does not fire a fresh request.",
            0.0, G_MAXDOUBLE, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LAST_UPDATE] = g_param_spec_string (
            "last-update", "Last update",
            "ISO-8601 timestamp of the last successful rate fetch.  "
            "Empty until the first fetch completes; persisted with "
            "the worksheet alongside `rate`.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_rate_init (PnRate *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->from        = PN_CURRENCY_ETH;
    self->to          = PN_CURRENCY_USD;
    self->rate        = 1.0;
    self->last_update = NULL;

    pn_node_set_class_name (node, "Rate");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Default endpoint and a polite refresh cadence.  The notify
     * hook clamps any subsequent attempt to dial the period below
     * the 10-minute floor — set the default first, then attach the
     * hook so the legitimate default-set does not trip the guard. */
    g_object_set (self, "url", PN_RATE_DEFAULT_ENDPOINT, NULL);
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_RATE_PERIOD_DEFAULT);
    g_signal_connect (self, "notify::period",
                      G_CALLBACK (on_period_notify), NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRate *
pn_rate_new (void)
{
    return g_object_new (PN_TYPE_RATE, NULL);
}

const gchar *
pn_currency_get_icon_name (PnCurrency cur)
{
    return currency_info (cur)->icon;
}
