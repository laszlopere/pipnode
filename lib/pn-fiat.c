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
/*  PnFiat — logic tier (headless core).                              */
/*                                                                    */
/*  The GTK-free half of the Fiat Converter: the GType, the currency  */
/*  table, all properties, the cache-aware periodic fetch and the     */
/*  receive()-time conversion.  The settings-dialog customisation     */
/*  (read-only cache rows, and the code+name pickers for the pair)    */
/*  lives in the companion gui-tier file pn-fiat-gui.c, which installs */
/*  that vfunc slot onto this class at editor startup.                */
/*                                                                    */
/*  This node is the FX Converter (pn-rate.c) with a different quote  */
/*  source, and it deliberately keeps that node's message contract    */
/*  member for member — a chain built around one converts to the      */
/*  other by swapping the node.  What differs is the fetch: the ECB   */
/*  publishes its reference rates against the euro for every currency */
/*  in the set, and the endpoint re-bases them on request, so a       */
/*  single round trip answers any pair without a USD pivot.           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-fiat.h"
#include "pn-message.h"

#include <math.h>
#include <stdarg.h>

#include <json-glib/json-glib.h>

/* Visual identity.  fa-money (U+F0D6) — banknotes, the fiat answer to
 * the FX Converter's fa-exchange arrows. */
#define PN_FIAT_NORMAL_ICON "\xef\x83\x96"

/* The ECB fixes its reference rates once per working day, shortly
 * after 16:00 CET, so there is nothing a fast poll can learn that an
 * hourly one cannot — the default is #PnAutoTrigger's ceiling of an
 * hour, which still picks the daily fixing up within the hour on a
 * worksheet left open.  The floor is the ten minutes the FX Converter
 * also enforces: low enough to leave the spinner some room (the
 * ceiling is a hard 3600, so a floor at the same value would make the
 * setting a constant), high enough that a worksheet carrying several
 * converters cannot turn a free public endpoint into a target. */
#define PN_FIAT_PERIOD_MIN     600u
#define PN_FIAT_PERIOD_DEFAULT 3600u

/* Frankfurter — a free, key-less, open-source front end to the ECB's
 * published reference rates.  `base` picks what one unit is quoted in
 * and `symbols` narrows the reply to the currency we actually want.
 * Exposed through the inherited PnHttp:url so the user can re-point
 * at their own instance (the server is self-hostable). */
#define PN_FIAT_DEFAULT_ENDPOINT "https://api.frankfurter.dev/v1/latest"

/* ------------------------------------------------------------------ */
/*  Currency table                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnFiatCurrency  id;
    const gchar    *code;   /* ISO 4217 — enum nick, label and API symbol */
    const gchar    *name;   /* English name, shown beside the code */
} FiatInfo;

static const FiatInfo fiat_table[] = {
    { PN_FIAT_CURRENCY_AUD, "AUD", "Australian dollar"     },
    { PN_FIAT_CURRENCY_BRL, "BRL", "Brazilian real"        },
    { PN_FIAT_CURRENCY_CAD, "CAD", "Canadian dollar"       },
    { PN_FIAT_CURRENCY_CHF, "CHF", "Swiss franc"           },
    { PN_FIAT_CURRENCY_CNY, "CNY", "Chinese renminbi yuan" },
    { PN_FIAT_CURRENCY_CZK, "CZK", "Czech koruna"          },
    { PN_FIAT_CURRENCY_DKK, "DKK", "Danish krone"          },
    { PN_FIAT_CURRENCY_EUR, "EUR", "Euro"                  },
    { PN_FIAT_CURRENCY_GBP, "GBP", "British pound"         },
    { PN_FIAT_CURRENCY_HKD, "HKD", "Hong Kong dollar"      },
    { PN_FIAT_CURRENCY_HUF, "HUF", "Hungarian forint"      },
    { PN_FIAT_CURRENCY_IDR, "IDR", "Indonesian rupiah"     },
    { PN_FIAT_CURRENCY_ILS, "ILS", "Israeli new shekel"    },
    { PN_FIAT_CURRENCY_INR, "INR", "Indian rupee"          },
    { PN_FIAT_CURRENCY_ISK, "ISK", "Icelandic króna"       },
    { PN_FIAT_CURRENCY_JPY, "JPY", "Japanese yen"          },
    { PN_FIAT_CURRENCY_KRW, "KRW", "South Korean won"      },
    { PN_FIAT_CURRENCY_MXN, "MXN", "Mexican peso"          },
    { PN_FIAT_CURRENCY_MYR, "MYR", "Malaysian ringgit"     },
    { PN_FIAT_CURRENCY_NOK, "NOK", "Norwegian krone"       },
    { PN_FIAT_CURRENCY_NZD, "NZD", "New Zealand dollar"    },
    { PN_FIAT_CURRENCY_PHP, "PHP", "Philippine peso"       },
    { PN_FIAT_CURRENCY_PLN, "PLN", "Polish złoty"          },
    { PN_FIAT_CURRENCY_RON, "RON", "Romanian leu"          },
    { PN_FIAT_CURRENCY_SEK, "SEK", "Swedish krona"         },
    { PN_FIAT_CURRENCY_SGD, "SGD", "Singapore dollar"      },
    { PN_FIAT_CURRENCY_THB, "THB", "Thai baht"             },
    { PN_FIAT_CURRENCY_TRY, "TRY", "Turkish lira"          },
    { PN_FIAT_CURRENCY_USD, "USD", "United States dollar"  },
    { PN_FIAT_CURRENCY_ZAR, "ZAR", "South African rand"    },
};

