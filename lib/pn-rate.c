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
/*  PnRate — logic tier (headless core).                               */
/*                                                                     */
/*  This file holds the GTK-free half of the Rate node: the GType, the */
/*  currency table, all properties, the cache-aware periodic fetch     */
/*  (trigger gate + request build + JSON parse) and the receive()-time */
/*  conversion.  The settings-dialog customisation (read-only cache    */
/*  rows + the icon+ticker currency combos) lives in the companion     */
/*  gui-tier file pn-rate-gui.c, which installs that vfunc slot onto    */
/*  this class at editor startup (see pn_rate_gui_install).  The        */
/*  headless runtime registers and runs this node without ever pulling */
/*  GTK.                                                                */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rate.h"
#include "pn-message.h"

#include <math.h>
#include <stdarg.h>

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

/* GeckoTerminal's free `simple/networks` endpoint — the node appends
 * `/<network>/token_price/<addr>[,<addr>]` and gets one USD price per
 * token contract.  One request covers one network, so a pair whose two
 * tokens live on different chains costs two requests per refresh. */
#define PN_RATE_GECKOTERMINAL_ENDPOINT \
    "https://api.geckoterminal.com/api/v2/simple/networks"

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
    const gchar *gt_network; /* GeckoTerminal network id; NULL when the
                              * currency has no liquid on-chain token
                              * (and for the USD pivot) */
    const gchar *gt_address; /* token contract on @gt_network: the
                              * wrapped native coin where the currency
                              * has its own chain, else the most liquid
                              * bridged/pegged copy */
} CurrencyInfo;

static const CurrencyInfo currency_table[] = {
    { PN_CURRENCY_ADA,   "ADA",   "cardano",          "ada",   "bsc",
      "0x3ee2200efb3400fabb9aacf31297cbdd1d435d47" },
    { PN_CURRENCY_ATOM,  "ATOM",  "cosmos",           "atom",  "bsc",
      "0x0eb3a705fc54725037cc9e008bdede697f62f335" },
    { PN_CURRENCY_AVAX,  "AVAX",  "avalanche-2",      "avax",  "avax",
      "0xb31f66aa3c1e785363f0875a1b74e27b85fd66c7" },
    { PN_CURRENCY_BCH,   "BCH",   "bitcoin-cash",     "bch",   "bsc",
      "0x8ff795a6f4d97e7887c79bea79aba5cc76444adf" },
    { PN_CURRENCY_BNB,   "BNB",   "binancecoin",      "bnb",   "bsc",
      "0xbb4cdb9cbd36b01bd1cbaebf2de08d9173bc095c" },
    { PN_CURRENCY_BTC,   "BTC",   "bitcoin",          "btc",   "eth",
      "0x2260fac5e5542a773aa44fbcfedf7c193bc2c599" },
    { PN_CURRENCY_CRO,   "CRO",   "crypto-com-chain", "cro",   "cro",
      "0x5c7f8a570d578ed84e63fdfa7b1ee72deae1ae23" },
    { PN_CURRENCY_DOGE,  "DOGE",  "dogecoin",         "doge",  "bsc",
      "0xba2ae424d960c26247dd6c32edc70b295c744c43" },
    { PN_CURRENCY_DOT,   "DOT",   "polkadot",         "dot",   "bsc",
      "0x7083609fce4d1d8dc0c979aab8c869ea2c873402" },
    { PN_CURRENCY_ETH,   "ETH",   "ethereum",         "eth",   "eth",
      "0xc02aaa39b223fe8d0a0e5c4f27ead9083c756cc2" },
    { PN_CURRENCY_LINK,  "LINK",  "chainlink",        "link",  "eth",
      "0x514910771af9ca656af840dff83e8264ecf986ca" },
    { PN_CURRENCY_LTC,   "LTC",   "litecoin",         "ltc",   "bsc",
      "0x4338665cbb7b2485a8855a139b75d5e34ab0db94" },
    { PN_CURRENCY_MATIC, "MATIC", "matic-network",    "matic", "polygon_pos",
      "0x0d500b1d8e8ef31e21c99d1db9a6444d3adf1270" },
    { PN_CURRENCY_PLS,   "PLS",   "pulsechain",       "pls",   "pulsechain",
      "0xa1077a294dde1b09bb078844df40758a5d0f9a27" },
    { PN_CURRENCY_SOL,   "SOL",   "solana",           "sol",   "solana",
      "So11111111111111111111111111111111111111112" },
    { PN_CURRENCY_TRX,   "TRX",   "tron",             "trx",   "tron",
      "TNUC9Qb1rRpS5CbWLmNMxXBjyFoydXjWFR" },
    { PN_CURRENCY_UNI,   "UNI",   "uniswap",          "uni",   "eth",
      "0x1f9840a85d5af5bf1d1762f925bdaddc4201f984" },
    { PN_CURRENCY_USD,   "USD",   NULL,               "usd",   NULL, NULL },
    { PN_CURRENCY_USDC,  "USDC",  "usd-coin",         "usdc",  "eth",
      "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48" },
    { PN_CURRENCY_USDT,  "USDT",  "tether",           "usdt",  "eth",
      "0xdac17f958d2ee523a2206206994597c13d831ec7" },
    { PN_CURRENCY_XLM,   "XLM",   "stellar",          "xlm",   NULL, NULL },
    { PN_CURRENCY_XMR,   "XMR",   "monero",           "xmr",   NULL, NULL },
    { PN_CURRENCY_XRP,   "XRP",   "ripple",           "xrp",   "bsc",
      "0x1d2f0da169ceb9fc7b3144628db156f3f6c60dbe" },
};

