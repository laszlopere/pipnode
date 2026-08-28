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
/*  PnBridgeQuote — logic tier (headless core).                        */
/*                                                                     */
/*  The GTK-free half of the Bridge Quote node: the GType, the two     */
/*  provider back-ends, all properties, the cache-aware periodic fetch */
/*  and the message it emits on every tick.  The settings-dialog       */
/*  customisation (icon+ticker currency combos, read-only cache rows)  */
/*  lives in the companion gui-tier file pn-bridge-quote-gui.c, which  */
/*  installs that vfunc slot onto this class at editor startup (see    */
/*  pn_bridge_quote_gui_install).  The headless runtime registers and  */
/*  runs this node without ever pulling GTK.                           */
/*                                                                     */
/*  Both back-ends ride the inherited #PnHttp transport: one blocking  */
/*  libsoup GET per tick on the auto-trigger's worker thread.  Only    */
/*  ChangeNOW needs a second request (its min/max range lives on a     */
/*  separate endpoint), which is issued from emit_message on that same */
/*  worker thread through a throwaway session, the way                 */
/*  pn_geocode_resolve_sync() does it.                                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-bridge-quote.h"
#include "pn-message.h"

#include <math.h>
#include <stdarg.h>

#include <json-glib/json-glib.h>

/* Visual identity.  fa-retweet (U+F079) — the two chasing arrows read as
 * "route this through something", which is what a bridge does.  Distinct
 * from the FX Converter's fa-exchange and from PnAutoRandom's fa-random. */
#define PN_BRIDGE_QUOTE_ICON "\xef\x81\xb9"

/* Body colour: teal, deliberately unlike the FX Converter's gold so the
 * two are never confused at a glance on a worksheet that carries both. */
#define PN_BRIDGE_QUOTE_COLOR { 0.20, 0.60, 0.70, 1.0 }

/* Poll cadence.  PulseLN's endpoint is undocumented and uncached
 * (cf-cache-status: DYNAMIC), so the floor is about being a polite guest
 * rather than about a published quota; ChangeNOW's own limit (30/s,
 * 1800/min) is far above anything we would do.  Bridge quotes move with
 * the underlying spot price, so a five-minute default loses nothing. */
#define PN_BRIDGE_QUOTE_PERIOD_MIN     60u
#define PN_BRIDGE_QUOTE_PERIOD_DEFAULT 300u

/* Default reference amount.  Bridge quotes are amount-sensitive (a flat
 * network fee dominates a small swap), so the node always quotes a
 * concrete size rather than a unit.  One million PLS sits comfortably
 * inside both providers' limits for the default pair. */
#define PN_BRIDGE_QUOTE_AMOUNT_DEFAULT 1000000.0

/* Status strings, kept as named constants so "no successful fetch for the
 * current settings yet" reads the same wherever it is set. */
#define PN_BRIDGE_QUOTE_STATUS_NEVER "Never updated"
#define PN_BRIDGE_QUOTE_STATUS_OK    "OK"

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
 *  Derived from #PnHttp:url rather than hard-coded so re-pointing the
 *  node at a proxy moves both requests together. */
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
/*  The node reuses #PnCurrency so its settings dialog gets the same    */
/*  icon+ticker picker as the FX Converter, but neither bridge lists    */
/*  all 23 entries.  A %NULL cell means "this provider does not trade   */
/*  this asset", which is what marks a pair unsupported — better a      */
/*  legible "PulseLN does not list XMR" than a puzzling empty reply.    */
/*                                                                     */
/*  PulseLN symbols are uppercase and come from the `onlineList` its    */
/*  swap page ships; note MATIC maps to POL, Polygon's post-rebrand     */
/*  ticker.  ChangeNOW symbols are its lowercase *legacy* tickers,      */
/*  confirmed against /v1/currencies?active=true: mostly the obvious    */
/*  identity, but BNB is chain-qualified and DOT and USDT only exist    */
/*  under a chain suffix.  USD is a fiat pivot on the FX Converter, not */
/*  a bridgeable asset, so it is %NULL on both.                         */
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

/** The ticker nick of @cur as the FX Converter spells it ("PLS", "BNB").
 *  Used for the message's `from`/`to` fields and for human-readable text,
 *  where the provider's own spelling ("bnbbsc") would only confuse.
 *
 *  The enum class ref is taken once and never released: the returned
 *  string is owned by the class, so dropping the last ref would free the
 *  nicks out from under every caller.  One held ref on a process-wide
 *  singleton class, the same bargain pn_node_factory_register makes. */