/** Look up the table entry for @cur.  An out-of-range value answers
 *  the euro row rather than running off the end, so every caller can
 *  dereference the result unconditionally.  The euro is the safest
 *  miss value here: it is the currency the ECB quotes everything else
 *  against, so a request built from it is always a request the
 *  endpoint understands. */
static const FiatInfo *
fiat_info (PnFiatCurrency cur)
{
    guint i;
    const FiatInfo *fallback = &fiat_table[0];

    for (i = 0; i < G_N_ELEMENTS (fiat_table); i++)
    {
        if (fiat_table[i].id == cur)
            return &fiat_table[i];
        if (fiat_table[i].id == PN_FIAT_CURRENCY_EUR)
            fallback = &fiat_table[i];
    }

    return fallback;
}

GType
pn_fiat_currency_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_FIAT_CURRENCY_AUD, "PN_FIAT_CURRENCY_AUD", "AUD" },
            { PN_FIAT_CURRENCY_BRL, "PN_FIAT_CURRENCY_BRL", "BRL" },
            { PN_FIAT_CURRENCY_CAD, "PN_FIAT_CURRENCY_CAD", "CAD" },
            { PN_FIAT_CURRENCY_CHF, "PN_FIAT_CURRENCY_CHF", "CHF" },
            { PN_FIAT_CURRENCY_CNY, "PN_FIAT_CURRENCY_CNY", "CNY" },
            { PN_FIAT_CURRENCY_CZK, "PN_FIAT_CURRENCY_CZK", "CZK" },
            { PN_FIAT_CURRENCY_DKK, "PN_FIAT_CURRENCY_DKK", "DKK" },
            { PN_FIAT_CURRENCY_EUR, "PN_FIAT_CURRENCY_EUR", "EUR" },
            { PN_FIAT_CURRENCY_GBP, "PN_FIAT_CURRENCY_GBP", "GBP" },
            { PN_FIAT_CURRENCY_HKD, "PN_FIAT_CURRENCY_HKD", "HKD" },
            { PN_FIAT_CURRENCY_HUF, "PN_FIAT_CURRENCY_HUF", "HUF" },
            { PN_FIAT_CURRENCY_IDR, "PN_FIAT_CURRENCY_IDR", "IDR" },
            { PN_FIAT_CURRENCY_ILS, "PN_FIAT_CURRENCY_ILS", "ILS" },
            { PN_FIAT_CURRENCY_INR, "PN_FIAT_CURRENCY_INR", "INR" },
            { PN_FIAT_CURRENCY_ISK, "PN_FIAT_CURRENCY_ISK", "ISK" },
            { PN_FIAT_CURRENCY_JPY, "PN_FIAT_CURRENCY_JPY", "JPY" },
            { PN_FIAT_CURRENCY_KRW, "PN_FIAT_CURRENCY_KRW", "KRW" },
            { PN_FIAT_CURRENCY_MXN, "PN_FIAT_CURRENCY_MXN", "MXN" },
            { PN_FIAT_CURRENCY_MYR, "PN_FIAT_CURRENCY_MYR", "MYR" },
            { PN_FIAT_CURRENCY_NOK, "PN_FIAT_CURRENCY_NOK", "NOK" },
            { PN_FIAT_CURRENCY_NZD, "PN_FIAT_CURRENCY_NZD", "NZD" },
            { PN_FIAT_CURRENCY_PHP, "PN_FIAT_CURRENCY_PHP", "PHP" },
            { PN_FIAT_CURRENCY_PLN, "PN_FIAT_CURRENCY_PLN", "PLN" },
            { PN_FIAT_CURRENCY_RON, "PN_FIAT_CURRENCY_RON", "RON" },
            { PN_FIAT_CURRENCY_SEK, "PN_FIAT_CURRENCY_SEK", "SEK" },
            { PN_FIAT_CURRENCY_SGD, "PN_FIAT_CURRENCY_SGD", "SGD" },
            { PN_FIAT_CURRENCY_THB, "PN_FIAT_CURRENCY_THB", "THB" },
            { PN_FIAT_CURRENCY_TRY, "PN_FIAT_CURRENCY_TRY", "TRY" },
            { PN_FIAT_CURRENCY_USD, "PN_FIAT_CURRENCY_USD", "USD" },
            { PN_FIAT_CURRENCY_ZAR, "PN_FIAT_CURRENCY_ZAR", "ZAR" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnFiatCurrency", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnFiat
{
    PnHttp parent_instance;

    /* @from / @to are read on the worker thread (build_request,
     * emit_message) and written by the main-thread property setter;
     * @rate / @last_update / @status are written by the worker after
     * each fetch and read by the main thread on receive.  All of them
     * ride @mutex. */
    GMutex          mutex;
    PnFiatCurrency  from;
    PnFiatCurrency  to;
    gdouble         rate;         /* 1 FROM equals this many TO */
    gchar          *last_update;  /* ISO-8601; NULL until first fetch */
    gchar          *status;       /* human-readable fetch outcome */

    /* Re-entrancy guard for the period clamp: re-setting the property
     * from inside its own notify handler fires another notify, which
     * has to be ignored or the two recurse. */
    gboolean        period_clamping;
};

G_DEFINE_TYPE (PnFiat, pn_fiat, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_FROM,
    PROP_TO,
    PROP_RATE,
    PROP_LAST_UPDATE,
    PROP_STATUS,
    N_PROPS,
};

/* Status strings.  Named so "no successful fetch for this pair yet"
 * and "the last fetch worked" read the same wherever they are set; a
 * failure builds its own string in fiat_record_failure(), and a
 * success that carried a fixing date appends it. */
#define PN_FIAT_STATUS_NEVER "Never updated"
#define PN_FIAT_STATUS_OK    "OK"

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  State accessors (thread-safe)                                      */
/* ------------------------------------------------------------------ */

static PnFiatCurrency
fiat_get_from_locked (PnFiat *self)
{
    PnFiatCurrency v;
    g_mutex_lock (&self->mutex);
    v = self->from;
    g_mutex_unlock (&self->mutex);
    return v;
}

static PnFiatCurrency
fiat_get_to_locked (PnFiat *self)
{
    PnFiatCurrency v;
    g_mutex_lock (&self->mutex);
    v = self->to;
    g_mutex_unlock (&self->mutex);
    return v;
}

static gdouble
fiat_get_rate_locked (PnFiat *self)
{
    gdouble v;
    g_mutex_lock (&self->mutex);
    v = self->rate;
    g_mutex_unlock (&self->mutex);
    return v;
}

/** The cached rate is "deprecated" whenever there is no recorded
 *  successful fetch for the current (from, to) pair — i.e.
 *  @last_update is empty.  True on a fresh load before the first
 *  fetch, the instant the user picks a different pair (the setter
 *  clears the timestamp), and after a fetch that failed.  Downstream
 *  sees it on the emitted message as `deprecated`; the canvas paints
 *  the standard error marker. */
static gboolean
fiat_is_deprecated_locked (PnFiat *self)
{
    gboolean dep;
    g_mutex_lock (&self->mutex);
    dep = (self->last_update == NULL || *self->last_update == '\0');
    g_mutex_unlock (&self->mutex);
    return dep;
}

/** Recompute the canvas appearance.  MAIN THREAD ONLY — it ends up in
 *  g_object_notify, which the worksheet turns into a redraw. */
static void
fiat_refresh_visual (PnFiat *self)
{
    PnHttp   *http       = PN_HTTP (self);
    gboolean  configured = PN_HTTP_GET_CLASS (self)->is_configured (http);

    pn_http_apply_visual_state (http, configured);
    if (configured && fiat_is_deprecated_locked (self))
        pn_node_set_has_error (PN_NODE (self), TRUE);
}

static gboolean
fiat_refresh_visual_main (gpointer data)
{
    fiat_refresh_visual (PN_FIAT (data));
    return G_SOURCE_REMOVE;
}

/** Bounce fiat_refresh_visual() onto the main thread, for callers on
 *  the fetch worker.  A no-op in practice when no main loop is running
 *  (the headless test path), which is fine: those callers assert on
 *  state, not on the canvas. */
static void
fiat_refresh_visual_async (PnFiat *self)
{
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                fiat_refresh_visual_main,
                                g_object_ref (self), g_object_unref);
}

