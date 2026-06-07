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

#include "pn-mqtt.h"
#include "pn-message.h"
#include "pn-vault.h"
#include "pn-mqtt-profile.h"
#include "pn-mqtt-util.h"

#include <mosquitto.h>

#include <errno.h>

/* Healthy node glyph.  The error indication (red body + warning glyph)
 * is painted centrally by the worksheet when pn_node_set_has_error() is
 * set, so the node only carries its normal "all good" icon here. */
#define PN_MQTT_NORMAL_ICON  "\xef\x82\x9e"  /* fa-rss U+F09E */

/* Default topic.  "#" matches every topic on the broker, so a node pointed
 * at a broker produces output without further configuration.  The broker URL
 * has NO built-in default: a site-specific address does not belong in the
 * plugin source, so it comes from the referenced mqtt-broker vault profile
 * (the host-managed local setting) or, for legacy files, the inline url —
 * empty until configured, which shows the node in its unconfigured state. */
#define PN_MQTT_DEFAULT_URL   ""
#define PN_MQTT_DEFAULT_TOPIC "#"

/* Keep-alive comes from the broker profile now (default 60); see
 * pn_mqtt_keepalive_seconds() in pn-mqtt-util. */

/* Reconnect back-off bounds in seconds.  libmosquitto handles the
 * actual reconnect itself when used through the threaded loop; we just
 * tell it how aggressively to retry.  Exponential ramp from 1 s up to
 * 30 s caps recovery time after a transient broker outage without
 * hammering the network on a sustained outage. */
#define PN_MQTT_RECONNECT_DELAY_MIN 1u
#define PN_MQTT_RECONNECT_DELAY_MAX 30u

/* Private data.  Hidden in the .c so subclasses (PnMqtt is now a
 * derivable type) need not know the field layout; they reach the base's
 * behaviour through the class vfuncs declared in pn-mqtt.h.
 *
 * Lifetime / threading:
 *
 *   - Configuration fields (broker_profile, url, topic, username,
 *     password, client_id, qos) are written exclusively on the main
 *     thread (property setters land there) and read by libmosquitto's
 *     network thread only via pointers we hand it before
 *     mosquitto_loop_start(); we never mutate them while a session is
 *     up, so no mutex is required.
 *
 *   - The pending-message queue is shared between the network thread
 *     (push) and the main thread (drain), protected by pending_lock. */
typedef struct
{
    /* Connection identity.  Preferred source is the referenced
     * "mqtt-broker" vault profile (broker_profile holds its id, or "" to
     * follow the primary); url/username/password/client_id are the legacy
     * inline fallback that keeps pre-v5 files working and seeds the one-time
     * import into a profile. */
    gchar  *broker_profile;
    gchar  *url;
    gchar  *topic;
    gchar  *username;
    gchar  *password;
    gchar  *client_id;
    guint   qos;

    /* One-shot idle that migrates a legacy node's inline credentials into a
     * vault profile after its properties have loaded; 0 when not scheduled. */
    guint   migrate_idle_id;

    /* Debounced (re)connect idle.  Property setters and vault changes do
     * not reconnect synchronously — they coalesce into this single idle so
     * loading a document (which fires many setters in a burst) results in
     * one connect cycle, and so the initial connect runs after construction
     * / deserialization is complete rather than from _init() (review C2). */
    guint   restart_idle_id;

    /* Live mosquitto client + a flag tracking whether its background
     * loop thread is running.  Both are NULL / FALSE while the node
     * sits in its disconnected state. */
    struct mosquitto *client;
    gboolean          loop_running;

    /* Per-client network context handed to libmosquitto as its user data,
     * and a monotonically increasing generation counter bumped on every
     * client (re)start.  The network-thread callbacks read the generation
     * from their own context, not from priv, and stamp it on each state
     * update they marshal to the main thread; the trampoline drops any
     * update whose generation no longer matches priv->generation, so a
     * stale session's queued callback cannot flip a freshly restarted node
     * (review H2).  net_ctx is owned here and freed when its client is
     * destroyed. */
    gpointer          net_ctx;
    guint             generation;

    /* Subscribe topic/qos snapshot taken on the main thread when the client
     * is (re)started, read by on_mqtt_connect on the network thread.  Reading
     * the snapshot rather than priv->topic/priv->qos avoids a torn read / UAF
     * when a setter frees priv->topic while libmosquitto's auto-reconnect
     * re-fires on_mqtt_connect (review H1).  Owned here. */
    gchar            *sub_topic;
    int               sub_qos;

    /* Sticky "are we currently connected" flag, flipped by the
     * on_connect / on_disconnect callbacks (marshalled to the main
     * thread).  Used to drive the visual state. */
    gboolean          connected;

    /* Processing-glow guard for an in-flight connect attempt.  Set when
     * restart_client kicks off the async connect (the loop thread runs
     * the TCP/TLS/CONNACK handshake — real background work the receive
     * wrap never sees) and cleared the moment that attempt resolves in
     * the conn-state trampoline, or when the client is torn down first.
     * Touched only on the main thread, so it pairs the begin/end without
     * extra locking. */
    gboolean          connecting;

    /* Inbound-message queue: libmosquitto's network thread builds the
     * #PnMessage on its side, then pushes it here.  A single main-thread
     * drain idle pops the whole batch and emits each — coalescing N
     * per-message main-loop wake-ups into one per burst, which keeps the
     * UI interactive when a broker dumps a retained-state flood at
     * subscribe time (zigbee2mqtt's `zigbee2mqtt/#` is the motivating
     * case).  Bounded by #PN_MQTT_PENDING_MAX so a runaway broker can't
     * grow the queue without limit; the oldest queued message is dropped
     * when the cap is hit. */
    GMutex            pending_lock;
    GQueue            pending;       /* of (PnMessage *), owned */
    guint             flush_idle_id; /* 0 when no drain idle scheduled */
    /* TRUE between deciding to schedule a drain and that drain running.
     * Set under pending_lock so the producer can schedule the idle OUTSIDE
     * the lock (avoiding the [pending_lock -> GMainContext-lock] order that
     * g_idle_add would impose) while still keeping at most one drain
     * pending and discarding a stale id if the drain races ahead. */
    gboolean          flush_scheduled;
} PnMqttPrivate;

