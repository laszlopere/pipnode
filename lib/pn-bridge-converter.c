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
/*  PnBridgeConverter — logic tier (headless core).                    */
/*                                                                     */
/*  The GTK-free half of the Bridge Converter node.  The provider      */
/*  back-ends are the shared ones in pn-bridge-common.c; what is here  */
/*  is the input-driven half: receive() parks the incoming message     */
/*  with the amount it carries and kicks the inherited auto-trigger    */
/*  worker, the worker performs one blocking request through the       */
/*  #PnHttp transport, and the reply is stamped onto that same message */
/*  and emitted from the main loop.                                    */
/*                                                                     */
/*  The auto-trigger machinery is used purely as a worker thread: the  */
/*  trigger override returns immediately unless receive() has parked   */
/*  something, so the node never polls a provider on its own.          */
/*  #PnAutoTrigger:period is pinned and hidden — it would only be a    */
/*  misleading knob on a node that does not tick — and serves solely   */
/*  as the per-request socket timeout the base class derives from it.  */
/*                                                                     */
/*  The settings-dialog customisation (the icon+ticker currency combos */
/*  and the read-only result rows) lives in the companion gui-tier     */
/*  file pn-bridge-converter-gui.c.  The headless runtime registers    */
/*  and runs this node without ever pulling GTK.                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-bridge-converter.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>
#include <stdarg.h>

/* Visual identity.  fa-share (U+F064) — an arrow leaving a corner, for
 * "send this amount through the bridge".  Distinct from Bridge Quote's
 * fa-retweet and from the FX Converter's fa-exchange, both of which sit
 * next to this node on the same worksheets. */
#define PN_BRIDGE_CONVERTER_ICON "\xef\x81\xa4"

/* Body colour: the same teal family as Bridge Quote, a shade deeper —
 * the two ask the same providers the same question, and reading as a
 * pair is more use than reading as strangers. */
#define PN_BRIDGE_CONVERTER_COLOR { 0.16, 0.52, 0.62, 1.0 }

/* The node does not tick, so #PnAutoTrigger:period is not a poll
 * interval here: it is pinned to this value and used only for the two
 * things the base class derives from it — the socket timeout of a quote
 * request (period - 1) and how often the idle worker wakes to find
 * nothing parked.  Thirty seconds is a generous ceiling for a quote and
 * costs two wakeups a minute on a node that is otherwise asleep. */
#define PN_BRIDGE_CONVERTER_PERIOD 30u

/* Status strings, kept as named constants so "nothing has come through
 * yet" reads the same wherever it is set. */
#define PN_BRIDGE_CONVERTER_STATUS_NEVER "Never converted"
#define PN_BRIDGE_CONVERTER_STATUS_OK    "OK"

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnBridgeConverter
{
    PnHttp parent_instance;

    /* @bridge / @from / @to are read on the worker thread and written by
     * the main-thread property setter; the parked request and everything
     * a fetch produces cross the same boundary the other way.  All of it
     * rides one mutex. */
    GMutex      mutex;
    PnBridge    bridge;
    PnCurrency  from;
    PnCurrency  to;

    /* The amount waiting to be quoted and the message it arrived on,
     * parked by receive() for the worker to pick up.  At most one: a
     * newer amount replaces an older one rather than queueing behind it.
     * @pending_message is %NULL exactly when nothing is parked. */
    gdouble     pending_amount;
    PnMessage  *pending_message;

    /* The message the worker is currently answering, moved out of the
     * pending slot at the top of the trigger; the amount that goes with
     * it is @amount, which the same step publishes so build_request,
     * emit_message and the dialog all read one number. */
    PnMessage  *active_message;

    /* Result of the last conversion.  Persisted with the worksheet so
     * reopening a flow still shows what last came through, in the same
     * rows the sibling Bridge Quote node uses.  @amount is set when the
     * request goes out rather than when it comes back, so a failed
     * conversion still says what was asked — and so a test can drive the
     * parse path by setting it directly. */
    gdouble     amount;        /* what was last quoted, in `from` units */
    gdouble     quote;         /* what the bridge would pay for it */
    gdouble     rate;          /* quote / amount, fees included */
    gdouble     fee;           /* provider fee fraction; 0 = not published */
    gdouble     min_amount;    /* provider limits in `from` units; */
    gdouble     max_amount;    /* 0 = unknown or uncapped */
    gchar      *last_update;   /* ISO-8601; NULL until the first success */
    gchar      *status;        /* human-readable outcome */

    /* Advisory extras from the last reply.  Not persisted: they describe
     * one conversion, not a durable setting. */
    gchar      *speed_forecast;
    gchar      *warning;
    gboolean    swappable;

    /* TRUE when the last conversion failed.  Transient — a freshly
     * loaded worksheet has not failed at anything yet, so unlike the
     * Bridge Quote node this one never paints itself red merely for
     * having no result: it has nothing to report until something is sent
     * through it. */
    gboolean    failed;

    /* Re-entrancy guard for the period pin: re-setting the property from
     * its own notify handler fires another notify. */
    gboolean    period_pinning;
};

