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

#include "pn-mqtt-sink.h"

#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-subst.h"
#include "pn-flow.h"
#include "pn-vault.h"
#include "pn-mqtt-profile.h"
#include "pn-mqtt-util.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>
#include <mosquitto.h>

#include <errno.h>

/* Healthy node glyph.  Matches PnMqtt: green fa-paper-plane while the
 * session is up.  The error indication (red body + warning glyph) is
 * painted centrally by the worksheet when pn_node_set_has_error() is set,
 * so the node only carries its normal "all good" icon here. */
#define PN_MQTT_SINK_NORMAL_ICON  "\xef\x87\x98"  /* fa-paper-plane U+F1D8 */

/* No built-in broker URL: like PnMqtt, a site-specific address does not
 * belong in the plugin source.  The broker comes from the referenced
 * mqtt-broker vault profile (the host-managed local setting), so a paired
 * Source + Sink following the same primary profile talk to the same broker
 * with no per-node setup; the inline url is the empty legacy fallback. */
#define PN_MQTT_SINK_DEFAULT_URL ""

/* Reconnect back-off bounds.  Keep-alive now comes from the broker profile
 * (default 60); see pn_mqtt_keepalive_seconds() in pn-mqtt-util. */
#define PN_MQTT_SINK_RECONNECT_DELAY_MIN   1u
#define PN_MQTT_SINK_RECONNECT_DELAY_MAX  30u

/* Offline publish queue.  While the broker session is down, publishes
 * are held here instead of dropped so a command issued at document load
 * -- before the async connect has completed -- still reaches the relay
 * once the link comes up (and likewise across a mid-session broker
 * reconnect).  Two bounds keep this from misbehaving:
 *
 *   - LEN:  a hard cap on the backlog so a prolonged outage cannot grow
 *           memory without limit; once full, the oldest entry is evicted
 *           to make room for the newest (most recent intent wins).
 *   - AGE:  on reconnect only entries still younger than this are
 *           flushed -- a "turn the relay on" that has been waiting for
 *           minutes is more likely stale than useful, whereas a command
 *           from the document load that just happened is exactly what
 *           the user wants enforced.  Chosen to comfortably cover
 *           load + async-connect (and a broker that comes up a little
 *           after the app) without resurrecting long-dead intents.
 *
 * The LEN / AGE bounds and the queue policy itself live in pn-mqtt-util.h
 * (PN_MQTT_SINK_QUEUE_MAX_LEN / _MAX_AGE_US) so they can be unit-tested. */

/* Private data.  Hidden in the .c so subclasses (PnMqttSink is now a
 * derivable type) need not know the field layout; they reach the base's
 * behaviour through the class vfuncs declared in pn-mqtt-sink.h.
 *
 * Mutated only on the main thread (property setters), read by both the
 * main thread (receive path) and libmosquitto's network thread
 * (callbacks).  The callbacks only read priv->connected (via the
 * conn-state trampoline) so no mutex is required -- the publish path
 * itself happens on the main thread inside receive, before handing bytes
 * to mosquitto_publish which has its own internal locking. */
typedef struct
{
    /* Connection identity — prefer the referenced "mqtt-broker" vault
     * profile (broker_profile holds its id, "" follows the primary); the
     * inline url/username/password/client_id are the legacy fallback that
     * keeps pre-v5 files working and seeds the one-time profile import. */
    gchar  *broker_profile;
    gchar  *url;
    gchar  *topic_template;   /* "" -> use inbound envelope topic    */
    gchar  *payload_template; /* "" -> use inbound data.payload      */
    gchar  *username;
    gchar  *password;
    gchar  *client_id;
    guint   qos;
    gboolean retain;

    /* One-shot post-load migration idle (see PnMqtt); 0 when unscheduled. */
    guint   migrate_idle_id;

    /* Debounced (re)connect idle (see PnMqtt): setters and vault changes
     * coalesce into one restart_client run, and the initial connect is
     * deferred to constructed() rather than _init() (review C2). */
    guint   restart_idle_id;

    struct mosquitto *client;
    gboolean          loop_running;
    gboolean          connected;

    /* Processing-glow guard for an in-flight connect attempt.  Set when
     * restart_client kicks off the async connect (the loop thread is
     * doing the TCP/TLS/CONNACK handshake — real background work the
     * receive wrap never sees) and cleared the moment that attempt
     * resolves in the conn-state trampoline, or when the client is torn
     * down first.  Touched only on the main thread (restart_client and
     * the trampoline both run there), so it pairs the begin/end without
     * extra locking. */
    gboolean          connecting;

    /* Per-client network context + generation counter; the conn-state
     * trampoline drops updates whose generation no longer matches so a
     * stale session cannot flip a freshly restarted node (review H2).
     * net_ctx is owned here and freed when its client is destroyed. */
    gpointer          net_ctx;
    guint             generation;

    /* Sticky "the last publish failed" flag.  A Sink has no output port
     * to emit a failure message on, so a publish that mosquitto rejects
     * (broken connection, payload too large, …) would otherwise be
     * invisible.  We flip the node to its error visual while it is set
     * and clear it again on the next successful publish or on reconnect.
     * Touched only on the main thread (receive + connect trampoline). */
    gboolean          publish_error;

    /* Offline publish backlog (PnMqttPending*, head = oldest).
     * Touched only on the main thread -- both the receive path and the
     * connect trampoline run there -- so it needs no locking. */
    GQueue           *pending;

    /* Resolved broker URL the backlog is buffered for; NULL until the
     * first restart resolves one.  Compared on every (re)connect so a
     * runtime broker change drops a backlog meant for the old broker
     * rather than misdelivering it to the new one (review M2). */
    gchar            *broker_identity;
} PnMqttSinkPrivate;

/* Per-client network context (the user data handed to mosquitto_new): a
 * borrowed pointer back to the node plus the generation the client was
 * created with, so the network-thread callbacks tag each state update with
 * their own generation for the trampoline's staleness check (review H2). */
typedef struct
{
    PnMqttSink *self;       /* borrowed */
    guint       generation;
} PnMqttSinkNetCtx;