/* Hard cap on the pending-message queue.  Sized for a healthy retained
 * burst (a few thousand messages) without unbounded growth under a
 * sustained-flood pathological case. */
#define PN_MQTT_PENDING_MAX 10000

/* Per-client network context (the user data we hand mosquitto_new).  Holds
 * a borrowed pointer back to the node plus the generation this client was
 * created with, so the network-thread callbacks know their own generation
 * without racing a read of priv->generation.  The node owns the live one in
 * priv->net_ctx and frees it when the client is destroyed; the client never
 * outlives the node (dispose joins the network thread first). */
typedef struct
{
    PnMqtt *self;       /* borrowed */
    guint   generation;
} PnMqttNetCtx;

G_DEFINE_TYPE_WITH_PRIVATE (PnMqtt, pn_mqtt, PN_TYPE_NODE)

#define PRIV(self) ((PnMqttPrivate *) pn_mqtt_get_instance_private (self))

enum {
    PROP_0,
    PROP_BROKER_PROFILE,
    PROP_URL,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_CLIENT_ID,
    PROP_TOPIC,
    PROP_QOS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void restart_client  (PnMqtt *self);
static void schedule_restart (PnMqtt *self);

/* ------------------------------------------------------------------ */
/*  Library init (one-shot)                                            */
/*                                                                     */
/*  mosquitto_lib_init() is required before any other libmosquitto     */
/*  call and must run exactly once per process.  We tuck it behind a   */
/*  g_once_init_enter so multiple PnMqtt instances all share a single  */
/*  init; mosquitto_lib_cleanup is intentionally never called — we do  */
/*  not know whether other plugins or future nodes also use            */
/*  libmosquitto, and the leak is bounded to a few small allocations   */
/*  for the lifetime of the process.                                   */
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
/*  Connection identity resolution                                     */
/*                                                                     */
/*  The values libmosquitto actually connects with come from the       */
/*  referenced "mqtt-broker" vault profile when one resolves (an        */
/*  explicit broker_profile id, or the type's primary when it is        */
/*  empty), and otherwise from the legacy inline fields.  Each getter   */
/*  returns a freshly-allocated string the caller frees.               */
/* ------------------------------------------------------------------ */

static void
resolve_connection (PnMqtt *self,
                    gchar **out_url,
                    gchar **out_user,
                    gchar **out_pass,
                    gchar **out_cid,
                    PnMqttConnOpts *out_opts)
{
    PnMqttPrivate *priv = PRIV (self);
    PnProfile     *p    = pn_node_get_profile (PN_NODE (self), "broker-profile");

    /* Profile-XOR-inline: when a profile resolves (an explicit id, or the
     * primary when broker-profile is "") every field comes from it; the inline
     * url/credentials are used only in "Custom settings" mode, where
     * pn_node_get_profile returns NULL (see pn_mqtt_resolve_connection). */
    pn_mqtt_resolve_connection (p, priv->url, priv->username, priv->password,
                                priv->client_id,
                                out_url, out_user, out_pass, out_cid, out_opts);
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Update the node's error indication: it is "ok" when a broker session
 *  is up (@priv->connected) and the effective URL is non-empty.  The node
 *  keeps its healthy green RSS identity at all times; the error overlay
 *  (red body + warning glyph) is painted by the worksheet whenever the
 *  has-error flag is set, so here we only toggle that flag. */
static void
apply_visual_state (PnMqtt *self)
{
    PnMqttPrivate *priv  = PRIV (self);
    PnNode        *node  = PN_NODE (self);
    PnColor        green = { 0.36, 0.66, 0.36, 1.0 };
    gchar         *url   = NULL;
    gboolean       ok;

    resolve_connection (self, &url, NULL, NULL, NULL, NULL);
    ok = priv->connected && url != NULL && *url != '\0';
    g_free (url);

    pn_node_set_color     (node, &green);
    pn_node_set_icon      (node, PN_MQTT_NORMAL_ICON);
    pn_node_set_has_error (node, !ok);
}

/* ------------------------------------------------------------------ */
/*  Main-thread marshalling                                            */
/*                                                                     */
/*  libmosquitto's threaded loop fires our callbacks on its own        */
/*  network thread.  pn_node_emit_message and the rest of the GObject  */
/*  graph live on the main GMainContext, so we trampoline through      */
/*  g_main_context_invoke_full to land back on the main loop before    */
/*  poking the node.                                                   */
/* ------------------------------------------------------------------ */

/** Main-thread drain.  Swaps the pending queue out under the lock in
 *  O(1), clears the scheduled-idle flag (so the network thread can
 *  schedule a fresh one for any message that arrives during the drain),
 *  then emits each queued message on the node — synchronously cascading
 *  through the wired graph in arrival order. */
static gboolean
flush_pending_on_main (gpointer data)
{
    PnMqtt        *self  = PN_MQTT (data);
    PnMqttPrivate *priv  = PRIV (self);
    PnMqttClass   *klass = PN_MQTT_GET_CLASS (self);
    GQueue         local = G_QUEUE_INIT;
    PnMessage     *msg;

    g_mutex_lock (&priv->pending_lock);
    /* Steal the queue's contents in one shot rather than popping under
     * the lock per message — keeps the network thread unblocked while
     * we do the (potentially expensive) per-message graph walk. */
    local.head        = priv->pending.head;
    local.tail        = priv->pending.tail;
    local.length      = priv->pending.length;
    g_queue_init (&priv->pending);
    priv->flush_idle_id   = 0;
    priv->flush_scheduled = FALSE;   /* this drain is running; allow the next */
    g_mutex_unlock (&priv->pending_lock);

    while ((msg = g_queue_pop_head (&local)) != NULL)
    {
        /* Late subclass hook on the constructed message.  Skip the
         * emit if the subclass returns FALSE; we still own @msg and
         * free it on the same line so a dropped message never leaks. */
        if (klass->process_message (self, msg))
            pn_node_emit_message (PN_NODE (self), msg);
        g_object_unref (msg);
    }
    return G_SOURCE_REMOVE;
}

/** Network-thread side: append @message to the pending queue and make
 *  sure a drain idle is scheduled.  Drops the oldest queued message when
 *  the cap is reached — under sustained overflow the user is already
 *  losing the race; keeping the freshest messages is more useful than
 *  the staleest. */
static void
emit_message_on_main (PnMqtt *self, PnMessage *message)
{
    PnMqttPrivate *priv     = PRIV (self);
    PnMessage     *dropped  = NULL;
    gboolean       schedule = FALSE;

    g_mutex_lock (&priv->pending_lock);
    if (priv->pending.length >= PN_MQTT_PENDING_MAX)
        dropped = g_queue_pop_head (&priv->pending);
    g_queue_push_tail (&priv->pending, message);  /* transfer */
    if (!priv->flush_scheduled)
    {
        priv->flush_scheduled = TRUE;
        schedule              = TRUE;
    }
    g_mutex_unlock (&priv->pending_lock);

    /* Schedule the drain OUTSIDE pending_lock.  g_idle_add locks the
     * process-wide GMainContext internally; holding pending_lock across it
     * would fix the producer's lock order as [pending_lock -> context-lock].
     * A teardown on the main thread that holds the context lock while it
     * joins this network thread would then deadlock.  flush_scheduled (set
     * under the lock above) keeps the "one drain pending" invariant without
     * nesting the two locks. */
    if (schedule)
    {
        guint id = g_idle_add (flush_pending_on_main, self);

        /* Adopt the id only if the drain has not already run on the main
         * thread in the meantime -- if it has, flush_scheduled is back to
         * FALSE, the source has auto-removed, and storing its (now stale,
         * reusable) id would let dispose cancel an unrelated source. */
        g_mutex_lock (&priv->pending_lock);
        if (priv->flush_scheduled)
            priv->flush_idle_id = id;
        g_mutex_unlock (&priv->pending_lock);
    }

    if (dropped != NULL)
        g_object_unref (dropped);
}

typedef struct
{
    PnMqtt     *self;        /* strong ref */
    gboolean    connected;
    guint       generation;  /* the client generation this update came from */
    PnLogLevel  level;       /* level for @reason, when non-NULL */
    gchar      *reason;      /* (nullable): line for the node Log dialog */
} PnMqttConnStateClosure;

static gboolean
conn_state_trampoline (gpointer data)
{
    PnMqttConnStateClosure *c    = data;
    PnMqtt                 *self = c->self;
    PnMqttPrivate          *priv = PRIV (self);

    /* Only honour the update if it comes from the current client: the node
     * still holds a client AND the generation matches.  A stale callback
     * from an old session — torn down and replaced while its update sat in
     * the main-loop queue — has an out-of-date generation and is dropped, so
     * it cannot flip a freshly reconnected node back to red (review H2). */
    if (priv->client != NULL && c->generation == priv->generation)
    {
        /* This connect attempt has resolved (connected, refused, or the
         * link dropped): close the processing glow opened in
         * restart_client.  Only the first resolution after a restart
         * carries the glow; libmosquitto's later auto-reconnects do not
         * relight it. */
        if (priv->connecting)
        {
            pn_node_processing_end (PN_NODE (self));
            priv->connecting = FALSE;
        }

        priv->connected = c->connected;
        apply_visual_state (self);
        /* Surface the reason in the per-node Log dialog (we are now on
         * the main thread, so pn_node_log is safe).  Guarded by the same
         * checks so a stale session's message is dropped. */
        if (c->reason != NULL)
            pn_node_log (PN_NODE (self), c->level, "%s", c->reason);
    }
    return G_SOURCE_REMOVE;
}

static void
conn_state_closure_free (gpointer data)
{
    PnMqttConnStateClosure *c = data;
    g_object_unref (c->self);
    g_free (c->reason);
    g_free (c);
}

/** Marshal a connection-state change (and an optional printf-style
 *  @fmt reason for the node Log dialog) onto the main thread.  libmosquitto
 *  fires our callbacks on its network thread, where touching the GObject
 *  / emitting log-changed would be unsafe.  @fmt may be %NULL for a
 *  state-only update. */
static void
post_conn_state_on_main_full (PnMqtt      *self,
                              guint        generation,
                              gboolean     connected,
                              PnLogLevel   level,
                              const gchar *fmt,
                              ...) G_GNUC_PRINTF (5, 6);

static void
post_conn_state_on_main_full (PnMqtt      *self,
                              guint        generation,
                              gboolean     connected,
                              PnLogLevel   level,
                              const gchar *fmt,
                              ...)
{
    PnMqttConnStateClosure *c = g_new0 (PnMqttConnStateClosure, 1);
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
    /* Fire-and-forget onto the main loop, exactly like emit_message_on_main's
     * g_idle_add.  g_main_context_invoke_full() can acquire the context and run
     * synchronously, blocking the network thread on the GMainContext lock.  A
     * teardown that joins this thread from the main thread (window close ->
     * pn_mqtt_dispose -> stop_client -> mosquitto_loop_stop) would then deadlock,
     * because the join cannot complete until the callback runs and the callback
     * cannot run until the join releases the main loop.  g_idle_add only queues
     * the source and returns, so the network thread never blocks and the join
     * completes; the trampoline's generation check still drops stale updates. */
    g_idle_add_full (G_PRIORITY_DEFAULT,
                     conn_state_trampoline,
                     c,
                     conn_state_closure_free);
}

/** Convenience: state change with no Log-dialog line. */
static void
post_conn_state_on_main (PnMqtt *self, guint generation, gboolean connected)
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
    PnMqttNetCtx  *ctx  = obj;
    PnMqtt        *self = ctx->self;
    PnMqttPrivate *priv = PRIV (self);

    if (rc != 0)
    {
        post_conn_state_on_main_full (self, ctx->generation, FALSE,
                                      PN_LOG_LEVEL_ERROR,
                                      "connect refused: %s",
                                      mosquitto_connack_string (rc));
        return;
    }

    /* Re-subscribe on every (re)connect: the broker forgets our
     * subscriptions when the session goes down (we asked for clean
     * sessions in mosquitto_new), so each fresh CONNACK has to be
     * paired with a fresh SUBSCRIBE.  We read the snapshot taken when
     * this client was started (priv->sub_topic/sub_qos), not the live
     * priv->topic/priv->qos: the snapshot is fixed for the client's
     * lifetime and is only replaced after the network thread has been
     * joined, so an auto-reconnect re-firing this callback never races a
     * setter freeing priv->topic (review H1). */
    if (priv->sub_topic != NULL && *priv->sub_topic != '\0')
    {
        int err = mosquitto_subscribe (mosq, NULL, priv->sub_topic,
                                       priv->sub_qos);
        if (err != MOSQ_ERR_SUCCESS)
        {
            /* We have a broker session but the subscription was refused
             * (typically an ACL denial) — the node would sit there
             * receiving nothing.  Flip it to the error state rather than
             * a misleading healthy green so the canvas shows it is not
             * actually working. */
            post_conn_state_on_main_full (self, ctx->generation, FALSE,
                                          PN_LOG_LEVEL_ERROR,
                                          "subscribe('%s') failed: %s",
                                          priv->sub_topic,
                                          mosquitto_strerror (err));
            return;
        }
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
    PnMqttNetCtx *ctx  = obj;
    PnMqtt       *self = ctx->self;

    (void) mosq;

    if (rc != 0)
    {
        /* Unexpected disconnect — libmosquitto's threaded loop will
         * try to reconnect on its own using the configured back-off,
         * so we just flip the visual state and let it work. */
        post_conn_state_on_main_full (self, ctx->generation, FALSE,
                                      PN_LOG_LEVEL_WARNING,
                                      "disconnected (%s); reconnecting…",
                                      mosquitto_strerror (rc));
        return;
    }

    post_conn_state_on_main (self, ctx->generation, FALSE);
}

/** Build a #PnMessage carrying one MQTT publish.  Envelope topic is
 *  the MQTT topic itself (verbatim, no prefix) so a downstream Filter
 *  matches what the broker actually publishes (e.g.
 *  `tele/+/SENSOR`); the data bag follows the standard contract:
 *    - data.payload   – parsed JSON value when the payload is valid
 *                       JSON (object, array, number, string, bool,
 *                       null); otherwise the raw UTF-8 string the
 *                       payload bytes carry; empty string when the
 *                       payload is binary / non-UTF-8
 *    - data.value     – payload parsed as a number, when it parses
 *                       (covers both "23.5" and JSON `23.5`)
 *    - data.output    – short one-liner naming the topic, suitable
 *                       for a debug log; the payload itself is *not*
 *                       inlined (downstream renderers can drill into
 *                       the structured `payload` if they want it)
 *    - data.success   – TRUE
 *    - data.qos       – QoS the publish arrived at
 *    - data.retained  – whether the broker delivered it from retained
 *                       storage rather than as a fresh publish
 *  Built on libmosquitto's network thread — caller marshals it to the
 *  main loop before emitting it. */
static PnMessage *
build_mqtt_message (
        PnMqtt                          *self,
        const struct mosquitto_message  *m)
{
    /* The decode/envelope logic lives in pn-mqtt-util.c so it can be
     * unit-tested without a broker; here we just unpack the libmosquitto
     * message struct into its plain fields. */
    return pn_mqtt_build_message (PN_NODE (self),
                                  m->topic,
                                  (const guchar *) m->payload,
                                  (gsize) m->payloadlen,
                                  m->qos,
                                  m->retain);
}

static void
on_mqtt_message (
        struct mosquitto                 *mosq,
        void                             *obj,
        const struct mosquitto_message   *m)
{
    PnMqttNetCtx *ctx   = obj;
    PnMqtt       *self  = ctx->self;
    PnMqttClass  *klass = PN_MQTT_GET_CLASS (self);
    PnMessage    *msg;

    (void) mosq;

    if (m == NULL)
        return;

    /* Early topic-level subclass filter, runs on libmosquitto's network
     * thread.  Returning FALSE drops the publish before paying the
     * PnMessage build / main-thread marshal cost — the right hook for
     * "ignore everything outside topic prefix X" style filtering. */
    if (!klass->accept_topic (self, m->topic ? m->topic : ""))
        return;

    msg = build_mqtt_message (self, m);
    emit_message_on_main (self, msg);
}

/* ------------------------------------------------------------------ */
/*  Client lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
stop_client (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);

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
        /* No live client, but a half-built one (connect/loop-start failed)
         * may have left a context / snapshot behind — clear them so the
         * next start begins clean. */
        g_clear_pointer (&priv->net_ctx,   g_free);
        g_clear_pointer (&priv->sub_topic, g_free);
        return;
    }