G_DEFINE_TYPE (PnBridgeConverter, pn_bridge_converter, PN_TYPE_HTTP)

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

/* ------------------------------------------------------------------ */
/*  State accessors (thread-safe)                                      */
/* ------------------------------------------------------------------ */

/** Snapshot what is being quoted: the three settings and the amount of
 *  the request the worker has in hand, read together under the mutex so
 *  the worker never sees half of an edit. */
static void
bridge_converter_get_request (
        PnBridgeConverter *self,
        PnBridgeRequest   *out)
{
    g_mutex_lock (&self->mutex);
    out->bridge = self->bridge;
    out->from   = self->from;
    out->to     = self->to;
    out->amount = self->amount;
    g_mutex_unlock (&self->mutex);
}

/** Recompute the canvas appearance.  MAIN THREAD ONLY.  The node shows
 *  the error marker while it is unconfigured — an unsupported pair, no
 *  URL — or when the last conversion failed.  Note that never having
 *  converted anything is not an error: a converter with nothing wired
 *  into it yet is idle, not broken. */
static void
bridge_converter_refresh_visual (PnBridgeConverter *self)
{
    PnHttp   *http       = PN_HTTP (self);
    gboolean  configured = PN_HTTP_GET_CLASS (self)->is_configured (http);
    gboolean  failed;

    g_mutex_lock (&self->mutex);
    failed = self->failed;
    g_mutex_unlock (&self->mutex);

    pn_http_apply_visual_state (http, configured);
    if (configured && failed)
        pn_node_set_has_error (PN_NODE (self), TRUE);
}

static gboolean
bridge_converter_refresh_visual_main (gpointer data)
{
    bridge_converter_refresh_visual (PN_BRIDGE_CONVERTER (data));
    return G_SOURCE_REMOVE;
}

/** Bounce bridge_converter_refresh_visual() onto the main thread, for
 *  callers on the fetch worker.  A no-op when no main loop is running
 *  (the headless test path), which is fine: those callers assert on
 *  state rather than on pixels. */
static void
bridge_converter_refresh_visual_async (PnBridgeConverter *self)
{
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                bridge_converter_refresh_visual_main,
                                g_object_ref (self), g_object_unref);
}

/* ------------------------------------------------------------------ */
/*  Emission                                                           */
/*                                                                     */
/*  The reply is stamped onto the very message that asked for it and    */
/*  forwarded, the way the FX Converter forwards what it converts, so   */
/*  fields an upstream node set survive the round trip.  What differs   */
/*  is *when*: a bridge has to be asked about this specific amount, so   */
/*  the message leaves on the far side of a network request rather than */
/*  from receive().                                                     */
/* ------------------------------------------------------------------ */

/** Take the message the worker is currently answering out of the active
 *  slot.  Only the worker touches that slot, between the trigger that
 *  filled it and the emission that empties it, so a request can never
 *  answer with a message that belongs to another one. */
static PnMessage *
bridge_converter_take_active (PnBridgeConverter *self)
{
    PnMessage *msg;

    g_mutex_lock (&self->mutex);
    msg = self->active_message;
    self->active_message = NULL;
    g_mutex_unlock (&self->mutex);

    return msg;
}

/** Stamp the current result onto @msg and emit it.  @msg is the message
 *  that asked for this conversion (transfer full; %NULL builds a fresh
 *  one), so every request answers exactly once.
 *
 *  Only fields the provider actually published are stamped: a zero `fee`
 *  or `max_amount` means "not published", and inventing a 0 for it would
 *  read downstream as a genuine zero fee or a zero cap. */