G_DEFINE_TYPE_WITH_PRIVATE (PnMqttSink, pn_mqtt_sink, PN_TYPE_NODE)

#define PRIV(self) ((PnMqttSinkPrivate *) pn_mqtt_sink_get_instance_private (self))

enum {
    PROP_0,
    PROP_BROKER_PROFILE,
    PROP_URL,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_CLIENT_ID,
    PROP_TOPIC,
    PROP_PAYLOAD,
    PROP_RETAIN,
    PROP_QOS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void restart_client   (PnMqttSink *self);
static void schedule_restart (PnMqttSink *self);

/* ------------------------------------------------------------------ */
/*  Error reporting funnel                                             */
/*                                                                     */
/*  Every error the base senses is routed through the report_error     */
/*  class vfunc rather than calling pn_node_log_error directly, so a    */
/*  subclass that overrides the vfunc observes (and can react to) the   */
/*  whole set -- publish rejections, connect refusals, invalid URLs,    */
/*  auth/TLS setup failures.  Always invoked on the main thread (the    */
/*  network-thread callbacks marshal their reason through the conn-     */
/*  state trampoline first), so the default node-log call and any       */
/*  subclass GObject work are safe.                                     */
/* ------------------------------------------------------------------ */

static void
report_error (PnMqttSink *self, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
report_error (PnMqttSink *self, const gchar *fmt, ...)
{
    va_list  args;
    gchar   *msg;

    va_start (args, fmt);
    msg = g_strdup_vprintf (fmt, args);
    va_end (args);

    PN_MQTT_SINK_GET_CLASS (self)->report_error (self, msg);
    g_free (msg);
}

/* ------------------------------------------------------------------ */
/*  Library init (one-shot)                                            */
/*                                                                     */
/*  mosquitto_lib_init() is idempotent in the sense that repeated      */
/*  PnMqtt + PnMqttSink instances on the same worksheet all share a    */
/*  single init -- each subclass guards its own g_once_init_enter      */
/*  rather than the underlying mosquitto_lib_init() being safe to      */
/*  call twice.  libmosquitto itself does *not* document that, so      */
/*  every node type that touches mosquitto runs its own one-shot       */
/*  guard; the call inside the guard is harmless on the second visit   */
/*  because libmosquitto's internals just bump a refcount.             */
/* ------------------------------------------------------------------ */

static void
ensure_mosquitto_initialised (void)
{
    static gsize initialised = 0;

    if (g_once_init_enter (&initialised))
    {
        mosquitto_lib_init ();
        g_once_init_leave (&initialised, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Resolve the connection identity from the referenced "mqtt-broker"
 *  vault profile when one applies, else from the legacy inline fields.
 *  Each out string is freshly allocated; the caller frees. */
static void
resolve_connection (PnMqttSink *self,
                    gchar **out_url,
                    gchar **out_user,
                    gchar **out_pass,
                    gchar **out_cid,
                    PnMqttConnOpts *out_opts)
{
    PnMqttSinkPrivate *priv = PRIV (self);
    PnProfile         *p    = pn_node_get_profile (PN_NODE (self), "broker-profile");

    /* Profile-XOR-inline: a resolved profile (explicit id, or the primary when
     * broker-profile is "") supplies every field; the inline url/credentials
     * are used only in "Custom settings" mode, where pn_node_get_profile
     * returns NULL (see pn_mqtt_resolve_connection). */
    pn_mqtt_resolve_connection (p, priv->url, priv->username, priv->password,
                                priv->client_id,
                                out_url, out_user, out_pass, out_cid, out_opts);
}

/** Flip the node body between the connected (green paper-plane) and
 *  the unconfigured / disconnected (red ❗) appearance.  Same rule as
 *  PnMqtt: needs a non-empty effective URL plus an active broker
 *  session.  The publish topic comes from the inbound message envelope
 *  rather than a per-node property, so per-message routing is the
 *  user's concern upstream (e.g. via PnRewrite) and the node itself has
 *  nothing it could complain about being "unconfigured" beyond the
 *  broker URL. */
static void
apply_visual_state (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv = PRIV (self);
    PnNode            *node = PN_NODE (self);
    gchar             *url  = NULL;
    gboolean           ok;

    PnColor green = { 0.36, 0.66, 0.36, 1.0 };

    resolve_connection (self, &url, NULL, NULL, NULL, NULL);
    ok = priv->connected && url != NULL && *url != '\0' && !priv->publish_error;
    g_free (url);

    /* Keep the healthy green paper-plane identity at all times; the error
     * overlay (red body + warning glyph) is painted by the worksheet
     * whenever has-error is set, so here we only toggle that flag. */
    pn_node_set_color     (node, &green);
    pn_node_set_icon      (node, PN_MQTT_SINK_NORMAL_ICON);
    pn_node_set_has_error (node, !ok);
}

/* ------------------------------------------------------------------ */
/*  Payload extraction                                                 */
/*                                                                     */
/*  The template-expansion and data.payload fallback logic lives in    */
/*  pn-mqtt-util.c so it can be unit-tested without a broker; this is   */
/*  a thin wrapper that supplies the node's template and flow.         */
/* ------------------------------------------------------------------ */

static gboolean
build_payload (
        PnMqttSink *self,
        PnMessage  *message,
        gchar     **out_bytes,
        gsize      *out_len)
{
    PnMqttSinkPrivate *priv = PRIV (self);

    return pn_mqtt_build_payload (priv->payload_template, message,
                                  pn_node_get_flow (PN_NODE (self)),
                                  out_bytes, out_len);
}

/* ------------------------------------------------------------------ */
/*  Offline publish queue                                              */
/*                                                                     */
/*  The bounded-backlog policy (eviction, freshness) lives in          */
/*  pn-mqtt-util.c so it can be unit-tested with an injected clock;     */
/*  these wrappers supply the real clock and keep the mosquitto_publish */
/*  here on the node.                                                   */
/* ------------------------------------------------------------------ */

/** Hold @payload (ownership transferred) for @topic until the broker
 *  session is back. */
static void
enqueue_pending (
        PnMqttSink  *self,
        const gchar *topic,
        gchar       *payload,
        gsize        payload_len,
        int          qos,
        gboolean     retain)
{
    PnMqttSinkPrivate *priv = PRIV (self);

    pn_mqtt_pending_enqueue (priv->pending, topic, payload, payload_len,
                             qos, retain, g_get_monotonic_time ());
}

/** Publish @payload_len bytes on @topic through the live client, guarding the
 *  gsize->int narrowing mosquitto_publish requires: a payload past the MQTT
 *  256MB ceiling (PN_MQTT_MAX_PAYLOAD_BYTES) would truncate to a bogus (often
 *  negative) int, so it is rejected up front with MOSQ_ERR_PAYLOAD_SIZE —
 *  exactly what libmosquitto returns for an oversize payload, so callers
 *  surface it through their normal error path.  Otherwise returns the
 *  mosquitto_publish result.  Requires priv->client to be live. */
static int
sink_publish (
        PnMqttSink  *self,
        const gchar *topic,
        const gchar *payload,
        gsize        payload_len,
        int          qos,
        gboolean     retain)
{
    PnMqttSinkPrivate *priv = PRIV (self);

    if (!pn_mqtt_payload_within_limit (payload_len))
        return MOSQ_ERR_PAYLOAD_SIZE;

    return mosquitto_publish (priv->client, NULL /* mid -- not tracked */,
                              topic, (int) payload_len, payload, qos, retain);
}

/** Publish every queued entry still younger than the freshness window,
 *  oldest first, discarding the rest.  Runs on the main thread from the
 *  connect trampoline once a session is up.  Requires priv->client to
 *  be live and connected. */
static void
flush_pending (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv  = PRIV (self);
    GPtrArray         *fresh = pn_mqtt_pending_collect_fresh (
            priv->pending, g_get_monotonic_time (),
            PN_MQTT_SINK_QUEUE_MAX_AGE_US);
    guint i;

    for (i = 0; i < fresh->len; i++)
    {
        PnMqttPending *p   = g_ptr_array_index (fresh, i);
        int            err = sink_publish (self, p->topic, p->payload,
                                           p->payload_len, p->qos, p->retain);
        if (err == MOSQ_ERR_SUCCESS)
            continue;   /* p is freed by the g_ptr_array_unref below */

        report_error (self, "queued publish to '%s' failed: %s",
                      p->topic, mosquitto_strerror (err));
        priv->publish_error = TRUE;

        if (err == MOSQ_ERR_NOMEM ||
            err == MOSQ_ERR_CONN_LOST ||
            err == MOSQ_ERR_NO_CONN)
        {
            /* Transient: the link dropped out again mid-flush.  Re-queue
             * this entry and every one still unflushed (keeping order and
             * their original timestamps) instead of discarding them, so a
             * buffered startup command survives to the next reconnect — the
             * buffer-and-flush guarantee the queue exists for (review H3).
             * The remaining entries would all hit the same dead link, so we
             * stop here rather than logging an error for each. */
            guint j;
            for (j = i; j < fresh->len; j++)
            {
                g_queue_push_tail (priv->pending, g_ptr_array_index (fresh, j));
                g_ptr_array_index (fresh, j) = NULL;   /* detach: do not free */
            }
            break;
        }
        /* Otherwise a permanent rejection (payload too large, protocol
         * error, …): drop it — re-queueing would just fail forever. */
    }

    g_ptr_array_unref (fresh);   /* frees the entries via pn_mqtt_pending_free */
}

/* ------------------------------------------------------------------ */
/*  Main-thread marshalling                                            */
/*                                                                     */
/*  Same shape as PnMqtt: libmosquitto's threaded loop fires our       */
/*  connect / disconnect callbacks on the network thread; we           */
/*  trampoline through g_main_context_invoke_full to land on the main  */
/*  loop before flipping the visual state.                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnMqttSink *self;        /* strong ref */
    gboolean    connected;
    guint       generation;  /* the client generation this update came from */
    PnLogLevel  level;       /* level for @reason, when non-NULL */
    gchar      *reason;      /* (nullable): line for the node Log dialog */
} PnMqttSinkConnStateClosure;

static gboolean
conn_state_trampoline (gpointer data)
{
    PnMqttSinkConnStateClosure *c    = data;
    PnMqttSink                 *self = c->self;
    PnMqttSinkPrivate          *priv = PRIV (self);

    /* Honour the update only if it comes from the current client: a stale
     * callback from a torn-down session has an out-of-date generation and is
     * dropped, so it cannot resurrect the wrong visual state or flush against
     * a replaced client (review H2). */
    if (priv->client != NULL && c->generation == priv->generation)
    {
        /* This connect attempt has resolved (broker connected, refused,
         * or the link dropped): close the processing glow opened in
         * restart_client.  Only the first resolution after a restart
         * carries the glow; libmosquitto's later auto-reconnects do not
         * relight it. */
        if (priv->connecting)
        {
            pn_node_processing_end (PN_NODE (self));
            priv->connecting = FALSE;
        }

        priv->connected = c->connected;
        /* Session just came up: a fresh link clears any stale publish
         * error, then we drain whatever was held while offline so a
         * command issued before the connect completed (the
         * enforce-on-startup case) reaches the broker now — the flush
         * re-sets the error flag if a queued publish fails. */
        if (c->connected)
        {
            priv->publish_error = FALSE;
            flush_pending (self);
        }
        apply_visual_state (self);
        /* Surface the reason in the per-node Log dialog (we are on the
         * main thread now, so both pn_node_log and the report_error
         * funnel are safe).  Route error-level reasons through the funnel
         * so a subclass senses connection failures too; non-error lines
         * (the "connected" / clean-disconnect notes) log directly. */
        if (c->reason != NULL)
        {
            if (c->level == PN_LOG_LEVEL_ERROR)
                report_error (self, "%s", c->reason);
            else
                pn_node_log (PN_NODE (self), c->level, "%s", c->reason);
        }
    }
    return G_SOURCE_REMOVE;
}

static void
conn_state_closure_free (gpointer data)
{
    PnMqttSinkConnStateClosure *c = data;
    g_object_unref (c->self);
    g_free (c->reason);
    g_free (c);
}

/** Marshal a connection-state change (and an optional printf-style @fmt
 *  reason for the node Log dialog) onto the main thread.  @fmt may be
 *  %NULL for a state-only update. */
static void
post_conn_state_on_main_full (PnMqttSink  *self,
                              guint        generation,
                              gboolean     connected,
                              PnLogLevel   level,
                              const gchar *fmt,
                              ...) G_GNUC_PRINTF (5, 6);

static void
post_conn_state_on_main_full (PnMqttSink  *self,
                              guint        generation,
                              gboolean     connected,
                              PnLogLevel   level,
                              const gchar *fmt,
                              ...)
{
    PnMqttSinkConnStateClosure *c = g_new0 (PnMqttSinkConnStateClosure, 1);
    c->self       = g_object_ref (self);
    c->connected  = connected;
    c->generation = generation;
    c->level      = level;
    if (fmt != NULL)
    {
        va_list args;
        va_start (args, fmt);
        c->reason = g_strdup_vprintf (fmt, args);
        va_end (args);
    }
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                conn_state_trampoline,
                                c,
                                conn_state_closure_free);
}

/** Convenience: state change with no Log-dialog line. */
static void
post_conn_state_on_main (PnMqttSink *self, guint generation, gboolean connected)
{
    post_conn_state_on_main_full (self, generation, connected,
                                  PN_LOG_LEVEL_INFO, NULL);
}

/* ------------------------------------------------------------------ */
/*  libmosquitto callbacks                                             */
/* ------------------------------------------------------------------ */

static void
on_mqtt_connect (
        struct mosquitto *mosq,
        void             *obj,
        int               rc)
{
    PnMqttSinkNetCtx *ctx  = obj;
    PnMqttSink       *self = ctx->self;

    (void) mosq;

    if (rc != 0)
    {
        post_conn_state_on_main_full (self, ctx->generation, FALSE,
                                      PN_LOG_LEVEL_ERROR,
                                      "connect refused: %s",
                                      mosquitto_connack_string (rc));
        return;
    }

    post_conn_state_on_main_full (self, ctx->generation, TRUE,
                                  PN_LOG_LEVEL_INFO, "connected to broker");
}

static void
on_mqtt_disconnect (
        struct mosquitto *mosq,
        void             *obj,
        int               rc)
{
    PnMqttSinkNetCtx *ctx  = obj;
    PnMqttSink       *self = ctx->self;

    (void) mosq;

    if (rc != 0)
    {
        post_conn_state_on_main_full (self, ctx->generation, FALSE,
                                      PN_LOG_LEVEL_WARNING,
                                      "disconnected (%s); reconnecting…",
                                      mosquitto_strerror (rc));
        return;
    }

    post_conn_state_on_main (self, ctx->generation, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_mqtt_sink_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnMqttSink        *self = PN_MQTT_SINK (node);
    PnMqttSinkPrivate *priv = PRIV (self);
    PnMqttSinkClass   *klass = PN_MQTT_SINK_GET_CLASS (self);
    const gchar       *publish_topic;
    gchar             *topic_owned = NULL;
    gchar             *payload = NULL;
    gsize              payload_len = 0;

    /* Late subclass hook: reshape (or drop) the message on its way out
     * before the base resolves topic / payload.  A device-specific Sink
     * subclass overrides process_message to stamp the topic / payload it
     * needs without an upstream PnRewrite; returning FALSE drops it. */
    if (!klass->process_message (self, message))
        return;

    /* Publish topic.  With an explicit template, expand it against this
     * node's variables (${nodeclass} / ${nodename} / ${hostname}) and
     * the incoming message (${topic}, ${data/device}, ...).  Without a
     * template it comes straight off the inbound envelope -- a canvas
     * pattern like `Tasmota Switch -> Rewrite (topic =
     * cmnd/${data/device}/POWER) -> MQTT Sink` then drives the right
     * broker topic per message.  Messages with no resulting topic are
     * dropped. */
    if (priv->topic_template != NULL && *priv->topic_template != '\0')
    {
        JsonObject      *root  = pn_json_lookup_root_for_message (message);
        gchar          **pairs = pn_node_dup_subst_pairs (node);
        PnSubstResolver  vars, msg, globals;
        PnSubstResolver *chain[4];
        PnSubstContext   ctx;

        pn_subst_resolver_strv (&vars, (const gchar * const *) pairs);
        pn_subst_resolver_json (&msg, root);
        pn_flow_subst_resolver_globals (&globals, pn_node_get_flow (node));
        chain[0] = &vars;     /* node vars win */
        chain[1] = &msg;      /* then message fields */
        chain[2] = &globals;  /* then document globals */
        chain[3] = NULL;
        ctx.resolvers = chain;
        ctx.mode      = PN_SUBST_TEXT;
        ctx.miss      = PN_SUBST_MISS_EMPTY;

        topic_owned = pn_subst_expand (priv->topic_template, &ctx);

        g_strfreev (pairs);
        json_object_unref (root);

        publish_topic = topic_owned;
    }
    else
    {
        publish_topic = pn_message_get_topic (message);
    }

    if (publish_topic == NULL || *publish_topic == '\0')
    {
        g_free (topic_owned);
        return;
    }

    if (!build_payload (self, message, &payload, &payload_len))
    {
        g_free (topic_owned);
        return;
    }

    /* Online: publish straight through.  Offline (the common case at
     * document load, before the async connect has completed): hold the
     * publish in the bounded, timestamped backlog and deliver it on
     * connect instead of dropping it, so an enforce-on-startup command
     * still reaches the relay. */
    if (priv->client != NULL && priv->connected)
    {
        gboolean was_error = priv->publish_error;
        int      err       = sink_publish (self, publish_topic, payload,
                                           payload_len, (int) priv->qos,
                                           priv->retain);
        if (err != MOSQ_ERR_SUCCESS)
        {
            report_error (self, "publish to '%s' failed: %s",
                          publish_topic, mosquitto_strerror (err));
            priv->publish_error = TRUE;
        }
        else
        {
            priv->publish_error = FALSE;
        }
        /* Re-render only when the error state actually changed — a
         * healthy high-rate Sink should not repaint on every publish. */
        if (priv->publish_error != was_error)
            apply_visual_state (self);
        g_free (payload);
    }
    else if (!pn_mqtt_payload_within_limit (payload_len))
    {
        /* Oversize even before we try to send: it can never be published, so
         * drop it now with an error rather than buffer a doomed (and possibly
         * huge) payload that the flush would just discard on connect. */
        report_error (self,
                      "publish to '%s' dropped: payload %" G_GSIZE_FORMAT
                      " bytes exceeds the %u-byte MQTT limit",
                      publish_topic, payload_len, PN_MQTT_MAX_PAYLOAD_BYTES);
        if (!priv->publish_error)
        {
            priv->publish_error = TRUE;
            apply_visual_state (self);
        }
        g_free (payload);
    }
    else
    {
        /* Ownership of @payload transfers into the queue entry. */
        enqueue_pending (self, publish_topic, payload, payload_len,
                         (int) priv->qos, priv->retain);
    }

    g_free (topic_owned);
}

/* ------------------------------------------------------------------ */
/*  Client lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
stop_client (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv = PRIV (self);

    /* Tearing the client down before its connect attempt resolved:
     * balance the processing glow opened in restart_client so the
     * begin/end refcount stays even (no perpetual glow). */
    if (priv->connecting)
    {
        pn_node_processing_end (PN_NODE (self));
        priv->connecting = FALSE;
    }

    if (priv->client == NULL)
    {
        /* A half-built client (connect/loop-start failed) may have left a
         * context behind — clear it so the next start begins clean. */
        g_clear_pointer (&priv->net_ctx, g_free);
        return;
    }

    mosquitto_disconnect (priv->client);
    if (priv->loop_running)
    {
        mosquitto_loop_stop (priv->client, TRUE);
        priv->loop_running = FALSE;
    }
    mosquitto_destroy (priv->client);
    priv->client        = NULL;
    priv->connected     = FALSE;
    priv->publish_error = FALSE;

    /* Network thread joined: no callback can still reach the context. */
    g_clear_pointer (&priv->net_ctx, g_free);
}

static void
restart_client (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv = PRIV (self);
    PnMqttSinkNetCtx  *ctx  = NULL;
    gchar             *host = NULL;
    int                port = 0;
    gboolean           tls  = FALSE;
    int                err;
    gchar             *url  = NULL;
    gchar             *user = NULL;
    gchar             *pass = NULL;
    gchar             *cid  = NULL;
    PnMqttConnOpts     opts = { 0 };

    stop_client (self);

    resolve_connection (self, &url, &user, &pass, &cid, &opts);

    /* Retarget guard (review M2): if the broker we are about to (re)connect
     * to differs from the one the backlog was buffered for, those publishes
     * were meant for a broker we have moved away from -- drop them rather
     * than misdeliver to the new broker.  A reconnect to the *same* broker
     * (a transient drop, or a vault edit that did not change the URL) keeps
     * the backlog, which is what the queue is for.  The empty-URL "no broker"
     * state is a distinct identity, so removing the broker also clears it. */
    {
        const gchar *new_id  = (url != NULL) ? url : "";
        guint        dropped = pn_mqtt_pending_retarget (priv->pending,
                                                         priv->broker_identity,
                                                         new_id);
        if (dropped > 0)
            pn_node_log (PN_NODE (self), PN_LOG_LEVEL_INFO,
                         "broker changed; discarded %u buffered publish%s "
                         "queued for the previous broker",
                         dropped, dropped == 1 ? "" : "es");
        g_free (priv->broker_identity);
        priv->broker_identity = g_strdup (new_id);
    }

    if (url == NULL || *url == '\0')
        goto out;

    if (!pn_mqtt_parse_url (url, &host, &port, &tls))
    {
        report_error (self, "invalid broker URL '%s'", url);
        goto out;
    }

    ensure_mosquitto_initialised ();

    /* New generation + back-pointer for this client's callbacks (review H2). */
    ctx             = g_new0 (PnMqttSinkNetCtx, 1);
    ctx->self       = self;
    ctx->generation = ++priv->generation;

    priv->client = mosquitto_new ((cid != NULL && *cid != '\0') ? cid : NULL,
                                  TRUE,
                                  ctx);
    if (priv->client == NULL)
    {
        report_error (self, "client init failed: %s", g_strerror (errno));
        g_clear_pointer (&ctx, g_free);
        goto out;
    }
    priv->net_ctx = ctx;   /* owned; freed in stop_client / fail path */

    mosquitto_connect_callback_set    (priv->client, on_mqtt_connect);
    mosquitto_disconnect_callback_set (priv->client, on_mqtt_disconnect);

    mosquitto_reconnect_delay_set (priv->client,
            PN_MQTT_SINK_RECONNECT_DELAY_MIN,
            PN_MQTT_SINK_RECONNECT_DELAY_MAX,
            TRUE);

    if (user != NULL && *user != '\0')
    {
        err = mosquitto_username_pw_set (priv->client,
                user,
                (pass != NULL && *pass != '\0') ? pass : NULL);
        if (err != MOSQ_ERR_SUCCESS)
            report_error (self, "username/password rejected: %s",
                          mosquitto_strerror (err));
    }

    if (tls)
    {
        /* Trust material from the profile (review M3): a CA bundle (NULL →
         * the system trust store, today's behaviour) plus an optional client
         * cert/key pair for mutual TLS.  Empty strings mean "unset". */
        const gchar *cafile = (opts.ca_file     && *opts.ca_file)     ? opts.ca_file     : NULL;
        const gchar *cert   = (opts.client_cert && *opts.client_cert) ? opts.client_cert : NULL;
        const gchar *key    = (opts.client_key  && *opts.client_key)  ? opts.client_key  : NULL;

        /* libmosquitto needs both halves of an mTLS pair or neither; a lone
         * one is a misconfiguration, so drop it rather than fail the connect. */
        if ((cert != NULL) != (key != NULL))
        {
            report_error (self,
                          "mutual TLS needs both a client certificate and a "
                          "key; ignoring the lone %s",
                          cert ? "certificate" : "key");
            cert = key = NULL;
        }

        err = mosquitto_tls_set (priv->client, cafile, NULL, cert, key, NULL);
        if (err != MOSQ_ERR_SUCCESS)
            report_error (self, "TLS setup failed: %s",
                          mosquitto_strerror (err));
        else if (opts.tls_insecure)
        {
            /* Opt-in, dev-only: accept a certificate that does not chain to a
             * trusted CA or match the hostname.  Logged so it is never silent. */
            mosquitto_tls_insecure_set (priv->client, TRUE);
            pn_node_log (PN_NODE (self), PN_LOG_LEVEL_WARNING,
                         "TLS certificate verification disabled (insecure)");
        }
    }

    err = mosquitto_connect_async (priv->client, host, port,
                                   pn_mqtt_keepalive_seconds (opts.keepalive));
    if (err != MOSQ_ERR_SUCCESS)
    {
        report_error (self, "connect to %s:%d failed: %s",
                      host, port, mosquitto_strerror (err));
        goto fail_destroy;
    }

    err = mosquitto_loop_start (priv->client);
    if (err != MOSQ_ERR_SUCCESS)
    {
        report_error (self, "network loop failed to start: %s",
                      mosquitto_strerror (err));
        goto fail_destroy;
    }
    priv->loop_running = TRUE;

    /* The async connect is now in flight on the network thread: light
     * the processing glow for the whole handshake.  Closed by the
     * conn-state trampoline when the attempt resolves, or by stop_client
     * if the node is restarted/disposed first. */
    pn_node_processing_begin (PN_NODE (self));
    priv->connecting = TRUE;
    goto out;

fail_destroy:
    /* The loop never started, so no callback ran — tear down directly. */
    mosquitto_destroy (priv->client);
    priv->client = NULL;
    g_clear_pointer (&priv->net_ctx, g_free);

out:
    g_free (host);
    g_free (url); g_free (user); g_free (pass); g_free (cid);
    pn_mqtt_conn_opts_clear (&opts);
    apply_visual_state (self);
}

/** Debounced (re)connect: coalesce a burst of setter / vault-change calls
 *  into a single restart_client on the next idle, and defer the initial
 *  connect past construction / deserialization (review C2). */
static gboolean
restart_client_idle (gpointer data)
{
    PnMqttSink        *self = PN_MQTT_SINK (data);
    PnMqttSinkPrivate *priv = PRIV (self);

    priv->restart_idle_id = 0;
    restart_client (self);
    return G_SOURCE_REMOVE;
}

static void
schedule_restart (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv = PRIV (self);

    if (priv->restart_idle_id == 0)
        priv->restart_idle_id = g_idle_add (restart_client_idle, self);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_mqtt_sink_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnMqttSink        *self = PN_MQTT_SINK (object);
    PnMqttSinkPrivate *priv = PRIV (self);

    switch (prop_id)
    {
    case PROP_BROKER_PROFILE: g_value_set_string (value, priv->broker_profile); break;
    case PROP_URL:       g_value_set_string  (value, priv->url);              break;
    case PROP_TOPIC:     g_value_set_string  (value, priv->topic_template);   break;
    case PROP_PAYLOAD:   g_value_set_string  (value, priv->payload_template); break;
    case PROP_RETAIN:    g_value_set_boolean (value, priv->retain);           break;
    case PROP_USERNAME:  g_value_set_string  (value, priv->username);         break;
    case PROP_PASSWORD:  g_value_set_string  (value, priv->password);         break;
    case PROP_CLIENT_ID: g_value_set_string  (value, priv->client_id);        break;
    case PROP_QOS:       g_value_set_uint    (value, priv->qos);              break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_mqtt_sink_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnMqttSink        *self = PN_MQTT_SINK (object);
    PnMqttSinkPrivate *priv = PRIV (self);

    switch (prop_id)
    {
    case PROP_BROKER_PROFILE:
        g_free (priv->broker_profile);
        priv->broker_profile = g_value_dup_string (value);
        schedule_restart (self);
        break;
    case PROP_URL:
        g_free (priv->url);
        priv->url = g_value_dup_string (value);
        schedule_restart (self);
        break;
    case PROP_TOPIC:
        g_free (priv->topic_template);
        priv->topic_template = g_value_dup_string (value);
        break;
    case PROP_PAYLOAD:
        g_free (priv->payload_template);
        priv->payload_template = g_value_dup_string (value);
        break;
    case PROP_RETAIN:
        priv->retain = g_value_get_boolean (value);
        break;
    case PROP_USERNAME:
        g_free (priv->username);
        priv->username = g_value_dup_string (value);
        schedule_restart (self);
        break;
    case PROP_PASSWORD:
        g_free (priv->password);
        priv->password = g_value_dup_string (value);
        schedule_restart (self);
        break;
    case PROP_CLIENT_ID:
        g_free (priv->client_id);
        priv->client_id = g_value_dup_string (value);
        schedule_restart (self);
        break;
    case PROP_QOS:
        priv->qos = g_value_get_uint (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Legacy-credentials migration                                       */
/* ------------------------------------------------------------------ */

/** One-shot, post-load migration of inline credentials into a vault
 *  profile — the publisher counterpart of PnMqtt's migration. */
static gboolean
migrate_legacy_credentials (gpointer data)
{
    PnMqttSink        *self = PN_MQTT_SINK (data);
    PnMqttSinkPrivate *priv = PRIV (self);
    const gchar       *names[]  = { "url", "username", "password", "client-id" };
    const gchar       *values[4];
    gchar             *id;

    priv->migrate_idle_id = 0;

    /* "Custom settings" mode: keep the inline fields as the connection config. */
    if (g_strcmp0 (priv->broker_profile, PN_PROFILE_REF_CUSTOM) == 0)
        return G_SOURCE_REMOVE;

    /* A real profile is selected: inline data is stale (it only exists in Custom
     * mode).  Clear it so the saved file and the dialog widgets are empty. */
    if (priv->broker_profile != NULL && *priv->broker_profile != '\0')
    {
        g_object_set (self, "url", "", "username", "", "password", "",
                      "client-id", "", NULL);
        return G_SOURCE_REMOVE;
    }

    /* broker-profile == "" (Default): new-model Default nodes have empty inline
     * fields, so these legacy branches only fire for pre-broker-profile files. */
    if ((priv->username == NULL || *priv->username == '\0') &&
        (priv->password == NULL || *priv->password == '\0'))
    {
        /* No inline credentials to migrate.  A legacy node with a typed inline
         * url was effectively using "custom settings"; mark it so it keeps that
         * url under the new model. */
        if (priv->url != NULL && *priv->url != '\0')
            g_object_set (self, "broker-profile", PN_PROFILE_REF_CUSTOM, NULL);
        return G_SOURCE_REMOVE;
    }

    values[0] = priv->url       ? priv->url       : "";
    values[1] = priv->username  ? priv->username  : "";
    values[2] = priv->password  ? priv->password  : "";
    values[3] = priv->client_id ? priv->client_id : "";

    id = pn_vault_import_inline_profile (
            PN_PROFILE_TYPE_MQTT_BROKER,
            (priv->url != NULL && *priv->url != '\0') ? priv->url
                                                      : "MQTT broker",
            names, values, G_N_ELEMENTS (names));
    if (id != NULL)
    {
        g_object_set (self, "broker-profile", id, NULL);
        g_free (id);
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* The referenced profile (or the primary it follows) changed in the vault —
 * re-resolve and reconnect so a manager edit takes effect immediately. */
static void
on_vault_changed (gpointer self)
{
    schedule_restart (PN_MQTT_SINK (self));
}

static void
pn_mqtt_sink_constructed (GObject *object)
{
    PnMqttSink        *self = PN_MQTT_SINK (object);
    PnMqttSinkPrivate *priv = PRIV (self);

    G_OBJECT_CLASS (pn_mqtt_sink_parent_class)->constructed (object);

    /* Migration first, then the debounced initial connect, so a broker-profile
     * change the migration makes folds into that same restart (review C2). */
    priv->migrate_idle_id = g_idle_add (migrate_legacy_credentials, self);
    schedule_restart (self);

    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
}

static void
pn_mqtt_sink_dispose (GObject *object)
{
    PnMqttSink        *self = PN_MQTT_SINK (object);
    PnMqttSinkPrivate *priv = PRIV (self);

    if (priv->migrate_idle_id != 0)
    {
        g_source_remove (priv->migrate_idle_id);
        priv->migrate_idle_id = 0;
    }
    if (priv->restart_idle_id != 0)
    {
        g_source_remove (priv->restart_idle_id);
        priv->restart_idle_id = 0;
    }

    /* The offline backlog is a transient, in-memory, best-effort buffer
     * scoped to the live broker session and this node's lifetime; it is
     * not persisted across saves or sessions (review M2).  On teardown
     * (node removed, worksheet closed, app quitting) we give it one last
     * chance: if the link is still up, flush before stop_client destroys
     * the client — the network thread is still running here, so a QoS-0
     * publish reaches the wire.  Anything that cannot go out now (we are
     * offline, or a queued publish failed) is dropped with the node;
     * surface that count rather than discarding it silently in finalize. */
    if (priv->client != NULL && priv->connected)
        flush_pending (self);
    if (priv->pending != NULL)
    {
        guint lost = g_queue_get_length (priv->pending);
        if (lost > 0)
            pn_node_log (PN_NODE (self), PN_LOG_LEVEL_WARNING,
                         "discarding %u buffered publish%s never delivered "
                         "to the broker (the offline backlog is not "
                         "persisted across sessions)",
                         lost, lost == 1 ? "" : "es");
    }

    stop_client (self);

    G_OBJECT_CLASS (pn_mqtt_sink_parent_class)->dispose (object);
}

static void
pn_mqtt_sink_finalize (GObject *object)
{
    PnMqttSink        *self = PN_MQTT_SINK (object);
    PnMqttSinkPrivate *priv = PRIV (self);

    g_clear_pointer (&priv->broker_profile,   g_free);
    g_clear_pointer (&priv->url,              g_free);
    g_clear_pointer (&priv->topic_template,   g_free);
    g_clear_pointer (&priv->payload_template, g_free);
    g_clear_pointer (&priv->username,         g_free);
    g_clear_pointer (&priv->password,         g_free);
    g_clear_pointer (&priv->client_id,        g_free);
    g_clear_pointer (&priv->broker_identity,  g_free);

    /* Belt and suspenders: dispose's stop_client already cleared this. */
    g_clear_pointer (&priv->net_ctx,          g_free);

    if (priv->pending != NULL)
    {
        g_queue_free_full (priv->pending, pn_mqtt_pending_free);
        priv->pending = NULL;
    }

    G_OBJECT_CLASS (pn_mqtt_sink_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Default vfunc implementations                                      */
/* ------------------------------------------------------------------ */

gboolean
pn_mqtt_sink_real_process_message (PnMqttSink *self, PnMessage *message)
{
    (void) self;
    (void) message;
    return TRUE;
}

void
pn_mqtt_sink_real_report_error (PnMqttSink *self, const gchar *message)
{
    pn_node_log_error (PN_NODE (self), "%s", message);
}

static void
pn_mqtt_sink_class_init (PnMqttSinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_mqtt_sink_get_property;
    object_class->set_property = pn_mqtt_sink_set_property;
    object_class->constructed  = pn_mqtt_sink_constructed;
    object_class->dispose      = pn_mqtt_sink_dispose;
    object_class->finalize     = pn_mqtt_sink_finalize;

    klass->process_message     = pn_mqtt_sink_real_process_message;
    klass->report_error        = pn_mqtt_sink_real_report_error;

    node_class->receive        = pn_mqtt_sink_receive;

    /* Palette pin: the instance icon flips with connection state but
     * the palette wants a stable glyph -- pin the paper-plane so the
     * entry reads consistently. */
    node_class->palette_icon   = PN_MQTT_SINK_NORMAL_ICON;
    node_class->class_name     = "MQTT Sink";
    node_class->icon           = PN_MQTT_SINK_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    /* No output port: this Sink never stamps its own base topic onto an
     * emitted message (the publish topic comes from the inbound envelope
     * or the topic-template), so the inherited base "topic" row is dead
     * UI -- hide it from the settings dialog. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_BROKER_PROFILE] = g_param_spec_string (
            "broker-profile", "Broker profile",
            "Id of the mqtt-broker credential profile this node publishes "
            "through.  Empty follows the primary mqtt-broker profile, so a "
            "Source + Sink pair both reach the default broker with no setup; "
            "pick another to target a different server, or \"Custom settings\" "
            "to use this node's own inline url/credentials.  Profile URLs and "
            "credentials live in the host vault, not in the workflow file.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_profile_ref (props[PROP_BROKER_PROFILE],
                                   PN_PROFILE_TYPE_MQTT_BROKER);
    /* Offer a "Custom settings" entry; these inline fields are its config
     * (greyed + cleared unless "Custom settings" is selected). */
    pn_param_spec_set_profile_ref_inline_fields (props[PROP_BROKER_PROFILE],
                                                 "url,username,password,client-id");

    props[PROP_URL] = g_param_spec_string (
            "url", "Broker URL",
            "MQTT broker URL.  Accepts tcp://host[:port] (plain MQTT, "
            "default port 1883), ssl://host[:port] or "
            "mqtts://host[:port] (MQTT over TLS, default port 8883), "
            "or a bare host[:port] which is treated as plain TCP.  "
            "Part of the node's \"Custom settings\": editable only when the "
            "Broker profile picker is on \"Custom settings\", and cleared when "
            "a profile (or Default) is selected.  Empty by default.",
            PN_MQTT_SINK_DEFAULT_URL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_USERNAME] = g_param_spec_string (
            "username", "Username",
            "MQTT username.  Empty disables auth.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PASSWORD] = g_param_spec_string (
            "password", "Password",
            "MQTT password; only sent when username is also set.  Legacy "
            "inline fallback — new configurations keep it in an mqtt-broker "
            "vault profile; tagged secret so it is never written to the "
            "workflow file.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_secret (props[PROP_PASSWORD]);

    props[PROP_CLIENT_ID] = g_param_spec_string (
            "client-id", "Client ID",
            "MQTT client identifier.  Empty asks libmosquitto to "
            "generate a random one -- fine for most cases.  Pin a "
            "fixed id when the broker enforces an ACL by client id "
            "(and use a *different* id from any paired MQTT Source on "
            "the same broker, since two clients with the same id kick "
            "each other off).",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Not profile-related: the publish topic/payload/retain + QoS sit
     * together at the end of the dialog, after the broker/credential
     * fields. */
    props[PROP_TOPIC] = g_param_spec_string (
            "topic-template", "Topic template",
            "Optional publish-topic template.  When empty the publish "
            "topic comes straight off the inbound message envelope (the "
            "common case: a Rewrite step upstream sets the topic).  When "
            "non-empty it is expanded and used as the publish topic: "
            "${nodeclass} / ${nodename} / ${hostname} resolve against "
            "this node, and ${path/to/field} (e.g. ${topic} or "
            "${data/device}) resolves against the incoming message, so "
            "'cmnd/${data/device}/POWER' or 'sensors/${hostname}/temp' "
            "drive the broker topic per message.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PAYLOAD] = g_param_spec_string (
            "payload", "Payload template",
            "Optional payload template.  When empty the inbound "
            "data.payload member is published verbatim -- string "
            "members go out as raw bytes, structured members (object, "
            "array, number, boolean) are serialised back to JSON.  "
            "When non-empty the template is expanded against the "
            "message (same ${path/to/field} syntax as the topic) and "
            "its UTF-8 bytes are published as-is, so '${data/value}' "
            "publishes a stringified number and 'ON' / 'OFF' publishes "
            "a fixed literal.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RETAIN] = g_param_spec_boolean (
            "retain", "Retain",
            "Publish the message as a retained message -- the broker "
            "remembers the last retained publish per topic and "
            "delivers it to every late subscriber as soon as they "
            "subscribe.  Used for device state (Tasmota's POWER "
            "publishes are retained for exactly this reason); leave "
            "off for transient events.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_QOS] = g_param_spec_uint (
            "qos", "Publish QoS",
            "MQTT QoS for outgoing publishes (0 = at most once, 1 = "
            "at least once, 2 = exactly once).  Higher QoS costs "
            "extra round-trips with the broker -- 0 is the right "
            "choice for high-rate telemetry, 1 for state changes you "
            "want acknowledged, 2 only when exactly-once matters and "
            "the broker / network can keep up.",
            0u, 2u, 0u,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_mqtt_sink_init (PnMqttSink *self)
{
    PnMqttSinkPrivate *priv = PRIV (self);
    PnNode            *node = PN_NODE (self);

    priv->broker_profile   = g_strdup ("");
    priv->url              = g_strdup (PN_MQTT_SINK_DEFAULT_URL);
    priv->topic_template   = NULL;
    priv->payload_template = NULL;
    priv->username         = NULL;
    priv->password         = NULL;
    priv->client_id        = NULL;
    priv->qos              = 0;
    priv->retain           = FALSE;
    priv->migrate_idle_id  = 0;
    priv->restart_idle_id  = 0;
    priv->client           = NULL;
    priv->loop_running     = FALSE;
    priv->connected        = FALSE;
    priv->connecting       = FALSE;
    priv->publish_error    = FALSE;
    priv->net_ctx          = NULL;
    priv->generation       = 0;
    priv->pending          = g_queue_new ();
    priv->broker_identity  = NULL;

    /* Class label is pinned on PnNodeClass.class_name in class_init and
     * resolved through pn_node_get_class_name()'s class fallback; do NOT
     * seed it per-instance here.  PnMqttSink is a base class, and a
     * per-instance "MQTT Sink" would shadow the label a subclass pins in
     * its own class_init. */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* The initial connect is scheduled from constructed() (review C2), not
     * here — see pn_mqtt_sink_constructed().  A message arriving before the
     * connect completes is held in the offline queue and flushed on connect,
     * so deferring loses nothing. */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMqttSink *
pn_mqtt_sink_new (void)
{
    return g_object_new (PN_TYPE_MQTT_SINK, NULL);
}