static const gchar *
currency_nick (PnCurrency cur)
{
    static GEnumClass *eclass = NULL;
    static gsize       once   = 0;
    GEnumValue        *value;

    if (g_once_init_enter (&once))
    {
        eclass = g_type_class_ref (PN_TYPE_CURRENCY);
        g_once_init_leave (&once, 1);
    }

    value = g_enum_get_value (eclass, (gint) cur);
    return (value != NULL && value->value_nick != NULL)
            ? value->value_nick : "?";
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnBridgeQuote
{
    PnHttp parent_instance;

    /* @bridge / @from / @to / @amount are read on the worker thread
     * (build_request, emit_message) and written by the main-thread
     * property setter; @quote and everything downstream of a fetch are
     * written by the worker and read by the main thread on save.  All of
     * it rides one mutex. */
    GMutex      mutex;
    PnBridge    bridge;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     amount;

    gdouble     quote;         /* output in `to` units for `amount` */
    gdouble     rate;          /* quote / amount, fees included */
    gdouble     fee;           /* provider fee fraction; 0 = not published */
    gdouble     min_amount;    /* provider limits in `from` units; */
    gdouble     max_amount;    /* 0 = unknown or uncapped */
    gchar      *last_update;   /* ISO-8601; NULL until the first success */
    gchar      *status;        /* human-readable outcome of the last fetch */

    /* Advisory extras from the last reply.  Not persisted: they describe
     * one fetch, not the cached quote, and a stale forecast next to a
     * reloaded worksheet would be worse than none. */
    gchar      *speed_forecast;
    gchar      *warning;

    /* Whether the provider would currently *execute* this route, as
     * opposed to merely price it.  Defaults to TRUE and is only cleared
     * by a fetch that found the route closed, so a freshly-loaded
     * worksheet does not claim knowledge it has not fetched yet. */
    gboolean    swappable;

    /* Re-entrancy guard for the period clamp: re-setting the property
     * from its own notify handler fires another notify. */
    gboolean    period_clamping;

    /* Coalesced "the settings moved" reaction — see
     * bridge_quote_schedule_settle().  @settle_source is the pending
     * idle (0 when none); @settle_kick records that at least one of the
     * changes was a real change and so may warrant a refresh. */
    guint       settle_source;
    gboolean    settle_kick;
};

G_DEFINE_TYPE (PnBridgeQuote, pn_bridge_quote, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_BRIDGE,
    PROP_FROM,
    PROP_TO,
    PROP_AMOUNT,
    PROP_QUOTE,
    PROP_RATE,
    PROP_FEE,
    PROP_MIN_AMOUNT,
    PROP_MAX_AMOUNT,
    PROP_LAST_UPDATE,
    PROP_STATUS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void bridge_quote_emit_from_cache (PnBridgeQuote *self,
                                          gboolean       success);

/* ------------------------------------------------------------------ */
/*  State accessors (thread-safe)                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnBridge    bridge;
    PnCurrency  from;
    PnCurrency  to;
    gdouble     amount;
} Settings;

/** Snapshot the four settings that describe "what are we quoting", so
 *  the worker reads a coherent set rather than four separately-locked
 *  fields that a dialog could change in between. */
static void
bridge_quote_get_settings (
        PnBridgeQuote *self,
        Settings      *out)
{
    g_mutex_lock (&self->mutex);
    out->bridge = self->bridge;
    out->from   = self->from;
    out->to     = self->to;
    out->amount = self->amount;
    g_mutex_unlock (&self->mutex);
}

/** The cached quote is "deprecated" whenever there is no recorded
 *  successful fetch for the current settings — @last_update is empty on a
 *  fresh node, the instant the user changes the bridge, pair or amount
 *  (the setters clear it), and after a fetch that failed.  Consumers see
 *  it on the message as `deprecated`; the canvas paints the error mark. */
static gboolean
bridge_quote_is_deprecated (PnBridgeQuote *self)
{
    gboolean dep;

    g_mutex_lock (&self->mutex);
    dep = (self->last_update == NULL || *self->last_update == '\0');
    g_mutex_unlock (&self->mutex);

    return dep;
}

/** Recompute the canvas appearance.  MAIN THREAD ONLY.  The node shows
 *  the error marker while it is unconfigured (an unsupported pair, say)
 *  or while the cached quote cannot be trusted. */
static void
bridge_quote_refresh_visual (PnBridgeQuote *self)
{
    PnHttp   *http       = PN_HTTP (self);
    gboolean  configured = PN_HTTP_GET_CLASS (self)->is_configured (http);

    pn_http_apply_visual_state (http, configured);
    if (configured && bridge_quote_is_deprecated (self))
        pn_node_set_has_error (PN_NODE (self), TRUE);
}

static gboolean
bridge_quote_refresh_visual_main (gpointer data)
{
    bridge_quote_refresh_visual (PN_BRIDGE_QUOTE (data));
    return G_SOURCE_REMOVE;
}

/** Bounce bridge_quote_refresh_visual() onto the main thread, for callers
 *  on the fetch worker.  A no-op when no main loop is running (the
 *  headless test path), which is fine: those callers assert on state. */
static void
bridge_quote_refresh_visual_async (PnBridgeQuote *self)
{
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                bridge_quote_refresh_visual_main,
                                g_object_ref (self), g_object_unref);
}

/** Worker-thread success path: store the new quote, its derived rate and
 *  a timestamp under the mutex, then refresh the canvas on the main
 *  thread.  No g_object_notify from here — it would run on the worker and
 *  could trip GTK from a dialog binding watching these properties. */
static void
bridge_quote_record_success (
        PnBridgeQuote *self,
        gdouble        quote,
        gdouble        amount,
        const gchar   *note)
{
    GDateTime *now = g_date_time_new_now_local ();
    gchar     *iso = g_date_time_format_iso8601 (now);

    g_mutex_lock (&self->mutex);
    self->quote = quote;
    self->rate  = (amount > 0.0) ? quote / amount : 0.0;
    g_free (self->last_update);
    self->last_update = iso;                 /* takes ownership */
    g_free (self->status);
    /* A caveat that does not invalidate the quote (a closed payout pool,
     * say) rides alongside the OK rather than replacing it. */
    self->status = (note != NULL && *note != '\0')
            ? g_strdup_printf ("%s \xe2\x80\x94 %s",
                               PN_BRIDGE_QUOTE_STATUS_OK, note)
            : g_strdup (PN_BRIDGE_QUOTE_STATUS_OK);
    g_mutex_unlock (&self->mutex);

    g_date_time_unref (now);
    bridge_quote_refresh_visual_async (self);
}

/** Worker-thread failure path: record a human-readable status, surface
 *  the reason in the per-node log (the app runs from a desktop launcher
 *  with no terminal, so a g_warning would be invisible), leave the cached
 *  quote untouched so the node stays visibly deprecated — and still emit,
 *  with success = FALSE, so a downstream Debug sink sees the failure
 *  rather than silence.  @reason is the bare cause; the stored status
 *  prefixes it with "Update failed: ". */
static void G_GNUC_PRINTF (2, 3)
bridge_quote_record_failure (
        PnBridgeQuote *self,
        const gchar   *fmt,
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
    /* The quote we are holding is for settings we could not confirm, so
     * mark it untrusted; the value itself is kept for the display. */
    g_free (self->last_update);
    self->last_update = NULL;
    g_mutex_unlock (&self->mutex);

    pn_auto_trigger_log_on_main (PN_AUTO_TRIGGER (self),
                                 PN_LOG_LEVEL_ERROR, "%s", reason);
    bridge_quote_refresh_visual_async (self);
    bridge_quote_emit_from_cache (self, FALSE);
    g_free (reason);
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

/* ------------------------------------------------------------------ */
/*  Emission                                                           */
/* ------------------------------------------------------------------ */

/** Build and emit the tick message from whatever is currently cached.
 *  Called on the worker thread from three places: after a successful
 *  parse, from the failure path, and from the freshness gate when a
 *  request was skipped — so a reopened worksheet repaints its displays
 *  without touching the network.
 *
 *  Only fields the provider actually published are stamped: a zero `fee`
 *  or `max_amount` means "not published", and inventing a 0 for it would
 *  read downstream as a genuine zero fee or a zero cap. */
static void
bridge_quote_emit_from_cache (
        PnBridgeQuote *self,
        gboolean       success)
{
    PnNode      *node = PN_NODE (self);
    PnMessage   *msg  = pn_message_new (node, NULL);
    Settings     set;
    gdouble      quote, rate, fee, min_amount, max_amount;
    gboolean     deprecated;
    gboolean     swappable;
    gchar       *status   = NULL;
    gchar       *forecast = NULL;
    gchar       *warning  = NULL;
    const gchar *from_nick;
    const gchar *to_nick;
    gchar       *output;

    bridge_quote_get_settings (self, &set);

    g_mutex_lock (&self->mutex);
    quote      = self->quote;
    rate       = self->rate;
    fee        = self->fee;
    min_amount = self->min_amount;
    max_amount = self->max_amount;
    deprecated = (self->last_update == NULL || *self->last_update == '\0');
    swappable  = self->swappable;
    status     = g_strdup (self->status ? self->status : "");
    forecast   = g_strdup (self->speed_forecast ? self->speed_forecast : "");
    warning    = g_strdup (self->warning ? self->warning : "");
    g_mutex_unlock (&self->mutex);

    from_nick = currency_nick (set.from);
    to_nick   = currency_nick (set.to);

    if (success)
        output = g_strdup_printf ("%s: %.10g %s \xe2\x86\x92 %.8g %s "
                                  "(rate %.6g)",
                                  pn_bridge_get_display_name (set.bridge),
                                  set.amount, from_nick,
                                  quote, to_nick, rate);
    else
        output = g_strdup_printf ("%s %s\xe2\x86\x92%s: %s",
                                  pn_bridge_get_display_name (set.bridge),
                                  from_nick, to_nick, status);

    /* `value` is the canonical number of the message contract: the amount
     * this bridge would actually pay out. */
    pn_message_set_double  (msg, "value",      quote);
    pn_message_set_string  (msg, "output",     output);
    pn_message_set_boolean (msg, "success",    success);
    pn_message_set_double  (msg, "rate",       rate);
    pn_message_set_double  (msg, "amount",     set.amount);
    pn_message_set_string  (msg, "from",       from_nick);
    pn_message_set_string  (msg, "to",         to_nick);
    pn_message_set_string  (msg, "bridge",
                            pn_bridge_get_display_name (set.bridge));
    pn_message_set_boolean (msg, "deprecated", deprecated);
    /* A route can be perfectly well priced and still be closed for
     * business; downstream can tell the two apart. */
    pn_message_set_boolean (msg, "swappable",  swappable);

    if (fee > 0.0)
        pn_message_set_double (msg, "fee", fee);
    if (min_amount > 0.0)
        pn_message_set_double (msg, "min_amount", min_amount);
    if (max_amount > 0.0)
        pn_message_set_double (msg, "max_amount", max_amount);
    if (*forecast != '\0')
        pn_message_set_string (msg, "speed_forecast", forecast);
    if (*warning != '\0')
        pn_message_set_string (msg, "warning", warning);

    pn_auto_trigger_emit_on_main (PN_AUTO_TRIGGER (self), msg);

    g_free (output);
    g_free (status);
    g_free (forecast);
    g_free (warning);
}

/** Replace the advisory extras carried by the last reply.  Either may be
 *  %NULL or empty, which clears the field. */
static void
bridge_quote_set_extras (
        PnBridgeQuote *self,
        const gchar   *forecast,
        const gchar   *warning)
{
    g_mutex_lock (&self->mutex);
    g_free (self->speed_forecast);
    self->speed_forecast = g_strdup (forecast);
    g_free (self->warning);
    self->warning = g_strdup (warning);
    g_mutex_unlock (&self->mutex);
}

static void
bridge_quote_set_swappable (
        PnBridgeQuote *self,
        gboolean       swappable)
{
    g_mutex_lock (&self->mutex);
    self->swappable = swappable;
    g_mutex_unlock (&self->mutex);
}

static void
bridge_quote_set_limits (
        PnBridgeQuote *self,
        gdouble        min_amount,
        gdouble        max_amount)
{
    g_mutex_lock (&self->mutex);
    self->min_amount = isfinite (min_amount) && min_amount > 0.0
            ? min_amount : 0.0;
    self->max_amount = isfinite (max_amount) && max_amount > 0.0
            ? max_amount : 0.0;
    g_mutex_unlock (&self->mutex);
}

static void
bridge_quote_set_fee (
        PnBridgeQuote *self,
        gdouble        fee)
{
    g_mutex_lock (&self->mutex);
    self->fee = (isfinite (fee) && fee > 0.0) ? fee : 0.0;
    g_mutex_unlock (&self->mutex);
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
pulseln_parse (
        PnBridgeQuote *self,
        JsonObject    *root,
        const Settings *set)
{
    const gchar *from_sym = pn_bridge_get_ticker (set->bridge, set->from);
    const gchar *to_sym   = pn_bridge_get_ticker (set->bridge, set->to);
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
        bridge_quote_record_failure (
                self, "PulseLN did not quote %s or %s", from_sym, to_sym);
        return;
    }

    /* Whether the route is open for business.  STATUS covers the deposit
     * side, PS the payout pool; the swap page gates only its *button* on
     * these (`outPool`), and still prices the swap while showing "Swaps
     * To X Temporarily Offline".  A monitor should do the same: the rate
     * is well-defined and worth recording either way, and dropping it
     * would blank the history exactly when a pool is drained.  So the
     * quote is computed regardless and the closure is reported through
     * `swappable`, `warning` and the node's status instead. */
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
    denom = (set->from == PN_CURRENCY_BTC) ? 1e8 : 1.0;

    if (!isfinite (to_per_from) || !isfinite (from_per_to) ||
        from_per_to == 0.0)
    {
        bridge_quote_record_failure (
                self, "no usable %s/%s cross-rate in the PulseLN reply",
                from_sym, to_sym);
        g_free (per_from);
        g_free (per_to);
        g_free (closed);
        return;
    }

    rate = to_per_from;
    if (isfinite (from_diff) && isfinite (from_per_usd))
        rate -= from_diff * from_per_usd / (denom * from_per_to);

    out = set->amount * rate;

    /* Assets routed over the DLN carry an extra flat fee, quoted in USD
     * and converted into payout units. */
    if (json_array_contains (root, "DLN_LIST", to_sym))
    {
        gdouble dln_fee  = json_number (root, "DLN_FEE");
        gdouble to_per_usd = json_number (to_obj, "perUSD");

        if (isfinite (dln_fee) && isfinite (to_per_usd))
            out -= dln_fee * to_per_usd;
    }

    out -= out * fee;

    /* The site rounds to six decimals, and floors a BTC payout to whole
     * satoshi.  Mirror both so the number matches what the user sees. */
    out = round (out * 1e6) / 1e6;
    if (set->to == PN_CURRENCY_BTC)
        out = floor (out);
    if (!(out > 0.0))
        out = 0.0;

    bridge_quote_set_fee (self, fee);

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

        bridge_quote_set_limits (self, min_amount, max_amount);

        if (closed == NULL &&
            isfinite (min_amount) && min_amount > 0.0 &&
            set->amount < min_amount)
            closed = g_strdup_printf ("below PulseLN's minimum of %.10g %s",
                                      min_amount, from_sym);
        else if (closed == NULL &&
                 isfinite (max_amount) && max_amount > 0.0 &&
                 set->amount > max_amount)
            closed = g_strdup_printf ("above PulseLN's maximum of %.10g %s",
                                      max_amount, from_sym);
    }

    bridge_quote_set_extras (self, NULL, closed);
    bridge_quote_set_swappable (self, closed == NULL);

    g_free (per_from);
    g_free (per_to);

    /* Limits are advisory here: PulseLN still returns a rate outside
     * them, so report the quote and let the message carry the range. */
    if (out <= 0.0)
    {
        bridge_quote_record_failure (
                self, "PulseLN quoted nothing for %.10g %s — below the "
                      "network fee?", set->amount, from_sym);
        g_free (closed);
        return;
    }

    bridge_quote_record_success (self, out, set->amount, closed);
    bridge_quote_emit_from_cache (self, TRUE);
    g_free (closed);
}

/* ------------------------------------------------------------------ */
/*  ChangeNOW back-end                                                 */
/*                                                                     */
/*  v1 is the whole story: its quote and range endpoints need no API    */
/*  key, while the v2 equivalents answer 401 without one.  The quote    */
/*  arrives on the inherited transport; the range needs a second GET,   */
/*  issued here on the same worker thread, because it is what turns a   */
/*  bare "max_amount_exceeded" into a sentence naming the actual cap.   */
/* ------------------------------------------------------------------ */

/** Fetch min/max for the pair and cache them.  Best-effort: a failure
 *  here must not sink an otherwise good quote, so it is silent. */
static void
changenow_fetch_range (
        PnBridgeQuote  *self,
        const gchar    *base_url,
        const gchar    *pair,
        guint           timeout_seconds)
{
    SoupSession *session;
    SoupMessage *msg;
    GBytes      *bytes;
    gchar       *parent;
    gchar       *url;
    JsonParser  *parser;
    JsonNode    *root;

    /* The range endpoint is a sibling of the quote endpoint, so derive it
     * from the configured URL rather than hard-coding the host — a node
     * re-pointed at a proxy keeps both halves together. */
    parent = g_path_get_dirname (base_url);
    url    = g_strdup_printf ("%s/%s/%s", parent,
                              PN_CHANGENOW_RANGE_PATH, pair);
    g_free (parent);

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

                bridge_quote_set_limits (self,
                                         json_number (obj, "minAmount"),
                                         json_number (obj, "maxAmount"));
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
 *  terse and machine-shaped; the range we just fetched lets the two
 *  limit errors name the number the user has to move to. */
static gchar *
changenow_error_text (
        PnBridgeQuote *self,
        const gchar   *code,
        const gchar   *message)
{
    gdouble min_amount, max_amount;

    g_mutex_lock (&self->mutex);
    min_amount = self->min_amount;
    max_amount = self->max_amount;
    g_mutex_unlock (&self->mutex);

    if (g_strcmp0 (code, "max_amount_exceeded") == 0 && max_amount > 0.0)
        return g_strdup_printf ("ChangeNOW caps this pair at %.10g; "
                                "lower the amount", max_amount);
    if (g_strcmp0 (code, "deposit_too_small") == 0 && min_amount > 0.0)
        return g_strdup_printf ("ChangeNOW needs at least %.10g; "
                                "raise the amount", min_amount);
    if (g_strcmp0 (code, "pair_is_inactive") == 0)
        return g_strdup ("ChangeNOW is not trading this pair right now");

    if (message != NULL && *message != '\0')
        return g_strdup_printf ("ChangeNOW: %s", message);

    return g_strdup_printf ("ChangeNOW: %s", code ? code : "unknown error");
}

static void
changenow_parse (
        PnBridgeQuote  *self,
        JsonObject     *root,
        const Settings *set)
{
    const gchar *code = json_string (root, "error");
    gdouble      estimated;

    if (code != NULL)
    {
        gchar *text = changenow_error_text (self, code,
                                            json_string (root, "message"));

        bridge_quote_record_failure (self, "%s", text);
        g_free (text);
        return;
    }

    estimated = json_number (root, "estimatedAmount");
    if (!isfinite (estimated) || estimated <= 0.0)
    {
        bridge_quote_record_failure (
                self, "no usable estimate in the ChangeNOW reply");
        return;
    }

    bridge_quote_set_extras (self,
                             json_string (root, "transactionSpeedForecast"),
                             json_string (root, "warningMessage"));
    /* v1 does not publish the provider's cut separately; leaving the fee
     * at zero keeps it off the message rather than claiming it is free. */
    bridge_quote_set_fee (self, 0.0);
    /* ChangeNOW simply refuses to quote a pair it is not trading
     * (`pair_is_inactive`, handled above), so a number here means the
     * route is open. */
    bridge_quote_set_swappable (self, TRUE);

    bridge_quote_record_success (self, estimated, set->amount, NULL);
    bridge_quote_emit_from_cache (self, TRUE);
}

/* ------------------------------------------------------------------ */
/*  PnHttpClass overrides                                              */
/* ------------------------------------------------------------------ */

/** Configured iff the URL is set, the pair is not degenerate, the amount
 *  is positive and the selected provider actually lists both assets. */
static gboolean
pn_bridge_quote_is_configured (PnHttp *http)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (http);
    gchar         *url  = pn_http_dup_url (http);
    Settings       set;
    gboolean       ok;

    bridge_quote_get_settings (self, &set);

    ok = (url != NULL && *url != '\0') &&
         (set.from != set.to) &&
         (set.amount > 0.0) &&
         (pn_bridge_get_ticker (set.bridge, set.from) != NULL) &&
         (pn_bridge_get_ticker (set.bridge, set.to)   != NULL);

    g_free (url);
    return ok;
}

static SoupMessage *
pn_bridge_quote_build_request (PnHttp *http)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (http);
    gchar         *url  = pn_http_dup_url (http);
    Settings       set;
    gchar         *full_url;
    SoupMessage   *msg;

    bridge_quote_get_settings (self, &set);

    if (set.bridge == PN_BRIDGE_CHANGENOW)
    {
        const gchar *from_sym = pn_bridge_get_ticker (set.bridge, set.from);
        const gchar *to_sym   = pn_bridge_get_ticker (set.bridge, set.to);
        gchar        amount[G_ASCII_DTOSTR_BUF_SIZE];

        /* The amount rides in the path, so it must be formatted C-locale
         * — a comma decimal separator would silently corrupt the query. */
        g_ascii_formatd (amount, sizeof amount, "%.8g", set.amount);
        full_url = g_strdup_printf ("%s/%s/%s_%s",
                                    url ? url : "", amount, from_sym, to_sym);
    }
    else
    {
        /* PulseLN takes no parameters: one blob carries every pair. */
        full_url = g_strdup (url ? url : "");
    }

    msg = soup_message_new (SOUP_METHOD_GET, full_url);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_free (full_url);
    g_free (url);
    return msg;
}