/** Worker-thread success path: store the new rate, timestamp and
 *  status under the mutex (no g_object_notify here — see the note in
 *  pn_fiat_emit_message), then refresh the canvas on the main thread.
 *  @fixing_date is the reference day the endpoint attributed the rate
 *  to, or %NULL when the reply carried none; it goes into the status
 *  because a rate fetched on Sunday is Friday's rate, and a user
 *  comparing the converted figure against a bank's wants to know. */
static void
fiat_record_success (
        PnFiat      *self,
        gdouble      new_rate,
        const gchar *fixing_date)
{
    GDateTime *now = g_date_time_new_now_local ();
    gchar     *iso = g_date_time_format_iso8601 (now);

    g_mutex_lock (&self->mutex);
    self->rate = new_rate;
    g_free (self->last_update);
    self->last_update = iso;                 /* takes ownership */
    g_free (self->status);
    self->status = (fixing_date != NULL && *fixing_date != '\0')
        ? g_strdup_printf ("%s (reference rate of %s)",
                           PN_FIAT_STATUS_OK, fixing_date)
        : g_strdup (PN_FIAT_STATUS_OK);
    g_mutex_unlock (&self->mutex);

    g_date_time_unref (now);
    fiat_refresh_visual_async (self);
}

/** Worker-thread failure path: record a human-readable status and
 *  surface the reason in the per-node log dialog (the editor runs from
 *  a desktop launcher with no terminal, so a g_warning would be
 *  invisible), leaving @rate / @last_update untouched so the node
 *  stays visibly deprecated.  @fmt spells the bare cause; the stored
 *  status prefixes it with "Update failed: ". */