static void
bridge_converter_emit (
        PnBridgeConverter *self,
        PnMessage         *msg,
        gboolean           success)
{
    PnNode         *node = PN_NODE (self);
    PnBridgeRequest set;
    gdouble         amount, quote, rate, fee, min_amount, max_amount;
    gboolean        swappable;
    gchar          *status   = NULL;
    gchar          *forecast = NULL;
    gchar          *warning  = NULL;
    const gchar    *from_nick;
    const gchar    *to_nick;
    gchar          *output;

    bridge_converter_get_request (self, &set);

    g_mutex_lock (&self->mutex);
    amount     = self->amount;
    quote      = self->quote;
    rate       = self->rate;
    fee        = self->fee;
    min_amount = self->min_amount;
    max_amount = self->max_amount;
    swappable  = self->swappable;
    status     = g_strdup (self->status ? self->status : "");
    forecast   = g_strdup (self->speed_forecast ? self->speed_forecast : "");
    warning    = g_strdup (self->warning ? self->warning : "");
    g_mutex_unlock (&self->mutex);

    /* Defensive: every caller hands over the message that asked, but an
     * emission without one is better than a dropped result. */
    if (msg == NULL)
        msg = pn_message_new (node, NULL);

    from_nick = pn_bridge_currency_nick (set.from);
    to_nick   = pn_bridge_currency_nick (set.to);

    if (success)
        output = g_strdup_printf ("%s: %.10g %s \xe2\x86\x92 %.8g %s "
                                  "(rate %.6g)",
                                  pn_bridge_get_display_name (set.bridge),
                                  amount, from_nick, quote, to_nick, rate);
    else
        output = g_strdup_printf ("%s %s\xe2\x86\x92%s: %s",
                                  pn_bridge_get_display_name (set.bridge),
                                  from_nick, to_nick, status);

    /* `value` is the canonical number of the message contract: what this
     * bridge would actually pay out for the amount that came in. */
    pn_message_set_double  (msg, "value",      quote);
    pn_message_set_string  (msg, "output",     output);
    pn_message_set_boolean (msg, "success",    success);
    pn_message_set_double  (msg, "rate",       rate);
    pn_message_set_double  (msg, "amount",     amount);
    pn_message_set_string  (msg, "from",       from_nick);
    pn_message_set_string  (msg, "to",         to_nick);
    pn_message_set_string  (msg, "bridge",
                            pn_bridge_get_display_name (set.bridge));
    /* Kept for parity with Bridge Quote, so one downstream chain can
     * read either node: a conversion that did not come off is exactly a
     * number not to be trusted. */
    pn_message_set_boolean (msg, "deprecated", !success);
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

/** Worker-thread success path: store the conversion and a timestamp
 *  under the mutex, then repaint on the main thread.  No
 *  g_object_notify from here — it would run on the worker and could trip
 *  GTK from a dialog binding watching these properties. */
static void
bridge_converter_record_success (
        PnBridgeConverter *self,
        gdouble            quote,
        gdouble            amount,
        const gchar       *note)
{
    GDateTime *now = g_date_time_new_now_local ();
    gchar     *iso = g_date_time_format_iso8601 (now);

    g_mutex_lock (&self->mutex);
    self->amount = amount;
    self->quote  = quote;
    self->rate   = (amount > 0.0) ? quote / amount : 0.0;
    self->failed = FALSE;
    g_free (self->last_update);
    self->last_update = iso;                 /* takes ownership */
    g_free (self->status);
    /* A caveat that does not invalidate the quote (a closed payout pool,
     * say) rides alongside the OK rather than replacing it. */
    self->status = (note != NULL && *note != '\0')
            ? g_strdup_printf ("%s \xe2\x80\x94 %s",
                               PN_BRIDGE_CONVERTER_STATUS_OK, note)
            : g_strdup (PN_BRIDGE_CONVERTER_STATUS_OK);
    g_mutex_unlock (&self->mutex);

    g_date_time_unref (now);
    bridge_converter_refresh_visual_async (self);
}

/** Failure path: record a human-readable status, surface the reason in
 *  the per-node log (the app runs from a desktop launcher with no
 *  terminal, so a g_warning would be invisible) — and still emit, with
 *  success = FALSE, because a converter that answers nothing at all
 *  stalls the chain it sits in.  @reason is the bare cause; the stored
 *  status prefixes it with "Conversion failed: ".
 *
 *  @msg is the message that asked (transfer full).  Safe on either
 *  thread: it is reached from receive() on the main thread when there is
 *  nothing to quote, and from the worker when a request comes back
 *  empty-handed. */
static void G_GNUC_PRINTF (3, 4)
bridge_converter_record_failure (
        PnBridgeConverter *self,
        PnMessage         *msg,
        const gchar       *fmt,
        ...)
{
    va_list  ap;
    gchar   *reason;

    va_start (ap, fmt);
    reason = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    g_mutex_lock (&self->mutex);
    g_free (self->status);
    self->status = g_strdup_printf ("Conversion failed: %s", reason);
    self->failed = TRUE;
    g_mutex_unlock (&self->mutex);

    pn_auto_trigger_log_on_main (PN_AUTO_TRIGGER (self),
                                 PN_LOG_LEVEL_ERROR, "%s", reason);
    bridge_converter_refresh_visual_async (self);
    bridge_converter_emit (self, msg, FALSE);
    g_free (reason);
}

/** Fold a finished #PnBridgeResult into the node's state and emit.  Runs
 *  on the worker thread.  The limits are stored whether or not the quote
 *  came off, because a reply that names the cap the amount just broke is
 *  exactly the case where the user needs to see it. */
static void
bridge_converter_apply_result (
        PnBridgeConverter    *self,
        const PnBridgeResult *result,
        gdouble               amount)
{
    g_mutex_lock (&self->mutex);
    if (result->min_amount > 0.0 || result->max_amount > 0.0)
    {
        self->min_amount = result->min_amount;
        self->max_amount = result->max_amount;
    }
    g_mutex_unlock (&self->mutex);

    if (!result->ok)
    {
        bridge_converter_record_failure (self,
                                         bridge_converter_take_active (self),
                                         "%s", result->error);
        return;
    }

    g_mutex_lock (&self->mutex);
    self->fee       = result->fee;
    self->swappable = result->swappable;
    g_free (self->speed_forecast);
    self->speed_forecast = g_strdup (result->speed_forecast);
    g_free (self->warning);
    self->warning = g_strdup (result->warning);
    g_mutex_unlock (&self->mutex);

    bridge_converter_record_success (self, result->quote, amount,
                                     result->warning);
    bridge_converter_emit (self, bridge_converter_take_active (self), TRUE);
}

/* ------------------------------------------------------------------ */
/*  PnHttpClass overrides                                              */
/* ------------------------------------------------------------------ */

/** Configured iff the URL is set and the selected provider lists both
 *  assets.  Unlike Bridge Quote there is no amount to check here: the
 *  amount is not a setting, it arrives on the wire, and a node waiting
 *  for its first message is configured, not broken. */
static gboolean
pn_bridge_converter_is_configured (PnHttp *http)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (http);
    gchar             *url  = pn_http_dup_url (http);
    PnBridge           bridge;
    PnCurrency         from;
    PnCurrency         to;
    gboolean           ok;

    g_mutex_lock (&self->mutex);
    bridge = self->bridge;
    from   = self->from;
    to     = self->to;
    g_mutex_unlock (&self->mutex);

    ok = (url != NULL && *url != '\0') &&
         pn_bridge_pair_is_supported (bridge, from, to);

    g_free (url);
    return ok;
}