    /* Order matters: ask for a clean MQTT-level disconnect first so
     * the broker sees a DISCONNECT rather than a TCP RST, then stop
     * the loop thread, then destroy the client.  mosquitto_loop_stop
     * blocks until the network thread has joined so the callbacks
     * cannot fire after this returns.
     *
     * force=FALSE (graceful): the disconnect above shuts the socket
     * down, which wakes the loop out of any blocking read(), so the
     * thread exits at a safe point on its own.  We must NOT force
     * (pthread_cancel) here: the network thread can be inside a marshal
     * to the main loop (post_conn_state_on_main_full / emit_message_on_main,
     * both g_idle_add-based) momentarily holding the process-wide
     * GMainContext lock.  Cancelling it there orphans that mutex and
     * deadlocks every later main-loop operation.  Conversely those marshals
     * must stay non-blocking (g_idle_add, never g_main_context_invoke_full):
     * since this join runs on the main thread, a marshal that waited on the
     * main loop would deadlock the join itself (window-close freeze). */
    mosquitto_disconnect (priv->client);
    if (priv->loop_running)
    {
        mosquitto_loop_stop (priv->client, FALSE);
        priv->loop_running = FALSE;
    }
    mosquitto_destroy (priv->client);
    priv->client    = NULL;
    priv->connected = FALSE;

