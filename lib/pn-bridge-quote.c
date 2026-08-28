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
/*  The GTK-free half of the Bridge Quote node: the GType, all          */
/*  properties, the cache-aware periodic fetch and the message it emits */
/*  on every tick.  The provider back-ends themselves — the ticker      */
/*  tables, PulseLN's fee arithmetic, ChangeNOW's error vocabulary —    */
/*  live in pn-bridge-common.c, shared with #PnBridgeConverter, which   */
/*  asks the same providers the same question for an amount that        */
/*  arrives on an input port instead of a fixed one.  The              */
/*  settings-dialog customisation (icon+ticker currency combos,         */
/*  read-only cache rows) lives in the companion gui-tier file          */
/*  pn-bridge-quote-gui.c, which installs that vfunc slot onto this     */
/*  class at editor startup (see pn_bridge_quote_gui_install).  The     */
/*  headless runtime registers and runs this node without ever pulling  */
/*  GTK.                                                                */
/*                                                                     */
/*  The fetch rides the inherited #PnHttp transport: one blocking       */
/*  libsoup GET per tick on the auto-trigger's worker thread.  Only    */
/*  ChangeNOW needs a second request (its min/max range lives on a     */
/*  separate endpoint), which pn_bridge_fetch_range issues from        */
/*  emit_message on that same worker thread.                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-bridge-quote.h"
#include "pn-message.h"

#include <math.h>
#include <stdarg.h>

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

/** Snapshot the four settings that describe "what are we quoting", so
 *  the worker reads a coherent set rather than four separately-locked
 *  fields that a dialog could change in between. */
static void
bridge_quote_get_settings (
        PnBridgeQuote   *self,
        PnBridgeRequest *out)
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
    PnNode         *node = PN_NODE (self);
    PnMessage      *msg  = pn_message_new (node, NULL);
    PnBridgeRequest set;
    gdouble         quote, rate, fee, min_amount, max_amount;
    gboolean        deprecated;
    gboolean        swappable;
    gchar          *status   = NULL;
    gchar          *forecast = NULL;
    gchar          *warning  = NULL;
    const gchar    *from_nick;
    const gchar    *to_nick;
    gchar          *output;

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

    from_nick = pn_bridge_currency_nick (set.from);
    to_nick   = pn_bridge_currency_nick (set.to);

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
/*  PnHttpClass overrides                                              */
/* ------------------------------------------------------------------ */

/** Configured iff the URL is set, the pair is not degenerate, the amount
 *  is positive and the selected provider actually lists both assets. */
static gboolean
pn_bridge_quote_is_configured (PnHttp *http)
{
    PnBridgeQuote  *self = PN_BRIDGE_QUOTE (http);
    gchar          *url  = pn_http_dup_url (http);
    PnBridgeRequest set;
    gboolean        ok;

    bridge_quote_get_settings (self, &set);

    ok = (url != NULL && *url != '\0') &&
         (set.amount > 0.0) &&
         pn_bridge_pair_is_supported (set.bridge, set.from, set.to);

    g_free (url);
    return ok;
}

static SoupMessage *
pn_bridge_quote_build_request (PnHttp *http)
{
    PnBridgeQuote  *self = PN_BRIDGE_QUOTE (http);
    gchar          *url  = pn_http_dup_url (http);
    PnBridgeRequest set;
    gchar          *full_url;
    SoupMessage    *msg;

    bridge_quote_get_settings (self, &set);
    full_url = pn_bridge_build_url (&set, url);

    msg = soup_message_new (SOUP_METHOD_GET, full_url);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_free (full_url);
    g_free (url);
    return msg;
}

/** Fold a finished #PnBridgeResult into the node's cached state and emit.
 *  Runs on the worker thread.  The limits are stored whether or not the
 *  quote came off, because a reply that names the cap the amount just
 *  broke is exactly the case where the user needs to see it. */
static void
bridge_quote_apply_result (
        PnBridgeQuote        *self,
        const PnBridgeResult *result,
        gdouble               amount)
{
    if (result->min_amount > 0.0 || result->max_amount > 0.0)
        bridge_quote_set_limits (self, result->min_amount,
                                 result->max_amount);

    if (!result->ok)
    {
        bridge_quote_record_failure (self, "%s", result->error);
        return;
    }

    bridge_quote_set_fee       (self, result->fee);
    bridge_quote_set_extras    (self, result->speed_forecast, result->warning);
    bridge_quote_set_swappable (self, result->swappable);

    /* A caveat that does not invalidate the quote — a closed payout pool,
     * an amount outside the band — rides alongside the OK status. */
    bridge_quote_record_success (self, result->quote, amount,
                                 result->warning);
    bridge_quote_emit_from_cache (self, TRUE);
}

static void
pn_bridge_quote_emit_message (
        PnHttp      *http,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    PnBridgeQuote  *self = PN_BRIDGE_QUOTE (http);
    PnBridgeRequest set;
    PnBridgeResult  result;
    gchar          *url;
    guint           period;

    /* Transport-level failure: no HTTP response at all.  pn_http_trigger
     * has logged the raw error; record it as the node's status too. */
    if (!ok)
    {
        bridge_quote_record_failure (self, "Request failed: %s",
                                     error_text ? error_text : "unknown error");
        return;
    }

    bridge_quote_get_settings (self, &set);
    pn_bridge_result_init (&result);

    /* ChangeNOW answers a rejected amount with HTTP 400 and a JSON error
     * body, so the range lookup has to run before the status is judged —
     * it is what lets the error text name the actual cap instead of
     * reporting a bare "HTTP 400".  A no-op for every other provider. */
    url    = pn_http_dup_url (http);
    period = pn_auto_trigger_get_period (PN_AUTO_TRIGGER (self));
    pn_bridge_fetch_range (&set, url, (period > 1u) ? period - 1u : 1u,
                           &result);
    g_free (url);

    pn_bridge_parse_reply (&set, http_status, body, &result);
    bridge_quote_apply_result (self, &result, set.amount);
    pn_bridge_result_clear (&result);
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