static SoupMessage *
pn_bridge_converter_build_request (PnHttp *http)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (http);
    gchar             *url  = pn_http_dup_url (http);
    PnBridgeRequest    set;
    gchar             *full_url;
    SoupMessage       *msg;

    bridge_converter_get_request (self, &set);
    full_url = pn_bridge_build_url (&set, url);

    msg = soup_message_new (SOUP_METHOD_GET, full_url);
    if (msg != NULL)
        soup_message_headers_replace (soup_message_get_request_headers (msg),
                                      "Accept", "application/json");

    g_free (full_url);
    g_free (url);
    return msg;
}

static void
pn_bridge_converter_emit_message (
        PnHttp      *http,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error_text)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (http);
    PnBridgeRequest    set;
    PnBridgeResult     result;
    gchar             *url;
    guint              period;

    /* Transport-level failure: no HTTP response at all.  pn_http_trigger
     * has logged the raw error; record it as the node's status too. */
    if (!ok)
    {
        bridge_converter_record_failure (
                self, bridge_converter_take_active (self),
                "Request failed: %s",
                error_text ? error_text : "unknown error");
        return;
    }

    bridge_converter_get_request (self, &set);
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
    bridge_converter_apply_result (self, &result, set.amount);
    pn_bridge_result_clear (&result);
}