static void
pn_bridge_quote_emit_message (
        PnHttp      *http,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (http);
    JsonParser    *parser;
    JsonNode      *root;
    JsonObject    *obj;
    GError        *error = NULL;
    Settings       set;

    bridge_quote_get_settings (self, &set);

    /* Transport-level failure: no HTTP response at all.  pn_http_trigger
     * has logged the raw error; record it as the node's status too. */
    if (!ok)
    {
        bridge_quote_record_failure (self, "Request failed: %s",
                                     error_text ? error_text : "unknown error");
        return;
    }

    if (body == NULL || *body == '\0')
    {
        bridge_quote_record_failure (self, "empty reply from %s",
                                     pn_bridge_get_display_name (set.bridge));
        return;
    }

    /* ChangeNOW answers a rejected amount with HTTP 400 and a JSON error
     * body, so the range lookup and the parse both have to run before the
     * status is judged — a bare "HTTP 400" would hide the actual reason.
     * Fetch the limits first so the error text can name them. */
    if (set.bridge == PN_BRIDGE_CHANGENOW)
    {
        const gchar *from_sym = pn_bridge_get_ticker (set.bridge, set.from);
        const gchar *to_sym   = pn_bridge_get_ticker (set.bridge, set.to);
        gchar       *url      = pn_http_dup_url (http);
        gchar       *pair     = g_strdup_printf ("%s_%s", from_sym, to_sym);
        guint        period   = pn_auto_trigger_get_period (
                                        PN_AUTO_TRIGGER (self));

        if (url != NULL && *url != '\0')
            changenow_fetch_range (self, url, pair,
                                   (period > 1u) ? period - 1u : 1u);

        g_free (pair);
        g_free (url);
    }
    else if (http_status < 200 || http_status >= 300)
    {
        /* PulseLN has no error body worth parsing: a non-2xx is the whole
         * message. */
        bridge_quote_record_failure (self, "PulseLN returned HTTP %d",
                                     http_status);
        return;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body, -1, &error))
    {
        bridge_quote_record_failure (self, "could not parse the %s reply: %s",
                                     pn_bridge_get_display_name (set.bridge),
                                     error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        bridge_quote_record_failure (self, "unexpected shape in the %s reply",
                                     pn_bridge_get_display_name (set.bridge));
        g_object_unref (parser);
        return;
    }
    obj = json_node_get_object (root);

    if (set.bridge == PN_BRIDGE_CHANGENOW)
        changenow_parse (self, obj, &set);
    else
        pulseln_parse (self, obj, &set);

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  Trigger override: cache-aware fetch gate                           */
/*                                                                     */
/*  The quote and its timestamp are persisted with the worksheet, so a  */
/*  reopened flow can repaint its Numeric and Graph sinks from cache    */
/*  instead of firing a request at the provider.  Unlike the FX         */
/*  Converter — which holds an internal rate and only speaks when a     */
/*  message arrives — this node is a source, so a skipped fetch must    */
/*  still emit: staying silent would leave downstream displays blank    */
/*  until the cache went stale.                                        */
/* ------------------------------------------------------------------ */

static gboolean
bridge_quote_cache_is_fresh (
        PnBridgeQuote *self,
        guint          period)
{
    gchar     *iso = NULL;
    GDateTime *then;
    GDateTime *now;
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
    /* Unparseable timestamp: treat as stale so a hand-edited save file
     * with a typo'd date recovers on the next tick. */
    if (then == NULL)
        return FALSE;

    now       = g_date_time_new_now_utc ();
    elapsed_s = g_date_time_difference (now, then) / G_TIME_SPAN_SECOND;
    fresh     = (elapsed_s >= 0) && ((guint64) elapsed_s < period);

    g_date_time_unref (now);
    g_date_time_unref (then);
    return fresh;
}

static void
pn_bridge_quote_trigger (PnAutoTrigger *trigger)
{
    PnBridgeQuote      *self   = PN_BRIDGE_QUOTE (trigger);
    PnAutoTriggerClass *parent = PN_AUTO_TRIGGER_CLASS (pn_bridge_quote_parent_class);
    guint               period = pn_auto_trigger_get_period (trigger);

    if (bridge_quote_cache_is_fresh (self, period))
    {
        bridge_quote_emit_from_cache (self, TRUE);
        return;
    }

    if (parent->trigger != NULL)
        parent->trigger (trigger);
}

/* ------------------------------------------------------------------ */
/*  Settling after a settings change                                   */
/*                                                                     */
/*  Reacting to bridge/from/to/amount the instant each one is set is   */
/*  wrong during a worksheet load.  pn-flow.c applies a node's saved    */
/*  properties one at a time, in the order they appear in the file, so  */
/*  when `amount` lands the cached `last-update` further down the bag   */
/*  has not been restored yet.  Repainting there marked a node with a   */
/*  perfectly good cached quote as deprecated — red body, ❗ overlay —   */
/*  and nothing repainted it afterwards, so it stayed red until the     */
/*  next real fetch minutes later.  Kicking the worker there was just   */
/*  as wrong: it raced the rest of the load and fired a request the     */
/*  cache existed to avoid.                                            */
/*                                                                     */
/*  So both are deferred to a single idle.  It runs once the whole      */
/*  property batch has been applied, when the node's state is coherent  */
/*  again, and it consults the cache rather than assuming: a restored   */
/*  quote that is still fresh paints clean and sends nothing, while a   */
/*  genuine user edit (which cleared `last-update`) repaints and kicks. */
/* ------------------------------------------------------------------ */

static gboolean
bridge_quote_settle (gpointer data)
{
    PnBridgeQuote *self = data;
    gboolean       kick;

    g_mutex_lock (&self->mutex);
    self->settle_source = 0;
    kick                = self->settle_kick;
    self->settle_kick   = FALSE;
    g_mutex_unlock (&self->mutex);

    bridge_quote_refresh_visual (self);

    /* Only chase a new quote when the cache cannot answer for the
     * settings we now hold.  After a load it can, so a reopened
     * worksheet still costs the provider nothing. */
    if (kick &&
        PN_HTTP_GET_CLASS (self)->is_configured (PN_HTTP (self)) &&
        !bridge_quote_cache_is_fresh (
                self, pn_auto_trigger_get_period (PN_AUTO_TRIGGER (self))))
        pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));

    return G_SOURCE_REMOVE;
}