    /* The network thread is joined, so no callback can still reach the
     * context or the subscribe snapshot — safe to free now. */
    g_clear_pointer (&priv->net_ctx,   g_free);
    g_clear_pointer (&priv->sub_topic, g_free);
}

/** Tear down any running client and start a fresh one with the
 *  current configuration.  Reached only through schedule_restart()'s
 *  debounced idle, so it runs once per burst of setter calls and after
 *  construction / deserialization is complete (review C2). */
static void
restart_client (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);
    PnMqttNetCtx  *ctx  = NULL;
    gchar         *host = NULL;
    int            port = 0;
    gboolean       tls  = FALSE;
    int            err;
    gchar         *url  = NULL;
    gchar         *user = NULL;
    gchar         *pass = NULL;
    gchar         *cid  = NULL;
    PnMqttConnOpts opts = { 0 };

    stop_client (self);

    resolve_connection (self, &url, &user, &pass, &cid, &opts);

    if (url == NULL || *url == '\0')
        goto out;

    if (!pn_mqtt_parse_url (url, &host, &port, &tls))
    {
        pn_node_log_error (PN_NODE (self), "invalid broker URL '%s'", url);
        goto out;
    }

    ensure_mosquitto_initialised ();

    /* Stamp a new generation for this client and hand it (with a back
     * pointer to the node) to libmosquitto as the callback user data, so
     * the network-thread callbacks can tag every state update with the
     * generation the trampoline checks (review H2). */
    ctx             = g_new0 (PnMqttNetCtx, 1);
    ctx->self       = self;
    ctx->generation = ++priv->generation;

    /* clean_session = TRUE: we do not persist subscriptions across
     * disconnects (we always re-subscribe in on_connect anyway), so
     * the broker need not remember us between sessions.  NULL
     * client_id lets libmosquitto generate a random one for us when
     * the user has not pinned a fixed id. */
    priv->client = mosquitto_new ((cid != NULL && *cid != '\0') ? cid : NULL,
                                  TRUE,
                                  ctx);
    if (priv->client == NULL)
    {
        pn_node_log_error (PN_NODE (self), "client init failed: %s",
                           g_strerror (errno));
        g_clear_pointer (&ctx, g_free);
        goto out;
    }
    priv->net_ctx = ctx;   /* owned; freed in stop_client / fail path */

    mosquitto_connect_callback_set    (priv->client, on_mqtt_connect);
    mosquitto_disconnect_callback_set (priv->client, on_mqtt_disconnect);
    mosquitto_message_callback_set    (priv->client, on_mqtt_message);

    mosquitto_reconnect_delay_set (priv->client,
            PN_MQTT_RECONNECT_DELAY_MIN,
            PN_MQTT_RECONNECT_DELAY_MAX,
            TRUE);

    if (user != NULL && *user != '\0')
    {
        err = mosquitto_username_pw_set (priv->client,
                user,
                (pass != NULL && *pass != '\0') ? pass : NULL);
        if (err != MOSQ_ERR_SUCCESS)
            pn_node_log_error (PN_NODE (self), "username/password rejected: %s",
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
            pn_node_log_error (PN_NODE (self),
                               "mutual TLS needs both a client certificate and "
                               "a key; ignoring the lone %s",
                               cert ? "certificate" : "key");
            cert = key = NULL;
        }

        err = mosquitto_tls_set (priv->client, cafile, NULL, cert, key, NULL);
        if (err != MOSQ_ERR_SUCCESS)
            pn_node_log_error (PN_NODE (self), "TLS setup failed: %s",
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

    /* Snapshot the subscribe topic/qos for the network thread before the
     * loop starts; on_mqtt_connect reads this, not the live properties
     * (review H1). */
    priv->sub_topic = g_strdup (priv->topic);
    priv->sub_qos   = (int) priv->qos;

    err = mosquitto_connect_async (priv->client, host, port,
                                   pn_mqtt_keepalive_seconds (opts.keepalive));
    if (err != MOSQ_ERR_SUCCESS)
    {
        pn_node_log_error (PN_NODE (self), "connect to %s:%d failed: %s",
                           host, port, mosquitto_strerror (err));
        goto fail_destroy;
    }

    err = mosquitto_loop_start (priv->client);
    if (err != MOSQ_ERR_SUCCESS)
    {
        pn_node_log_error (PN_NODE (self), "network loop failed to start: %s",
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
    /* The loop never started, so no callback ran — tearing the client and
     * its context down directly is safe. */
    mosquitto_destroy (priv->client);
    priv->client = NULL;
    g_clear_pointer (&priv->net_ctx,   g_free);
    g_clear_pointer (&priv->sub_topic, g_free);

out:
    g_free (host);
    g_free (url); g_free (user); g_free (pass); g_free (cid);
    pn_mqtt_conn_opts_clear (&opts);
    /* Visual state stays red until on_mqtt_connect lands a successful
     * CONNACK on the main thread — which is when we actually have a
     * working subscription, not just a TCP socket pending. */
    apply_visual_state (self);
}

/** Debounced (re)connect: coalesce a burst of setter / vault-change calls
 *  into a single restart_client run on the next main-loop idle.  This both
 *  collapses the ~N connect cycles a document load would otherwise trigger
 *  (one per credential setter) and defers the very first connect until after
 *  construction / deserialization — and after a subclass has finished its
 *  own init — is complete (review C2). */
static gboolean
restart_client_idle (gpointer data)
{
    PnMqtt        *self = PN_MQTT (data);
    PnMqttPrivate *priv = PRIV (self);

    priv->restart_idle_id = 0;
    restart_client (self);
    return G_SOURCE_REMOVE;
}

static void
schedule_restart (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);

    if (priv->restart_idle_id == 0)
        priv->restart_idle_id = g_idle_add (restart_client_idle, self);
}

/* ------------------------------------------------------------------ */
/*  Legacy-credentials migration                                       */
/* ------------------------------------------------------------------ */

/** One-shot, post-load migration: if this node still carries inline
 *  credentials and references no profile, move them into a vault profile and
 *  point broker-profile at it.  Scheduled from constructed() so it runs after
 *  the file loader has applied the node's properties; a freshly-dragged node
 *  has no inline secret, so the migration is a no-op for it. */
static gboolean
migrate_legacy_credentials (gpointer data)
{
    PnMqtt        *self     = PN_MQTT (data);
    PnMqttPrivate *priv     = PRIV (self);
    const gchar   *names[]  = { "url", "username", "password", "client-id" };
    const gchar   *values[4];
    gchar         *id;

    priv->migrate_idle_id = 0;

    /* "Custom settings" mode: keep the inline fields as the connection config. */
    if (g_strcmp0 (priv->broker_profile, PN_PROFILE_REF_CUSTOM) == 0)
        return G_SOURCE_REMOVE;

    /* A real profile is selected: inline data is stale (the invariant is that it
     * only exists in Custom mode).  Clear it so the saved file and the dialog
     * widgets are empty/default. */
    if (priv->broker_profile != NULL && *priv->broker_profile != '\0')
    {
        g_object_set (self, "url", "", "username", "", "password", "",
                      "client-id", "", NULL);
        return G_SOURCE_REMOVE;
    }

    /* broker-profile == "" (Default).  A new-model Default node has empty inline
     * fields (cleared when Default was picked), so the branches below only fire
     * for genuinely legacy files that predate broker-profile. */
    if ((priv->username == NULL || *priv->username == '\0') &&
        (priv->password == NULL || *priv->password == '\0'))
    {
        /* No inline credentials to migrate into a profile.  A legacy node with a
         * typed inline url was effectively using "custom settings": mark it so it
         * keeps connecting to that url under the new model. */
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
        /* Repoints the live connection at the profile; the inline secret is
         * now redundant and, being tagged secret, will not be re-serialized. */
        g_object_set (self, "broker-profile", id, NULL);
        g_free (id);
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_mqtt_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

    switch (prop_id)
    {
    case PROP_BROKER_PROFILE:
        g_value_set_string (value, priv->broker_profile);
        break;
    case PROP_URL:
        g_value_set_string (value, priv->url);
        break;
    case PROP_TOPIC:
        g_value_set_string (value, priv->topic);
        break;
    case PROP_USERNAME:
        g_value_set_string (value, priv->username);
        break;
    case PROP_PASSWORD:
        g_value_set_string (value, priv->password);
        break;
    case PROP_CLIENT_ID:
        g_value_set_string (value, priv->client_id);
        break;
    case PROP_QOS:
        g_value_set_uint (value, priv->qos);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_mqtt_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

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
        g_free (priv->topic);
        priv->topic = g_value_dup_string (value);
        schedule_restart (self);
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
        schedule_restart (self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* The referenced profile (or the primary it follows) changed in the vault —
 * re-resolve and reconnect so a manager edit takes effect immediately. */
static void
on_vault_changed (gpointer self)
{
    schedule_restart (PN_MQTT (self));
}

static void
pn_mqtt_constructed (GObject *object)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

    G_OBJECT_CLASS (pn_mqtt_parent_class)->constructed (object);

    /* See migrate_legacy_credentials(): deferred so it runs after the loader
     * applies properties.  It is scheduled before the connect idle below so
     * any broker-profile change it makes is folded into that same restart. */
    priv->migrate_idle_id = g_idle_add (migrate_legacy_credentials, self);

    /* Initial connect, debounced onto an idle so it runs after the loader has
     * applied every property (and after a subclass has finished init), rather
     * than synchronously from _init() before either is done (review C2). */
    schedule_restart (self);

    /* Reconnect whenever the vault changes (auto-disconnected on finalize). */
    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
}

static void
pn_mqtt_dispose (GObject *object)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

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
    /* stop_client joins libmosquitto's network thread first, so no new
     * messages can land in the pending queue past this point.  Then
     * cancel the drain idle and free anything still queued — dropping
     * buffered traffic at shutdown is the right call: nobody is left to
     * see the messages, and processing them would only delay exit. */
    stop_client (self);

    {
        PnMessage *msg;
        g_mutex_lock (&priv->pending_lock);
        /* The network thread is joined, so flush_scheduled can no longer be
         * mid-update: if it is set, flush_idle_id is the real pending id. */
        if (priv->flush_idle_id != 0)
        {
            g_source_remove (priv->flush_idle_id);
            priv->flush_idle_id = 0;
        }
        priv->flush_scheduled = FALSE;
        while ((msg = g_queue_pop_head (&priv->pending)) != NULL)
            g_object_unref (msg);
        g_mutex_unlock (&priv->pending_lock);
    }

    G_OBJECT_CLASS (pn_mqtt_parent_class)->dispose (object);
}

static void
pn_mqtt_finalize (GObject *object)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

    g_clear_pointer (&priv->broker_profile, g_free);
    g_clear_pointer (&priv->url,       g_free);
    g_clear_pointer (&priv->topic,     g_free);
    g_clear_pointer (&priv->username,  g_free);
    g_clear_pointer (&priv->password,  g_free);
    g_clear_pointer (&priv->client_id, g_free);

    /* Belt and suspenders: dispose's stop_client already cleared these. */
    g_clear_pointer (&priv->net_ctx,   g_free);
    g_clear_pointer (&priv->sub_topic, g_free);

    g_mutex_clear (&priv->pending_lock);

    G_OBJECT_CLASS (pn_mqtt_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Default vfunc implementations                                      */
/* ------------------------------------------------------------------ */

gboolean
pn_mqtt_real_accept_topic (PnMqtt *self, const gchar *topic)
{
    (void) self;
    (void) topic;
    return TRUE;
}

gboolean
pn_mqtt_real_process_message (PnMqtt *self, PnMessage *message)
{
    (void) self;
    (void) message;
    return TRUE;
}

static void
pn_mqtt_class_init (PnMqttClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_mqtt_get_property;
    object_class->set_property = pn_mqtt_set_property;
    object_class->constructed  = pn_mqtt_constructed;
    object_class->dispose      = pn_mqtt_dispose;
    object_class->finalize     = pn_mqtt_finalize;

    klass->accept_topic    = pn_mqtt_real_accept_topic;
    klass->process_message = pn_mqtt_real_process_message;

    node_class->palette_icon   = PN_MQTT_NORMAL_ICON;
    node_class->class_name     = "MQTT Source";
    node_class->icon           = PN_MQTT_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_BROKER_PROFILE] = g_param_spec_string (
            "broker-profile", "Broker profile",
            "Id of the mqtt-broker credential profile this node connects "
            "with.  Empty follows the primary mqtt-broker profile, so a node "
            "needs no per-node setup to reach the default broker; pick another "
            "to target a different server, or \"Custom settings\" to use this "
            "node's own inline url/credentials instead.  Profile URLs and "
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
            PN_MQTT_DEFAULT_URL,
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
            "generate a random one for us — fine for most cases.  Pin "
            "a fixed id when the broker enforces an ACL by client "
            "id.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Not profile-related: the subscribe topic + QoS sit together at the
     * end of the dialog, after the broker/credential fields. */
    props[PROP_TOPIC] = g_param_spec_string (
            "subscribe-topic", "Subscribe topic",
            "MQTT topic filter to subscribe to.  Honours the standard "
            "MQTT wildcards: + matches a single level, # matches the "
            "remainder of the topic.  Defaults to \"#\" so the node "
            "produces output as soon as it connects.",
            PN_MQTT_DEFAULT_TOPIC,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_QOS] = g_param_spec_uint (
            "qos", "Subscribe QoS",
            "MQTT QoS to request for the subscription (0 = at most "
            "once, 1 = at least once, 2 = exactly once).  The broker "
            "may downgrade.",
            0u, 2u, 0u,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_mqtt_init (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);
    PnNode        *node = PN_NODE (self);

    priv->broker_profile  = g_strdup ("");
    priv->url             = g_strdup (PN_MQTT_DEFAULT_URL);
    priv->topic           = g_strdup (PN_MQTT_DEFAULT_TOPIC);
    priv->username        = NULL;
    priv->password        = NULL;
    priv->client_id       = NULL;
    priv->qos             = 0;
    priv->migrate_idle_id = 0;
    priv->restart_idle_id = 0;
    priv->client          = NULL;
    priv->loop_running    = FALSE;
    priv->connected       = FALSE;
    priv->connecting      = FALSE;
    priv->net_ctx         = NULL;
    priv->generation      = 0;
    priv->sub_topic       = NULL;
    priv->sub_qos         = 0;

    g_mutex_init (&priv->pending_lock);
    g_queue_init (&priv->pending);
    priv->flush_idle_id   = 0;
    priv->flush_scheduled = FALSE;

    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* The initial connect is scheduled from constructed() (review C2), not
     * here — see pn_mqtt_constructed().  Doing it in _init() would start the
     * network thread before deserialization and any subclass init complete. */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMqtt *
pn_mqtt_new (void)
{
    return g_object_new (PN_TYPE_MQTT, NULL);
}