/* ------------------------------------------------------------------ */
/*  Trigger override: the worker is a one-shot, not a clock            */
/*                                                                     */
/*  #PnAutoTrigger gives every #PnHttp node a worker thread that can    */
/*  block on a socket without freezing the UI.  This node wants the     */
/*  thread but not the schedule, so the tick does nothing unless        */
/*  receive() has parked an amount: an ordinary wakeup finds the        */
/*  pending slot empty and goes straight back to sleep, and a kick from */
/*  receive() finds a request and runs it.                             */
/* ------------------------------------------------------------------ */

static void
pn_bridge_converter_trigger (PnAutoTrigger *trigger)
{
    PnBridgeConverter  *self   = PN_BRIDGE_CONVERTER (trigger);
    PnAutoTriggerClass *parent =
            PN_AUTO_TRIGGER_CLASS (pn_bridge_converter_parent_class);

    /* Move the parked request into the active slot.  Anything that
     * arrives from here on is a fresh request that will kick again. */
    g_mutex_lock (&self->mutex);
    if (self->pending_message == NULL)
    {
        g_mutex_unlock (&self->mutex);
        return;                              /* an idle wakeup */
    }
    g_clear_object (&self->active_message);
    self->active_message  = self->pending_message;
    self->amount          = self->pending_amount;
    self->pending_message = NULL;
    g_mutex_unlock (&self->mutex);

    /* PnHttp::trigger returns silently when the node is unconfigured,
     * which would strand the message we just took.  Answer it here
     * instead: a chain waiting on this node deserves a reason. */
    if (!PN_HTTP_GET_CLASS (self)->is_configured (PN_HTTP (self)))
    {
        PnBridgeRequest set;

        bridge_converter_get_request (self, &set);
        bridge_converter_record_failure (
                self, bridge_converter_take_active (self),
                "%s does not trade %s \xe2\x86\x92 %s, or no endpoint is set",
                pn_bridge_get_display_name (set.bridge),
                pn_bridge_currency_nick (set.from),
                pn_bridge_currency_nick (set.to));
        return;
    }

    if (parent->trigger != NULL)
        parent->trigger (trigger);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass.receive                                                */
/* ------------------------------------------------------------------ */

/** Park the incoming message and its amount, then wake the worker.
 *  Nothing is emitted here: the answer is a network round-trip away, and
 *  passing the message through unconverted in the meantime would look
 *  exactly like a conversion that happened to be the identity.
 *
 *  A message with no numeric `data.value` has no amount to quote, so it
 *  is answered immediately with a failure rather than sent to a provider
 *  that would only reject it. */
static void
pn_bridge_converter_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (node);
    JsonNode          *value_node;
    gdouble            amount = 0.0;
    gboolean           have_amount = FALSE;
    gboolean           superseded;

    value_node = pn_message_get_member (message, "value");
    if (value_node != NULL && JSON_NODE_HOLDS_VALUE (value_node))
    {
        GType vt = json_node_get_value_type (value_node);

        if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
        {
            amount      = json_node_get_double (value_node);
            have_amount = TRUE;
        }
    }

    /* Nothing a provider could price: answer it here rather than spend a
     * request on a rejection.  The message still goes out — with
     * success = FALSE and a reason — so the chain does not simply
     * stall. */
    if (!have_amount)
    {
        bridge_converter_record_failure (
                self, g_object_ref (message),
                "the incoming message carries no numeric data.value to "
                "quote");
        return;
    }
    if (!(amount > 0.0))
    {
        bridge_converter_record_failure (
                self, g_object_ref (message),
                "nothing to quote: data.value is %.10g", amount);
        return;
    }

    g_mutex_lock (&self->mutex);
    /* At most one amount waits.  A newer one replaces it: quoting a
     * backlog of amounts nobody is looking at any more would spend the
     * provider's goodwill on stale questions. */
    superseded = (self->pending_message != NULL);
    g_clear_object (&self->pending_message);
    self->pending_message = g_object_ref (message);
    self->pending_amount  = amount;
    g_mutex_unlock (&self->mutex);

    if (superseded)
        pn_node_log (node, PN_LOG_LEVEL_INFO,
                     "a newer amount arrived while a quote was still in "
                     "flight; the older one was dropped");

    pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
}