/** Queue one settle pass, collapsing the four property sets a load (or a
 *  dialog's worth of edits) performs into a single repaint.  The idle
 *  holds a reference so it can never outlive the node it inspects. */
static void
bridge_quote_schedule_settle (
        PnBridgeQuote *self,
        gboolean       changed)
{
    g_mutex_lock (&self->mutex);

    if (changed)
        self->settle_kick = TRUE;

    if (self->settle_source == 0)
        self->settle_source = g_idle_add_full (G_PRIORITY_DEFAULT,
                                               bridge_quote_settle,
                                               g_object_ref (self),
                                               g_object_unref);

    g_mutex_unlock (&self->mutex);
}

/* ------------------------------------------------------------------ */
/*  Period floor enforcement                                           */
/* ------------------------------------------------------------------ */

/** Re-clamp #PnAutoTrigger:period to our floor on every change.  The base
 *  pspec carries the global 1 s minimum, so both a dialog spinner and a
 *  hand-edited worksheet can drop below it; this notify hook catches
 *  both, guarded against the recursion its own re-set would cause. */
static void
on_period_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (object);
    guint          period;

    (void) pspec;
    (void) user_data;

    if (self->period_clamping)
        return;

    g_object_get (object, "period", &period, NULL);
    if (period < PN_BRIDGE_QUOTE_PERIOD_MIN)
    {
        self->period_clamping = TRUE;
        g_object_set (object, "period", PN_BRIDGE_QUOTE_PERIOD_MIN, NULL);
        self->period_clamping = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

/** Drop the cached quote because the question it answered has changed.
 *  Clearing the timestamp both marks the quote deprecated at once and
 *  makes the freshness gate fall through to a real fetch — without it the
 *  kick below would be swallowed whenever the previous fetch was recent.
 *  Call with @self->mutex held. */
static void
bridge_quote_invalidate_locked (PnBridgeQuote *self)
{
    g_free (self->last_update);
    self->last_update = NULL;
    g_free (self->status);
    self->status = g_strdup (PN_BRIDGE_QUOTE_STATUS_NEVER);
    /* The limits and the fee belong to the old provider or pair too. */
    self->fee        = 0.0;
    self->min_amount = 0.0;
    self->max_amount = 0.0;
    g_free (self->speed_forecast);
    self->speed_forecast = NULL;
    g_free (self->warning);
    self->warning = NULL;
    self->swappable = TRUE;
}

static void
pn_bridge_quote_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (object);

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_BRIDGE:
        g_value_set_enum (value, self->bridge);
        break;
    case PROP_FROM:
        g_value_set_enum (value, self->from);
        break;
    case PROP_TO:
        g_value_set_enum (value, self->to);
        break;
    case PROP_AMOUNT:
        g_value_set_double (value, self->amount);
        break;
    case PROP_QUOTE:
        g_value_set_double (value, self->quote);
        break;
    case PROP_RATE:
        g_value_set_double (value, self->rate);
        break;
    case PROP_FEE:
        g_value_set_double (value, self->fee);
        break;
    case PROP_MIN_AMOUNT:
        g_value_set_double (value, self->min_amount);
        break;
    case PROP_MAX_AMOUNT:
        g_value_set_double (value, self->max_amount);
        break;
    case PROP_LAST_UPDATE:
        g_value_set_string (value,
                            self->last_update ? self->last_update : "");
        break;
    case PROP_STATUS:
        g_value_set_string (value, self->status ? self->status : "");
        break;
    default:
        g_mutex_unlock (&self->mutex);
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        return;
    }

    g_mutex_unlock (&self->mutex);
}