static void G_GNUC_PRINTF (2, 3)
fiat_record_failure (
        PnFiat      *self,
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
    fiat_refresh_visual_async (self);
    g_free (reason);
}

/* ------------------------------------------------------------------ */
/*  Trigger override: cache-aware fetch gate                           */
/*                                                                     */
/*  The base #PnAutoTrigger fires its first tick ~1s after             */
/*  construction so long-period nodes do not stay blank after a        */
/*  worksheet load.  Without a gate every reopen would burn a request  */
/*  even when the persisted `last-update` is still well within         */
/*  @period — defeating the point of caching the rate in the           */
/*  worksheet.  Only a missing, unparseable or genuinely stale         */
/*  timestamp chains to PnHttp::trigger; pair changes clear the        */
/*  timestamp in the setter, so the kick that follows one does fall    */
/*  through to a fetch.                                                */
/* ------------------------------------------------------------------ */

static gboolean
fiat_cache_is_fresh (PnFiat *self,
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
pn_fiat_trigger (PnAutoTrigger *trigger)
{
    PnFiat             *self   = PN_FIAT (trigger);
    PnAutoTriggerClass *parent = PN_AUTO_TRIGGER_CLASS (pn_fiat_parent_class);
    guint               period = pn_auto_trigger_get_period (trigger);

    if (fiat_cache_is_fresh (self, period))
        return;

    if (parent->trigger != NULL)
        parent->trigger (trigger);
}

/* ------------------------------------------------------------------ */
/*  Period floor enforcement                                           */
/* ------------------------------------------------------------------ */

/** Re-clamp #PnAutoTrigger:period to the ten-minute floor every time
 *  it changes.  The base property's pspec carries the global 1 s
 *  minimum, so a deserialiser or a user dialling the spinner down can
 *  drop it below our floor; this notify hook catches both. */
static void
on_period_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnFiat *self = PN_FIAT (object);
    guint   period;

    (void) pspec;
    (void) user_data;

    if (self->period_clamping)
        return;

    g_object_get (object, "period", &period, NULL);
    if (period < PN_FIAT_PERIOD_MIN)
    {
        self->period_clamping = TRUE;
        g_object_set (object, "period", PN_FIAT_PERIOD_MIN, NULL);
        self->period_clamping = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  PnHttpClass overrides                                              */
/* ------------------------------------------------------------------ */

/** Configured iff the URL is non-empty AND the pair is not the
 *  degenerate same-currency case, which would otherwise burn a
 *  request per period to learn that one euro is one euro. */
static gboolean
pn_fiat_is_configured (PnHttp *http)
{
    PnFiat   *self = PN_FIAT (http);
    gchar    *url  = pn_http_dup_url (http);
    gboolean  ok   = (url != NULL && *url != '\0' &&
                      fiat_get_from_locked (self) !=
                      fiat_get_to_locked   (self));

    g_free (url);
    return ok;
}

/** Build the request.  `base` re-bases the ECB's euro-denominated
 *  table on the source currency and `symbols` trims the reply to the
 *  one rate we want, so the answer needs no pivot and no arithmetic
 *  beyond reading a single member. */
static SoupMessage *
pn_fiat_build_request (PnHttp *http)
{
    PnFiat      *self = PN_FIAT (http);
    gchar       *url  = pn_http_dup_url (http);
    const gchar *from = fiat_info (fiat_get_from_locked (self))->code;
    const gchar *to   = fiat_info (fiat_get_to_locked   (self))->code;
    gchar       *full_url;
    SoupMessage *msg;

    /* is_configured already rules out from == to and an empty URL. */
    full_url = g_strdup_printf ("%s?base=%s&symbols=%s",
                                url ? url : "", from, to);

    msg = soup_message_new (SOUP_METHOD_GET, full_url);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_free (full_url);
    g_free (url);
    return msg;
}

/** Parse the reply, store the rate, and stash it under the mutex.  We
 *  deliberately do not emit a downstream message: the rate update is
 *  internal state, the conversion happens on the receive path, and
 *  emitting a "rate ticked" event each period would surprise sinks
 *  that expect one message out per message in.
 *
 *  Every exit that is not a successful update routes through
 *  fiat_record_failure(), which both records a human-readable status
 *  for the settings dialog and logs the reason to the per-node log —
 *  a node frozen on a stale rate has to say why. */
static void
pn_fiat_emit_message (
        PnHttp      *http,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    PnFiat         *self = PN_FIAT (http);
    JsonParser     *parser;
    JsonNode       *root;
    JsonNode       *rates_node;
    JsonObject     *obj;
    JsonObject     *rates;
    GError         *error = NULL;
    PnFiatCurrency  from;
    PnFiatCurrency  to;
    const gchar    *to_code;
    const gchar    *fixing_date = NULL;
    gdouble         new_rate;

    /* Transport-level failure: no HTTP response at all (DNS, connect,
     * timeout).  pn_http_trigger has already logged the raw error;
     * record it as the node's status too. */
    if (!ok)
    {
        fiat_record_failure (self, "Request failed: %s",
                             error_text ? error_text : "unknown error");
        return;
    }

    /* HTTP-level failure: the body on a non-2xx is an API error
     * object, not a rate table, so do not fall through to the parse —
     * it would find no rate and report that instead of the status. */
    if (http_status < 200 || http_status >= 300)
    {
        fiat_record_failure (self, "exchange-rate API returned HTTP %d",
                             http_status);
        return;
    }

    if (body == NULL || *body == '\0')
    {
        fiat_record_failure (self, "empty reply from the exchange-rate API");
        return;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body, -1, &error))
    {
        fiat_record_failure (self,
                             "could not parse the exchange-rate reply: %s",
                             error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        fiat_record_failure (self,
                             "unexpected shape in the exchange-rate reply");
        g_object_unref (parser);
        return;
    }
    obj = json_node_get_object (root);

    from    = fiat_get_from_locked (self);
    to      = fiat_get_to_locked   (self);
    to_code = fiat_info (to)->code;

    rates_node = json_object_has_member (obj, "rates")
        ? json_object_get_member (obj, "rates") : NULL;
    if (rates_node == NULL || !JSON_NODE_HOLDS_OBJECT (rates_node))
    {
        fiat_record_failure (self, "no rate table in the reply");
        g_object_unref (parser);
        return;
    }
    rates = json_node_get_object (rates_node);

    if (!json_object_has_member (rates, to_code))
    {
        fiat_record_failure (self, "no usable %s/%s rate in the reply",
                             fiat_info (from)->code, to_code);
        g_object_unref (parser);
        return;
    }

    new_rate = json_object_get_double_member (rates, to_code);
    if (!isfinite (new_rate) || new_rate <= 0.0)
    {
        fiat_record_failure (self, "implausible %s/%s rate in the reply",
                             fiat_info (from)->code, to_code);
        g_object_unref (parser);
        return;
    }

    /* Optional: the working day the rates were fixed on. */
    if (json_object_has_member (obj, "date"))
    {
        JsonNode *date_node = json_object_get_member (obj, "date");
        if (date_node != NULL && JSON_NODE_HOLDS_VALUE (date_node) &&
            json_node_get_value_type (date_node) == G_TYPE_STRING)
            fixing_date = json_node_get_string (date_node);
    }

    /* Success.  fiat_record_success stores the new state under the
     * mutex and bounces a canvas refresh to the main thread.  No
     * notifications are fired from here: they would run on the worker
     * thread and could trip GTK from inside a dialog binding watching
     * these properties.  The save path reads through the mutex-guarded
     * getter, and the dialog re-reads on next open. */
    fiat_record_success (self, new_rate, fixing_date);

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass.receive                                                */
/* ------------------------------------------------------------------ */

/** Multiply the incoming `data.value` by the cached rate and forward.
 *  A non-numeric or missing value is left alone — the message still
 *  passes so chains downstream of a not-yet-fetched converter do not
 *  stall.  The result is stamped with the conversion details (`rate`,
 *  `currency`, `deprecated`), member for member what the FX Converter
 *  writes, so the same Format / Debug wiring reads either node. */
static void
pn_fiat_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnFiat         *self = PN_FIAT (node);
    JsonNode       *value_node;
    PnFiatCurrency  to;
    gdouble         rate;
    gboolean        deprecated;

    g_mutex_lock (&self->mutex);
    rate       = self->rate;
    to         = self->to;
    deprecated = (self->last_update == NULL || *self->last_update == '\0');
    g_mutex_unlock (&self->mutex);

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
    pn_message_set_string (message, "currency", fiat_info (to)->code);

    /* Flag the conversion as untrustworthy while the rate is
     * deprecated (no successful fetch yet for the current pair, or the
     * last one failed).  The value still passes converted so chains do
     * not stall; downstream Debug / Format nodes can surface it. */
    pn_message_set_boolean (message, "deprecated", deprecated);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_fiat_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFiat *self = PN_FIAT (object);

    switch (prop_id)
    {
    case PROP_FROM:
        g_value_set_enum (value, fiat_get_from_locked (self));
        break;
    case PROP_TO:
        g_value_set_enum (value, fiat_get_to_locked (self));
        break;
    case PROP_RATE:
        g_value_set_double (value, fiat_get_rate_locked (self));
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
pn_fiat_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFiat *self = PN_FIAT (object);
    PnHttp *http = PN_HTTP (self);

    switch (prop_id)
    {
    case PROP_FROM:
    case PROP_TO:
    {
        PnFiatCurrency  new_value = (PnFiatCurrency) g_value_get_enum (value);
        gboolean        changed;

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
            /* The cache is keyed on the (from, to) pair; the rate
             * stored a moment ago answers a different question now.
             * Dropping the timestamp both marks the rate deprecated at
             * once and lets the freshness gate in pn_fiat_trigger fall
             * through to a real fetch — the kick below would otherwise
             * be swallowed whenever the previous fetch was recent. */
            g_free (self->last_update);
            self->last_update = NULL;
            /* The new pair has never been fetched; reset the status so
             * a stale "OK" is not left next to a deprecated rate. */
            g_free (self->status);
            self->status = g_strdup (PN_FIAT_STATUS_NEVER);
        }
        g_mutex_unlock (&self->mutex);

        /* On the main thread already (property setter), so refresh the
         * canvas synchronously; this also paints the error marker for
         * the freshly-deprecated rate. */
        fiat_refresh_visual (self);

        /* Picking a different pair invalidates the cached rate.  Kick
         * the worker so the new conversion lands within seconds rather
         * than after a full period — the hourly floor is there to stop
         * polling spam, not deliberate user-driven refreshes. */
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
        /* Restoring the timestamp un-deprecates the cached rate, so
         * the error marker has to come back off.  This matters on
         * every worksheet load: the properties bag applies `from` /
         * `to` first, and a saved pair that differs from the
         * constructor default trips the pair-change branch above,
         * which clears the timestamp and paints the node red.  The
         * `last-update` member that follows puts the cache back. */
        fiat_refresh_visual (self);
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
pn_fiat_finalize (GObject *object)
{
    PnFiat *self = PN_FIAT (object);

    g_clear_pointer (&self->last_update, g_free);
    g_clear_pointer (&self->status, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_fiat_parent_class)->finalize (object);
}

static void
pn_fiat_class_init (PnFiatClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnHttpClass        *http_class    = PN_HTTP_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_fiat_get_property;
    object_class->set_property = pn_fiat_set_property;
    object_class->finalize     = pn_fiat_finalize;
    node_class->receive        = pn_fiat_receive;
    trigger_class->trigger     = pn_fiat_trigger;

    /* Visual identity.  Banknote green, deliberately unlike the FX
     * Converter's gold, so the two converters are told apart on a
     * worksheet that carries both. */
    node_class->palette_icon = PN_FIAT_NORMAL_ICON;
    node_class->class_name   = "Fiat Converter";
    node_class->icon         = PN_FIAT_NORMAL_ICON;
    node_class->color        = (PnColor){ 0.24, 0.55, 0.35, 1.0 };
    node_class->category     = "Filters/Compute & AI";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_FIAT_NORMAL_ICON;
    http_class->normal_color = (PnColor){ 0.24, 0.55, 0.35, 1.0 };

    http_class->is_configured = pn_fiat_is_configured;
    http_class->build_request = pn_fiat_build_request;
    http_class->emit_message  = pn_fiat_emit_message;

    props[PROP_FROM] = g_param_spec_enum (
            "from", "From",
            "Source currency.  The incoming `data.value` is treated "
            "as an amount in this currency before the rate is "
            "applied.",
            PN_TYPE_FIAT_CURRENCY, PN_FIAT_CURRENCY_EUR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TO] = g_param_spec_enum (
            "to", "To",
            "Destination currency.  The outgoing `data.value` is the "
            "input expressed in this currency.",
            PN_TYPE_FIAT_CURRENCY, PN_FIAT_CURRENCY_USD,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* `rate` and `last-update` are READABLE+WRITABLE so the worksheet
     * (de)serialisation in pn-flow.c picks them up automatically and
     * the cached value survives save/load — the whole point of
     * caching it. */
    props[PROP_RATE] = g_param_spec_double (
            "rate", "Rate",
            "Cached conversion factor (1 unit of `from` equals this "
            "many units of `to`).  Updated periodically by the auto-"
            "trigger and persisted in the worksheet so re-opening the "
            "file does not fire a fresh request.",
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
     * read-only in the settings dialog.  READWRITE so the dialog
     * enumerates it and it survives save/load; the node writes it
     * through the mutex-guarded field rather than the setter on the
     * fetch worker. */
    props[PROP_STATUS] = g_param_spec_string (
            "status", "Status",
            "Outcome of the most recent rate fetch.",
            PN_FIAT_STATUS_NEVER,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_fiat_init (PnFiat *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->from        = PN_FIAT_CURRENCY_EUR;
    self->to          = PN_FIAT_CURRENCY_USD;
    self->rate        = 1.0;
    self->last_update = NULL;
    self->status      = g_strdup (PN_FIAT_STATUS_NEVER);

    pn_node_set_class_name (node, "Fiat Converter");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Default endpoint and a polite refresh cadence.  The notify hook
     * clamps any later attempt to dial the period below the ten-minute
     * floor — set the default first, then attach the hook so the
     * legitimate default-set does not trip the guard. */
    g_object_set (self, "url", PN_FIAT_DEFAULT_ENDPOINT, NULL);
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_FIAT_PERIOD_DEFAULT);
    g_signal_connect (self, "notify::period",
                      G_CALLBACK (on_period_notify), NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFiat *
pn_fiat_new (void)
{
    return g_object_new (PN_TYPE_FIAT, NULL);
}

const gchar *
pn_fiat_currency_get_code (PnFiatCurrency cur)
{
    return fiat_info (cur)->code;
}

const gchar *
pn_fiat_currency_get_name (PnFiatCurrency cur)
{
    return fiat_info (cur)->name;
}