/* ------------------------------------------------------------------ */
/*  Period pinning                                                     */
/* ------------------------------------------------------------------ */

/** Hold #PnAutoTrigger:period at our fixed value.  The row is hidden
 *  from the settings dialog, but the property is still writable — a
 *  hand-edited worksheet or the D-Bus automation surface can set it —
 *  and a period that drifted would silently change the request timeout
 *  on a node that never polls.  Guarded against the recursion its own
 *  re-set would cause. */
static void
on_period_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (object);
    guint              period;

    (void) pspec;
    (void) user_data;

    if (self->period_pinning)
        return;

    g_object_get (object, "period", &period, NULL);
    if (period != PN_BRIDGE_CONVERTER_PERIOD)
    {
        self->period_pinning = TRUE;
        g_object_set (object, "period", PN_BRIDGE_CONVERTER_PERIOD, NULL);
        self->period_pinning = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

/** Drop the cached result because the question it answered has changed.
 *  Call with @self->mutex held. */
static void
bridge_converter_invalidate_locked (PnBridgeConverter *self)
{
    g_free (self->last_update);
    self->last_update = NULL;
    g_free (self->status);
    self->status = g_strdup (PN_BRIDGE_CONVERTER_STATUS_NEVER);
    /* The limits and the fee belong to the old provider or pair too. */
    self->fee        = 0.0;
    self->min_amount = 0.0;
    self->max_amount = 0.0;
    g_free (self->speed_forecast);
    self->speed_forecast = NULL;
    g_free (self->warning);
    self->warning   = NULL;
    self->swappable = TRUE;
    /* Re-pointing the node is not a failure; clear the error mark so a
     * node fixed by switching provider stops looking broken. */
    self->failed = FALSE;
}

static void
pn_bridge_converter_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (object);

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
pn_bridge_converter_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (object);

    switch (prop_id)
    {
    case PROP_BRIDGE:
    case PROP_FROM:
    case PROP_TO:
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
        default:
        {
            PnCurrency v = (PnCurrency) g_value_get_enum (value);
            changed  = (self->to != v);
            self->to = v;
            break;
        }
        }

        if (changed)
            bridge_converter_invalidate_locked (self);
        g_mutex_unlock (&self->mutex);

        /* Each provider has its own endpoint, so follow the selection.
         * Done outside the lock: the URL setter repaints, which reads
         * back through is_configured. */
        if (bridge_moved)
            g_object_set (self, "url",
                          pn_bridge_get_default_url (new_bridge), NULL);

        /* Nothing to kick: this node only asks when something is sent
         * through it.  A repaint is all a settings change needs, and
         * unlike the polling sibling it is safe to do here — the visual
         * does not depend on a `last-update` that a worksheet load has
         * not restored yet. */
        bridge_converter_refresh_visual (self);
        break;
    }
    case PROP_AMOUNT:
        g_mutex_lock (&self->mutex);
        self->amount = g_value_get_double (value);
        g_mutex_unlock (&self->mutex);
        break;
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
pn_bridge_converter_dispose (GObject *object)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (object);

    /* The parent's dispose joins the worker; drop the parked messages
     * first so neither outlives the node that would have emitted it. */
    g_mutex_lock (&self->mutex);
    g_clear_object (&self->pending_message);
    g_clear_object (&self->active_message);
    g_mutex_unlock (&self->mutex);

    G_OBJECT_CLASS (pn_bridge_converter_parent_class)->dispose (object);
}

static void
pn_bridge_converter_finalize (GObject *object)
{
    PnBridgeConverter *self = PN_BRIDGE_CONVERTER (object);

    g_clear_pointer (&self->last_update, g_free);
    g_clear_pointer (&self->status, g_free);
    g_clear_pointer (&self->speed_forecast, g_free);
    g_clear_pointer (&self->warning, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_bridge_converter_parent_class)->finalize (object);
}