/** Look up the table entry for @cur.  Falls back to the USD row when
 *  asked about an out-of-range value so callers can dereference the
 *  return value unconditionally — USD's NULL `coingecko` field is the
 *  one row that build_request and friends already special-case (it is
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

GType
pn_rate_provider_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_RATE_PROVIDER_COINGECKO,
              "PN_RATE_PROVIDER_COINGECKO",     "CoinGecko"     },
            { PN_RATE_PROVIDER_GECKOTERMINAL,
              "PN_RATE_PROVIDER_GECKOTERMINAL", "GeckoTerminal" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnRateProvider", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/** The default #PnHttp:url for @provider. */
static const gchar *
provider_default_endpoint (PnRateProvider provider)
{
    return provider == PN_RATE_PROVIDER_GECKOTERMINAL
           ? PN_RATE_GECKOTERMINAL_ENDPOINT
           : PN_RATE_DEFAULT_ENDPOINT;
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnRate
{
    PnHttp parent_instance;

    /* @from / @to are read on the worker thread (build_request,
     * emit_message) and written by the main-thread property setter,
     * so accesses are guarded by @mutex.  @rate / @last_update are
     * read by the main thread on receive and written by the worker
     * after each successful fetch, so they ride the same mutex. */
    GMutex          mutex;
    PnRateProvider  provider;
    PnCurrency      from;
    PnCurrency      to;
    gdouble     rate;          /* multiplier from FROM units to TO units */
    gchar      *last_update;   /* ISO-8601 string; NULL until first save */
    gchar      *status;        /* human-readable outcome of the last fetch
                                * ("OK", "Update failed: …", "Never
                                * updated", …); read on the main thread,
                                * written by the worker after each fetch */

    /* Re-entrancy guard for the period clamp.  When a notify::period
     * fires below the floor, we re-set the property; that re-set
     * fires another notify, which we have to ignore to avoid an
     * infinite recursion. */
    gboolean    period_clamping;

    /* GeckoTerminal fetch state.  One GeckoTerminal request prices the
     * tokens of one network, so pn_rate_trigger drives one PnHttp
     * request per network ("leg") the pair touches and these fields
     * carry the pair snapshot and the partial prices across the legs.
     * Touched only on the fetch worker, so not under @mutex. */
    PnCurrency   gt_from;
    PnCurrency   gt_to;
    const gchar *gt_network;     /* network of the leg in flight */
    gdouble      gt_price_from;  /* USD price; NaN until a leg finds it */
    gdouble      gt_price_to;
    gboolean     gt_failed;      /* a leg recorded a failure: stop */
};

G_DEFINE_TYPE (PnRate, pn_rate, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_PROVIDER,
    PROP_FROM,
    PROP_TO,
    PROP_RATE,
    PROP_LAST_UPDATE,
    PROP_STATUS,
    N_PROPS,
};

/* Status strings.  Kept as named constants so the "no successful fetch
 * for the current pair yet" and "last fetch succeeded" states read the
 * same wherever they are set.  A failed fetch builds its own string via
 * rate_record_failure(). */
#define PN_RATE_STATUS_NEVER "Never updated"
#define PN_RATE_STATUS_OK    "OK"

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  State accessors (thread-safe)                                      */
/* ------------------------------------------------------------------ */

static PnRateProvider
rate_get_provider_locked (PnRate *self)
{
    PnRateProvider v;
    g_mutex_lock (&self->mutex);
    v = self->provider;
    g_mutex_unlock (&self->mutex);
    return v;
}

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

/** The cached rate is "deprecated" whenever there is no recorded
 *  successful fetch for the current (from, to) pair — i.e. @last_update
 *  is empty.  That is true on a fresh load before the first fetch, the
 *  instant the user picks a different pair (the setter clears the
 *  timestamp), and after a fetch that failed.  Downstream consumers see
 *  this on the emitted message as `deprecated`, and the canvas paints
 *  the node with the standard error marker. */
static gboolean
rate_is_deprecated_locked (PnRate *self)
{
    gboolean dep;
    g_mutex_lock (&self->mutex);
    dep = (self->last_update == NULL || *self->last_update == '\0');
    g_mutex_unlock (&self->mutex);
    return dep;
}

/** Recompute the canvas appearance.  MAIN THREAD ONLY — it ends up in
 *  g_object_notify, which the worksheet turns into a redraw.  The node
 *  shows the error marker when it is unconfigured (handled by the base)
 *  or when the cached rate is deprecated. */
static void
rate_refresh_visual (PnRate *self)
{
    PnHttp   *http       = PN_HTTP (self);
    gboolean  configured = PN_HTTP_GET_CLASS (self)->is_configured (http);

    pn_http_apply_visual_state (http, configured);
    if (configured && rate_is_deprecated_locked (self))
        pn_node_set_has_error (PN_NODE (self), TRUE);
}

static gboolean
rate_refresh_visual_main (gpointer data)
{
    rate_refresh_visual (PN_RATE (data));
    return G_SOURCE_REMOVE;
}

/** Bounce rate_refresh_visual() onto the main thread, for callers on
 *  the fetch worker.  A no-op in practice when no main loop is running
 *  (the headless one-shot test path), which is fine: those callers
 *  assert on state, not on the canvas. */
static void
rate_refresh_visual_async (PnRate *self)
{
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                rate_refresh_visual_main,
                                g_object_ref (self), g_object_unref);
}