static void
pn_bridge_quote_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (object);
    PnHttp        *http = PN_HTTP (self);

    switch (prop_id)
    {
    case PROP_BRIDGE:
    case PROP_FROM:
    case PROP_TO:
    case PROP_AMOUNT:
    {
        gboolean changed      = FALSE;
        gboolean bridge_moved = FALSE;
        PnBridge new_bridge   = PN_BRIDGE_PULSELN;

        g_mutex_lock (&self->mutex);
        switch (prop_id)
        {
        case PROP_BRIDGE:
            new_bridge   = (PnBridge) g_value_get_enum (value);
            changed      = (self->bridge != new_bridge);
            bridge_moved = changed;
            self->bridge = new_bridge;
            break;
        case PROP_FROM:
        {
            PnCurrency v = (PnCurrency) g_value_get_enum (value);
            changed    = (self->from != v);
            self->from = v;
            break;
        }
        case PROP_TO:
        {
            PnCurrency v = (PnCurrency) g_value_get_enum (value);
            changed  = (self->to != v);
            self->to = v;
            break;
        }
        default:
        {
            gdouble v = g_value_get_double (value);
            changed      = (self->amount != v);
            self->amount = v;
            break;
        }
        }

        if (changed)
            bridge_quote_invalidate_locked (self);
        g_mutex_unlock (&self->mutex);

        /* Each provider has its own endpoint, so follow the selection.
         * Done outside the lock: the URL setter repaints, which reads
         * back through is_configured. */
        if (bridge_moved)
            g_object_set (self, "url",
                          pn_bridge_get_default_url (new_bridge), NULL);

        /* Repaint and, if the cache can no longer answer, chase a fresh
         * quote — but not until the rest of the property batch has
         * landed.  A user editing the dialog still sees the new quote
         * within seconds; the period floor exists to stop polling spam,
         * not deliberate user-driven refreshes. */
        (void) http;
        bridge_quote_schedule_settle (self, changed);
        break;
    }
    case PROP_QUOTE:
        g_mutex_lock (&self->mutex);
        self->quote = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_RATE:
        g_mutex_lock (&self->mutex);
        self->rate = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_FEE:
        g_mutex_lock (&self->mutex);
        self->fee = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_MIN_AMOUNT:
        g_mutex_lock (&self->mutex);
        self->min_amount = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_MAX_AMOUNT:
        g_mutex_lock (&self->mutex);
        self->max_amount = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
    case PROP_LAST_UPDATE:
        g_mutex_lock (&self->mutex);
        g_free (self->last_update);
        self->last_update = g_value_dup_string (value);
        g_mutex_unlock (&self->mutex);
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
pn_bridge_quote_dispose (GObject *object)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (object);
    guint          source;

    g_mutex_lock (&self->mutex);
    source = self->settle_source;
    self->settle_source = 0;
    g_mutex_unlock (&self->mutex);

    /* Removing the source runs its destroy notify, dropping the
     * reference the idle was holding. */
    if (source != 0)
        g_source_remove (source);

    G_OBJECT_CLASS (pn_bridge_quote_parent_class)->dispose (object);
}