static void
pn_bridge_converter_class_init (PnBridgeConverterClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnHttpClass        *http_class    = PN_HTTP_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);
    PnColor             color         = PN_BRIDGE_CONVERTER_COLOR;

    object_class->get_property = pn_bridge_converter_get_property;
    object_class->set_property = pn_bridge_converter_set_property;
    object_class->dispose      = pn_bridge_converter_dispose;
    object_class->finalize     = pn_bridge_converter_finalize;
    node_class->receive        = pn_bridge_converter_receive;
    trigger_class->trigger     = pn_bridge_converter_trigger;

    /* Visual identity. */
    node_class->palette_icon = PN_BRIDGE_CONVERTER_ICON;
    node_class->class_name   = "Bridge Converter";
    node_class->icon         = PN_BRIDGE_CONVERTER_ICON;
    node_class->color        = color;
    node_class->category     = "Network";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_BRIDGE_CONVERTER_ICON;
    http_class->normal_color = color;

    http_class->is_configured = pn_bridge_converter_is_configured;
    http_class->build_request = pn_bridge_converter_build_request;
    http_class->emit_message  = pn_bridge_converter_emit_message;

    props[PROP_BRIDGE] = g_param_spec_enum (
            "bridge", "Bridge",
            "Swap provider to quote.  Changing it re-points the URL at "
            "that provider's endpoint and drops the last result.",
            PN_TYPE_BRIDGE, PN_BRIDGE_PULSELN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FROM] = g_param_spec_enum (
            "from", "From",
            "Asset being sold.  The incoming `data.value` is a quantity "
            "in this currency.",
            PN_TYPE_CURRENCY, PN_CURRENCY_PLS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TO] = g_param_spec_enum (
            "to", "To",
            "Asset being bought.  The emitted `data.value` is the payout "
            "in this currency.",
            PN_TYPE_CURRENCY, PN_CURRENCY_BNB,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Everything below is result state.  It is READABLE+WRITABLE so the
     * worksheet (de)serialisation in pn-flow.c picks it up automatically
     * and the settings dialog still shows what last came through after a
     * reopen — unlike the polling sibling, nothing is re-emitted from it. */
    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "The amount last converted, in `from` units — the "
            "`data.value` of the message that asked.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_QUOTE] = g_param_spec_double (
            "quote", "Quote",
            "The payout for `amount`: how much of `to` the bridge would "
            "deliver, fees included.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RATE] = g_param_spec_double (
            "rate", "Rate",
            "Effective rate, `quote` divided by `amount` — what one unit "
            "of `from` really fetched after the provider's spread and "
            "fees.  Comparable across bridges at the same size.",
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
            "ISO-8601 timestamp of the last successful conversion.  Empty "
            "until the first message has been through.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_STATUS] = g_param_spec_string (
            "status", "Status",
            "Outcome of the most recent conversion.",
            PN_BRIDGE_CONVERTER_STATUS_NEVER,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Hide the inherited period row.  The node has no schedule: period
     * is pinned (see on_period_notify) and means nothing a user would
     * want to set, so offering the spinner would only invite the
     * question "how often does this poll?" — answer: never. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_row       (schema, "period", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "period", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_bridge_converter_init (PnBridgeConverter *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->bridge      = PN_BRIDGE_PULSELN;
    self->from        = PN_CURRENCY_PLS;
    self->to          = PN_CURRENCY_BNB;
    self->status      = g_strdup (PN_BRIDGE_CONVERTER_STATUS_NEVER);
    self->swappable   = TRUE;

    pn_node_set_class_name (node, "Bridge Converter");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    /* Default endpoint, and the pinned period.  Set it before attaching
     * the pin hook so the legitimate default-set does not trip the
     * re-entrancy guard. */
    g_object_set (self, "url",
                  pn_bridge_get_default_url (PN_BRIDGE_PULSELN), NULL);
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_BRIDGE_CONVERTER_PERIOD);
    g_signal_connect (self, "notify::period",
                      G_CALLBACK (on_period_notify), NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnBridgeConverter *
pn_bridge_converter_new (void)
{
    return g_object_new (PN_TYPE_BRIDGE_CONVERTER, NULL);
}