/** Worker-thread success path: store the new rate, timestamp and an
 *  "OK" status under the mutex (no g_object_notify here — see the note
 *  in pn_rate_emit_message), then refresh the canvas on the main
 *  thread. */
static void
rate_record_success (
        PnRate *self,
        gdouble new_rate)
{
    GDateTime *now = g_date_time_new_now_local ();
    gchar     *iso = g_date_time_format_iso8601 (now);

    g_mutex_lock (&self->mutex);
    self->rate = new_rate;
    g_free (self->last_update);
    self->last_update = iso;                 /* takes ownership */
    g_free (self->status);
    self->status = g_strdup (PN_RATE_STATUS_OK);
    g_mutex_unlock (&self->mutex);

    g_date_time_unref (now);
    rate_refresh_visual_async (self);
}

/** Worker-thread failure path: record a human-readable status, surface
 *  the reason in the node log dialog (the node runs from a desktop
 *  launcher with no terminal, so a g_warning would be invisible), and
 *  leave @rate / @last_update untouched so the node stays visibly
 *  deprecated.  @reason is the bare cause; the stored status prefixes
 *  it with "Update failed: ". */
static void G_GNUC_PRINTF (2, 3)
rate_record_failure (
        PnRate      *self,
        const gchar *fmt,
        ...)
{
    va_list  ap;
    gchar   *reason;

    va_start (ap, fmt);
    reason = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    g_mutex_lock (&self->mutex);
    g_free (self->status);
    self->status = g_strdup_printf ("Update failed: %s", reason);
    g_mutex_unlock (&self->mutex);

    pn_auto_trigger_log_on_main (PN_AUTO_TRIGGER (self),
                                 PN_LOG_LEVEL_ERROR, "%s", reason);
    rate_refresh_visual_async (self);
    g_free (reason);
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
/*  PnHttp::trigger to actually fetch.  Pair changes (FROM / TO)      */
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

/** The USD price @cur starts a GeckoTerminal fetch with: the pivot is
 *  1.0 by definition, everything else is unknown until a leg finds it. */
static gdouble
gt_initial_price (const CurrencyInfo *info)
{
    return info->coingecko == NULL ? 1.0 : (gdouble) NAN;
}

/** One GeckoTerminal refresh.  The pair is snapshotted, then the base
 *  PnHttp tick runs once per distinct network among the two tokens
 *  (USD needs none); pn_rate_build_request / pn_rate_emit_message read
 *  @gt_network to know which leg they serve.  Success is recorded only
 *  after the last leg, once both prices are known. */
static void
rate_fetch_geckoterminal (
        PnRate             *self,
        PnAutoTriggerClass *parent)
{
    const CurrencyInfo *finfo;
    const CurrencyInfo *tinfo;
    const gchar        *legs[2] = { NULL, NULL };
    guint               i;

    self->gt_from = rate_get_from_locked (self);
    self->gt_to   = rate_get_to_locked   (self);
    finfo = currency_info (self->gt_from);
    tinfo = currency_info (self->gt_to);

    if (finfo->coingecko != NULL && finfo->gt_network == NULL)
    {
        rate_record_failure (self, "%s has no on-chain price on GeckoTerminal",
                             finfo->nick);
        return;
    }
    if (tinfo->coingecko != NULL && tinfo->gt_network == NULL)
    {
        rate_record_failure (self, "%s has no on-chain price on GeckoTerminal",
                             tinfo->nick);
        return;
    }

    self->gt_price_from = gt_initial_price (finfo);
    self->gt_price_to   = gt_initial_price (tinfo);
    self->gt_failed     = FALSE;

    legs[0] = finfo->gt_network != NULL ? finfo->gt_network
                                        : tinfo->gt_network;
    if (tinfo->gt_network != NULL &&
        g_strcmp0 (tinfo->gt_network, legs[0]) != 0)
        legs[1] = tinfo->gt_network;

    for (i = 0; i < G_N_ELEMENTS (legs) && legs[i] != NULL; i++)
    {
        self->gt_network = legs[i];
        parent->trigger (PN_AUTO_TRIGGER (self));
        if (self->gt_failed)
            break;
    }
    self->gt_network = NULL;

    if (self->gt_failed)
        return;

    if (!isfinite (self->gt_price_from) || !isfinite (self->gt_price_to) ||
        self->gt_price_to == 0.0)
    {
        rate_record_failure (self, "no usable %s/%s price in the reply",
                             finfo->nick, tinfo->nick);
        return;
    }

    rate_record_success (self, self->gt_price_from / self->gt_price_to);
}

static void
pn_rate_trigger (PnAutoTrigger *trigger)
{
    PnRate                   *self   = PN_RATE (trigger);
    PnAutoTriggerClass       *parent = PN_AUTO_TRIGGER_CLASS (pn_rate_parent_class);
    guint                     period = pn_auto_trigger_get_period (trigger);

    if (rate_cache_is_fresh (self, period))
        return;

    if (parent->trigger == NULL)
        return;

    /* Unconfigured: the base tick would skip silently, which on the
     * GeckoTerminal path would read as "no usable price". */
    if (!PN_HTTP_GET_CLASS (self)->is_configured (PN_HTTP (self)))
        return;

    if (rate_get_provider_locked (self) == PN_RATE_PROVIDER_GECKOTERMINAL)
        rate_fetch_geckoterminal (self, parent);
    else
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

/** Build the GeckoTerminal request for the leg in flight: every token
 *  of the snapshotted pair that lives on @gt_network, comma-separated,
 *  under `<url>/<network>/token_price/`. */
static SoupMessage *
rate_build_geckoterminal_request (PnRate *self)
{
    gchar              *url   = pn_http_dup_url (PN_HTTP (self));
    const CurrencyInfo *finfo = currency_info (self->gt_from);
    const CurrencyInfo *tinfo = currency_info (self->gt_to);
    GString            *full  = g_string_new (url ? url : "");
    gboolean            first = TRUE;
    SoupMessage        *msg;

    while (full->len > 0 && full->str[full->len - 1] == '/')
        g_string_truncate (full, full->len - 1);
    g_string_append_printf (full, "/%s/token_price/", self->gt_network);

    if (g_strcmp0 (finfo->gt_network, self->gt_network) == 0)
    {
        g_string_append (full, finfo->gt_address);
        first = FALSE;
    }
    if (g_strcmp0 (tinfo->gt_network, self->gt_network) == 0 &&
        tinfo->id != finfo->id)
    {
        if (!first)
            g_string_append_c (full, ',');
        g_string_append (full, tinfo->gt_address);
    }

    msg = soup_message_new (SOUP_METHOD_GET, full->str);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_string_free (full, TRUE);
    g_free (url);
    return msg;
}

/** Build the request.  CoinGecko's `simple/price` returns one USD
 *  price per requested coin id; we always pivot through USD so a
 *  single request is enough no matter which two of our currencies
 *  the user picked.  The helper appends each non-USD currency in
 *  {from, to} to the comma-separated `ids=` list. */
static SoupMessage *
pn_rate_build_request (PnHttp *http)
{
    PnRate              *self  = PN_RATE (http);
    gchar               *url   = pn_http_dup_url (http);
    PnCurrency           from  = rate_get_from_locked (self);
    PnCurrency           to    = rate_get_to_locked   (self);
    const CurrencyInfo  *finfo = currency_info (from);
    const CurrencyInfo  *tinfo = currency_info (to);
    GString             *ids   = g_string_new (NULL);
    gchar               *full_url;
    SoupMessage         *msg;

    /* GeckoTerminal legs are driven by rate_fetch_geckoterminal(). */
    if (self->gt_network != NULL)
    {
        g_string_free (ids, TRUE);
        g_free (url);
        return rate_build_geckoterminal_request (self);
    }

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

    msg = soup_message_new (SOUP_METHOD_GET, full_url);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_free (full_url);
    g_string_free (ids, TRUE);
    g_free (url);
    return msg;
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

/** Validate one HTTP reply and parse it into a JSON object.  Returns
 *  the parser (the caller unrefs it) with the root object in @root, or
 *  %NULL after recording the failure via rate_record_failure().
 *
 *  Every exit that is not a usable reply routes through
 *  rate_record_failure(), which both records a human-readable status
 *  (shown in the settings dialog) and logs the reason to the per-node
 *  log dialog.  The earlier version returned silently on a transport
 *  error, an HTTP error status, or a reply that did not contain the
 *  requested coins — which is exactly how a CoinGecko 403 (missing
 *  User-Agent) used to leave the node frozen on a stale rate with no
 *  visible explanation. */
static JsonParser *
rate_parse_reply (
        PnRate       *self,
        gboolean      ok,
        gint          http_status,
        const gchar  *body,
        const gchar  *error_text,
        JsonObject  **root)
{
    JsonParser *parser;
    JsonNode   *node;
    GError     *error = NULL;

    /* Transport-level failure: no HTTP response at all (DNS, connect,
     * timeout).  pn_http_trigger has already logged the raw error, but
     * record it as the node's status too. */
    if (!ok)
    {
        rate_record_failure (self, "Request failed: %s",
                             error_text ? error_text : "unknown error");
        return NULL;
    }

    /* HTTP-level failure: a non-2xx status.  The body on these is an
     * API error object, not a price table, so do not fall through to
     * the parse below — it would extract NaN and look like "no data". */
    if (http_status < 200 || http_status >= 300)
    {
        rate_record_failure (self, "exchange-rate API returned HTTP %d",
                             http_status);
        return NULL;
    }

    if (body == NULL || *body == '\0')
    {
        rate_record_failure (self, "empty reply from the exchange-rate API");
        return NULL;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body, -1, &error))
    {
        rate_record_failure (self,
                             "could not parse the exchange-rate reply: %s",
                             error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return NULL;
    }

    node = json_parser_get_root (parser);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    {
        rate_record_failure (self, "unexpected shape in the exchange-rate reply");
        g_object_unref (parser);
        return NULL;
    }

    *root = json_node_get_object (node);
    return parser;
}

/** Pull the USD price of @info's token out of GeckoTerminal's
 *  `token_prices` object (address → decimal string).  EVM addresses
 *  come back lower-cased, so 0x addresses match case-insensitively;
 *  base58 ones (Solana, Tron) are case-significant.  Returns %NaN when
 *  the token is missing or its price unparseable. */
static gdouble
extract_gt_price (
        JsonObject         *prices,
        const CurrencyInfo *info)
{
    gboolean  evm   = g_str_has_prefix (info->gt_address, "0x");
    GList    *names = json_object_get_members (prices);
    GList    *l;
    gdouble   price = (gdouble) NAN;

    for (l = names; l != NULL; l = l->next)
    {
        const gchar *name = l->data;
        JsonNode    *node;

        if (evm ? g_ascii_strcasecmp (name, info->gt_address) != 0
                : g_strcmp0 (name, info->gt_address) != 0)
            continue;

        node = json_object_get_member (prices, name);
        if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
            break;

        if (json_node_get_value_type (node) == G_TYPE_STRING)
        {
            const gchar *text = json_node_get_string (node);
            gchar       *end  = NULL;
            gdouble      v    = g_ascii_strtod (text, &end);

            if (end != text && *end == '\0')
                price = v;
        }
        else
        {
            price = json_node_get_double (node);
        }
        break;
    }

    g_list_free (names);
    return price;
}

/** One GeckoTerminal leg's reply: fill in whichever of the pair's
 *  prices live on this leg's network.  The rate itself is computed by
 *  rate_fetch_geckoterminal() once every leg is in. */
static void
rate_emit_geckoterminal (
        PnRate      *self,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    const CurrencyInfo *finfo = currency_info (self->gt_from);
    const CurrencyInfo *tinfo = currency_info (self->gt_to);
    JsonParser         *parser;
    JsonObject         *root   = NULL;
    JsonObject         *prices = NULL;
    JsonNode           *node;

    parser = rate_parse_reply (self, ok, http_status, body, error_text,
                               &root);
    if (parser == NULL)
    {
        self->gt_failed = TRUE;
        return;
    }

    /* { "data": { "attributes": { "token_prices": { addr: "1.23" } } } } */
    node = json_object_get_member (root, "data");
    if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
    {
        node = json_object_get_member (json_node_get_object (node),
                                       "attributes");
        if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
        {
            node = json_object_get_member (json_node_get_object (node),
                                           "token_prices");
            if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
                prices = json_node_get_object (node);
        }
    }

    if (prices == NULL)
    {
        rate_record_failure (self, "unexpected shape in the exchange-rate reply");
        self->gt_failed = TRUE;
        g_object_unref (parser);
        return;
    }

    if (g_strcmp0 (finfo->gt_network, self->gt_network) == 0)
        self->gt_price_from = extract_gt_price (prices, finfo);
    if (g_strcmp0 (tinfo->gt_network, self->gt_network) == 0)
        self->gt_price_to = extract_gt_price (prices, tinfo);

    g_object_unref (parser);
}

/** Parse the reply, recompute the rate, and stash it under the mutex.
 *  We deliberately do not emit a downstream message: the rate update is
 *  internal state, the conversion happens on the receive path, and
 *  emitting a "rate ticked" event each period would surprise downstream
 *  sinks that expect a 1:1 relationship between input and output
 *  messages on this node. */
static void
pn_rate_emit_message (
        PnHttp      *http,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    PnRate     *self = PN_RATE (http);
    JsonParser *parser;
    JsonObject *obj  = NULL;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     price_from;
    gdouble     price_to;
    gdouble     new_rate;

    if (self->gt_network != NULL)
    {
        rate_emit_geckoterminal (self, ok, http_status, body, error_text);
        return;
    }

    parser = rate_parse_reply (self, ok, http_status, body, error_text,
                               &obj);
    if (parser == NULL)
        return;

    from       = rate_get_from_locked (self);
    to         = rate_get_to_locked   (self);
    price_from = extract_usd_price (obj, from);
    price_to   = extract_usd_price (obj, to);

    if (!isfinite (price_from) || !isfinite (price_to) || price_to == 0.0)
    {
        rate_record_failure (self, "no usable %s/%s price in the reply",
                             currency_info (from)->nick,
                             currency_info (to)->nick);
        g_object_unref (parser);
        return;
    }

    new_rate = price_from / price_to;

    /* Success.  rate_record_success stores the new state under the mutex
     * and bounces a canvas refresh to the main thread.  Notifications
     * are intentionally NOT fired from here: they would run on the
     * worker thread and could trip GTK from inside any dialog binding
     * currently watching these properties.  The worksheet save path
     * reads the new state through the mutex-guarded getter, and the
     * dialog re-reads on next open. */
    rate_record_success (self, new_rate);

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
    gboolean             deprecated;

    g_mutex_lock (&self->mutex);
    rate       = self->rate;
    to         = self->to;
    deprecated = (self->last_update == NULL || *self->last_update == '\0');
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

    /* Flag the conversion as untrustworthy while the rate is deprecated
     * (no successful fetch yet for the current pair, or the last fetch
     * failed).  Downstream Debug / Format nodes can surface it; the
     * value is still passed through converted so chains do not stall. */
    pn_message_set_boolean (message, "deprecated", deprecated);

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
    case PROP_PROVIDER:
        g_value_set_enum (value, rate_get_provider_locked (self));
        break;
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
    case PROP_STATUS:
        g_mutex_lock (&self->mutex);
        g_value_set_string (value, self->status ? self->status : "");
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
    case PROP_PROVIDER:
    {
        PnRateProvider  new_value = (PnRateProvider) g_value_get_enum (value);
        PnRateProvider  old_value;
        gboolean        changed;

        g_mutex_lock (&self->mutex);
        old_value      = self->provider;
        changed        = (old_value != new_value);
        self->provider = new_value;
        if (changed)
        {
            /* A rate from the other provider is not this provider's
             * answer: drop the cache exactly as a pair change does. */
            g_free (self->last_update);
            self->last_update = NULL;
            g_free (self->status);
            self->status = g_strdup (PN_RATE_STATUS_NEVER);
        }
        g_mutex_unlock (&self->mutex);

        if (!changed)
            break;

        /* Follow the endpoint along unless the user pointed it somewhere
         * of their own (a proxy, a paid tier): only an empty URL or the
         * previous provider's stock endpoint is swapped. */
        {
            gchar *url = pn_http_dup_url (http);

            if (url == NULL || *url == '\0' ||
                g_strcmp0 (url, provider_default_endpoint (old_value)) == 0)
                g_object_set (self, "url",
                              provider_default_endpoint (new_value), NULL);
            g_free (url);
        }

        rate_refresh_visual (self);
        if (PN_HTTP_GET_CLASS (self)->is_configured (http))
            pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
        break;
    }
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
             * the previous fetch happened within @period seconds.
             * Clearing it also marks the rate deprecated at once (see
             * rate_is_deprecated_locked), so the node flags the stale
             * conversion the instant the user picks a new pair rather
             * than waiting for the refresh to land. */
            g_free (self->last_update);
            self->last_update = NULL;
            /* The new pair has never been fetched; reset the status to
             * match so a stale "OK" from the previous pair is not left
             * sitting next to a now-deprecated rate. */
            g_free (self->status);
            self->status = g_strdup (PN_RATE_STATUS_NEVER);
        }
        g_mutex_unlock (&self->mutex);

        /* On the main thread already (property setter), so refresh the
         * canvas synchronously; this also repaints the error marker for
         * the freshly-deprecated rate. */
        rate_refresh_visual (self);

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
        /* Restoring the timestamp un-deprecates the cached rate, so the
         * error marker has to come back off.  This matters on every
         * worksheet load: the properties bag applies `from` / `to`
         * first, and a saved pair that differs from the constructor
         * default trips the pair-change branch above, which clears the
         * timestamp and paints the node red.  The `last-update` member
         * that follows puts the cache back — without this refresh the
         * node stayed visibly in error until the next successful fetch,
         * which the freshness gate in pn_rate_trigger deliberately
         * suppresses while the restored cache is still young. */
        rate_refresh_visual (self);
        break;
    case PROP_STATUS:
        g_mutex_lock (&self->mutex);
        g_free (self->status);
        self->status = g_value_dup_string (value);
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
    g_clear_pointer (&self->status, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_rate_parent_class)->finalize (object);
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

    /* Visual identity. */
    node_class->palette_icon = PN_RATE_NORMAL_ICON;
    node_class->class_name   = "FX Converter";
    node_class->icon         = PN_RATE_NORMAL_ICON;
    node_class->color        = (PnColor){ 0.85, 0.65, 0.20, 1.0 };
    node_class->category     = "Filters/Compute & AI";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_RATE_NORMAL_ICON;
    http_class->normal_color = (PnColor){ 0.85, 0.65, 0.20, 1.0 };

    http_class->is_configured = pn_rate_is_configured;
    http_class->build_request = pn_rate_build_request;
    http_class->emit_message  = pn_rate_emit_message;

    /* Installed first so it serialises (and is replayed on load) ahead
     * of `rate` / `last-update`: a provider change drops the cache, so
     * it must land before the cached rate is restored. */
    props[PROP_PROVIDER] = g_param_spec_enum (
            "provider", "Provider",
            "Where USD prices come from.  CoinGecko reports an "
            "aggregated market price; GeckoTerminal reports the on-"
            "chain DEX price of each currency's wrapped or pegged token. "
            "Switching swaps the URL to the provider's stock endpoint "
            "unless it was customised.",
            PN_TYPE_RATE_PROVIDER, PN_RATE_PROVIDER_COINGECKO,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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

    /* Human-readable outcome of the most recent fetch, surfaced
     * read-only in the settings dialog ("OK", "Update failed: …",
     * "Never updated", "Awaiting update (pair changed)").  READWRITE so
     * the settings dialog enumerates it and it survives save/load; the
     * gui tier renders it as a read-only label, and the node writes it
     * directly through the mutex-guarded field rather than the setter
     * on the fetch worker. */
    props[PROP_STATUS] = g_param_spec_string (
            "status", "Status",
            "Outcome of the most recent rate fetch.",
            PN_RATE_STATUS_NEVER,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_rate_init (PnRate *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->provider    = PN_RATE_PROVIDER_COINGECKO;
    self->from        = PN_CURRENCY_ETH;
    self->to          = PN_CURRENCY_USD;
    self->rate        = 1.0;
    self->last_update = NULL;
    self->status      = g_strdup (PN_RATE_STATUS_NEVER);

    pn_node_set_class_name (node, "FX Converter");
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