static void
pn_bridge_quote_finalize (GObject *object)
{
    PnBridgeQuote *self = PN_BRIDGE_QUOTE (object);

    g_clear_pointer (&self->last_update, g_free);
    g_clear_pointer (&self->status, g_free);
    g_clear_pointer (&self->speed_forecast, g_free);
    g_clear_pointer (&self->warning, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_bridge_quote_parent_class)->finalize (object);
}

static void
pn_bridge_quote_class_init (PnBridgeQuoteClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnHttpClass        *http_class    = PN_HTTP_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);
    PnColor             color         = PN_BRIDGE_QUOTE_COLOR;

    object_class->get_property = pn_bridge_quote_get_property;
    object_class->set_property = pn_bridge_quote_set_property;
    object_class->dispose      = pn_bridge_quote_dispose;
    object_class->finalize     = pn_bridge_quote_finalize;
    trigger_class->trigger     = pn_bridge_quote_trigger;

    /* Visual identity. */
    node_class->palette_icon = PN_BRIDGE_QUOTE_ICON;
    node_class->class_name   = "Bridge Quote";
    node_class->icon         = PN_BRIDGE_QUOTE_ICON;
    node_class->color        = color;
    node_class->category     = "Network";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_BRIDGE_QUOTE_ICON;
    http_class->normal_color = color;

    http_class->is_configured = pn_bridge_quote_is_configured;
    http_class->build_request = pn_bridge_quote_build_request;
    http_class->emit_message  = pn_bridge_quote_emit_message;

    props[PROP_BRIDGE] = g_param_spec_enum (
            "bridge", "Bridge",
            "Swap provider to quote.  Changing it re-points the URL at "
            "that provider's endpoint and drops the cached quote.",
            PN_TYPE_BRIDGE, PN_BRIDGE_PULSELN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FROM] = g_param_spec_enum (
            "from", "From",
            "Asset being sold.  `amount` is a quantity in this currency.",
            PN_TYPE_CURRENCY, PN_CURRENCY_PLS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TO] = g_param_spec_enum (
            "to", "To",
            "Asset being bought.  The quote is expressed in this currency.",
            PN_TYPE_CURRENCY, PN_CURRENCY_BNB,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "How much of `from` to quote.  Bridge pricing is "
            "amount-sensitive — a flat network fee dominates a small "
            "swap — so the quote is always for a concrete size.",
            0.0, G_MAXDOUBLE, PN_BRIDGE_QUOTE_AMOUNT_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Everything below is fetched state.  It is READABLE+WRITABLE so the
     * worksheet (de)serialisation in pn-flow.c picks it up automatically
     * and the cached quote survives save/load — which is what lets a
     * reopened flow repaint its displays without a fresh request. */
    props[PROP_QUOTE] = g_param_spec_double (
            "quote", "Quote",
            "Cached payout: how much of `to` the bridge would deliver for "
            "`amount` units of `from`, fees included.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RATE] = g_param_spec_double (
            "rate", "Rate",
            "Effective rate, `quote` divided by `amount` — what one unit "
            "of `from` really fetches after the provider's spread and "
            "fees.  Comparable across bridges.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FEE] = g_param_spec_double (
            "fee", "Fee",
            "Provider fee as a fraction (0.014 = 1.4%).  Zero means the "
            "provider does not publish it separately — ChangeNOW's v1 "
            "endpoint folds it into the estimate.",
            0.0, 1.0, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MIN_AMOUNT] = g_param_spec_double (
            "min-amount", "Minimum amount",
            "Smallest swap the provider accepts, in `from` units.  Zero "
            "means unknown.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_AMOUNT] = g_param_spec_double (
            "max-amount", "Maximum amount",
            "Largest swap the provider accepts, in `from` units.  Zero "
            "means unknown or uncapped.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LAST_UPDATE] = g_param_spec_string (
            "last-update", "Last update",
            "ISO-8601 timestamp of the last successful quote.  Empty "
            "until the first one lands; persisted with the worksheet "
            "alongside `quote`.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_STATUS] = g_param_spec_string (
            "status", "Status",
            "Outcome of the most recent quote request.",
            PN_BRIDGE_QUOTE_STATUS_NEVER,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_bridge_quote_init (PnBridgeQuote *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->bridge      = PN_BRIDGE_PULSELN;
    self->from        = PN_CURRENCY_PLS;
    self->to          = PN_CURRENCY_BNB;
    self->amount      = PN_BRIDGE_QUOTE_AMOUNT_DEFAULT;
    self->quote       = 0.0;
    self->rate        = 0.0;
    self->last_update = NULL;
    self->status      = g_strdup (PN_BRIDGE_QUOTE_STATUS_NEVER);
    self->swappable   = TRUE;

    pn_node_set_class_name (node, "Bridge Quote");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Default endpoint and a polite cadence.  Set the period before
     * attaching the clamp hook so the legitimate default-set does not
     * trip the re-entrancy guard. */
    g_object_set (self, "url",
                  pn_bridge_get_default_url (PN_BRIDGE_PULSELN), NULL);
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_BRIDGE_QUOTE_PERIOD_DEFAULT);
    g_signal_connect (self, "notify::period",
                      G_CALLBACK (on_period_notify), NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnBridgeQuote *
pn_bridge_quote_new (void)
{
    return g_object_new (PN_TYPE_BRIDGE_QUOTE, NULL);
}
