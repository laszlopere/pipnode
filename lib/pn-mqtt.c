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

#include <json-glib/json-glib.h>
#include <mosquitto.h>

#include <errno.h>
#include <string.h>

/* Visual states.  Body colour carries the alert; the icon panel is
 * rendered in white on top. */
#define PN_MQTT_NORMAL_ICON  "\xef\x82\x9e"  /* fa-rss U+F09E */
#define PN_MQTT_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* Default topic.  "#" matches every topic on the broker, so a node pointed
 * at a broker produces output without further configuration.  The broker URL
 * has NO built-in default: a site-specific address does not belong in the
 * plugin source, so it comes from the referenced mqtt-broker vault profile
 * (the host-managed local setting) or, for legacy files, the inline url —
 * empty until configured, which shows the node in its unconfigured state. */
#define PN_MQTT_DEFAULT_URL   ""
#define PN_MQTT_DEFAULT_TOPIC "#"

/* MQTT keep-alive (seconds).  The broker disconnects a client that
 * goes silent for 1.5x this — 60 s gives a brisk dead-peer detection
 * window without flooding the network with pings on idle sessions. */
#define PN_MQTT_KEEPALIVE_SECONDS 60

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

    /* Live mosquitto client + a flag tracking whether its background
     * loop thread is running.  Both are NULL / FALSE while the node
     * sits in its disconnected state. */
    struct mosquitto *client;
    gboolean          loop_running;

    /* Sticky "are we currently connected" flag, flipped by the
     * on_connect / on_disconnect callbacks (marshalled to the main
     * thread).  Used to drive the visual state. */
    gboolean          connected;

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
} PnMqttPrivate;

/* Hard cap on the pending-message queue.  Sized for a healthy retained
 * burst (a few thousand messages) without unbounded growth under a
 * sustained-flood pathological case. */
#define PN_MQTT_PENDING_MAX 10000

G_DEFINE_TYPE_WITH_PRIVATE (PnMqtt, pn_mqtt, PN_TYPE_NODE)

#define PRIV(self) ((PnMqttPrivate *) pn_mqtt_get_instance_private (self))

enum {
    PROP_0,
    PROP_BROKER_PROFILE,
    PROP_URL,
    PROP_TOPIC,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_CLIENT_ID,
    PROP_QOS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void restart_client (PnMqtt *self);

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
                    gchar **out_cid)
{
    PnMqttPrivate *priv = PRIV (self);
    PnProfile     *p    = pn_node_get_profile (PN_NODE (self), "broker-profile");

    if (p != NULL)
    {
        if (out_url)  *out_url  = pn_profile_get_string (p, "url");
        if (out_user) *out_user = pn_profile_get_string (p, "username");
        if (out_pass) *out_pass = pn_profile_get_secret (p, "password");
        if (out_cid)  *out_cid  = pn_profile_get_string (p, "client-id");
    }
    else
    {
        if (out_url)  *out_url  = g_strdup (priv->url       ? priv->url       : "");
        if (out_user) *out_user = g_strdup (priv->username  ? priv->username  : "");
        if (out_pass) *out_pass = g_strdup (priv->password  ? priv->password  : "");
        if (out_cid)  *out_cid  = g_strdup (priv->client_id ? priv->client_id : "");
    }
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the connected (green RSS) and the
 *  unconfigured / disconnected (red ❗) appearance based on whether
 *  @priv->connected is set and the effective URL is non-empty. */
static void
apply_visual_state (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);
    PnNode        *node = PN_NODE (self);
    gchar         *url  = NULL;
    gboolean       ok;

    resolve_connection (self, &url, NULL, NULL, NULL);
    ok = priv->connected && url != NULL && *url != '\0';
    g_free (url);

    if (ok)
    {
        PnColor green = { 0.36, 0.66, 0.36, 1.0 };
        pn_node_set_color (node, &green);
        pn_node_set_icon  (node, PN_MQTT_NORMAL_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_MQTT_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  URL parsing                                                        */
/*                                                                     */
/*  Accepts tcp://host[:port] for plain MQTT (default port 1883) and   */
/*  ssl://host[:port] or mqtts://host[:port] for MQTT-over-TLS         */
/*  (default port 8883).  A bare host[:port] without a scheme is also  */
/*  accepted and treated as plain TCP.  Returns TRUE on success and    */
/*  fills *out_host (caller frees), *out_port and *out_tls; returns    */
/*  FALSE without modifying outputs on a malformed URL.                */
/* ------------------------------------------------------------------ */

static gboolean
parse_mqtt_url (
        const gchar *url,
        gchar      **out_host,
        int         *out_port,
        gboolean    *out_tls)
{
    const gchar *p = url;
    const gchar *colon;
    gboolean     tls  = FALSE;
    int          port;
    gchar       *host;

    if (url == NULL || *url == '\0')
        return FALSE;

    if (g_str_has_prefix (p, "tcp://"))
    {
        p   += 6;
        tls  = FALSE;
    }
    else if (g_str_has_prefix (p, "ssl://"))
    {
        p   += 6;
        tls  = TRUE;
    }
    else if (g_str_has_prefix (p, "mqtt://"))
    {
        p   += 7;
        tls  = FALSE;
    }
    else if (g_str_has_prefix (p, "mqtts://"))
    {
        p   += 8;
        tls  = TRUE;
    }
    /* Otherwise treat as bare host[:port] — be liberal in what we
     * accept so a user typing just "mqtt.local:1883" into the URL
     * field still works without learning the scheme syntax. */

    /* Reject an empty host part (e.g. just "tcp://"). */
    if (*p == '\0' || *p == ':' || *p == '/')
        return FALSE;

    port  = tls ? 8883 : 1883;
    colon = strchr (p, ':');
    if (colon != NULL)
    {
        gchar  *endp;
        glong   parsed;

        host = g_strndup (p, (gsize) (colon - p));
        errno = 0;
        parsed = strtol (colon + 1, &endp, 10);
        if (errno != 0 || endp == colon + 1 || parsed <= 0 || parsed > 65535)
        {
            g_free (host);
            return FALSE;
        }
        port = (int) parsed;
    }
    else
    {
        host = g_strdup (p);
    }

    *out_host = host;
    *out_port = port;
    *out_tls  = tls;
    return TRUE;
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
    priv->flush_idle_id = 0;
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
    PnMqttPrivate *priv          = PRIV (self);
    PnMessage     *dropped       = NULL;
    gboolean       need_schedule = FALSE;

    g_mutex_lock (&priv->pending_lock);
    if (priv->pending.length >= PN_MQTT_PENDING_MAX)
        dropped = g_queue_pop_head (&priv->pending);
    g_queue_push_tail (&priv->pending, message);  /* transfer */
    if (priv->flush_idle_id == 0)
    {
        /* g_idle_add is thread-safe: it locks the default GMainContext
         * internally before scheduling.  We store the source id under
         * the same mutex so flush_pending_on_main can clear it
         * atomically with the queue swap. */
        priv->flush_idle_id = g_idle_add (flush_pending_on_main, self);
        need_schedule = TRUE;
    }
    g_mutex_unlock (&priv->pending_lock);

    if (dropped != NULL)
        g_object_unref (dropped);
    (void) need_schedule;
}

typedef struct
{
    PnMqtt   *self;        /* strong ref */
    gboolean  connected;
} PnMqttConnStateClosure;

static gboolean
conn_state_trampoline (gpointer data)
{
    PnMqttConnStateClosure *c    = data;
    PnMqtt                 *self = c->self;
    PnMqttPrivate          *priv = PRIV (self);

    /* Only honour the update if the node has not torn down its
     * client in the meantime — a stale on_disconnect from an old
     * session must not flip a freshly reconnected one back to red. */
    if (priv->client != NULL)
    {
        priv->connected = c->connected;
        apply_visual_state (self);
    }
    return G_SOURCE_REMOVE;
}

static void
conn_state_closure_free (gpointer data)
{
    PnMqttConnStateClosure *c = data;
    g_object_unref (c->self);
    g_free (c);
}

static void
post_conn_state_on_main (PnMqtt *self, gboolean connected)
{
    PnMqttConnStateClosure *c = g_new0 (PnMqttConnStateClosure, 1);
    c->self      = g_object_ref (self);
    c->connected = connected;
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                conn_state_trampoline,
                                c,
                                conn_state_closure_free);
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
    PnMqtt        *self = PN_MQTT (obj);
    PnMqttPrivate *priv = PRIV (self);

    if (rc != 0)
    {
        g_warning ("pn-mqtt: connect refused: %s",
                   mosquitto_connack_string (rc));
        post_conn_state_on_main (self, FALSE);
        return;
    }

    /* Re-subscribe on every (re)connect: the broker forgets our
     * subscriptions when the session goes down (we asked for clean
     * sessions in mosquitto_new), so each fresh CONNACK has to be
     * paired with a fresh SUBSCRIBE. */
    if (priv->topic != NULL && *priv->topic != '\0')
    {
        int err = mosquitto_subscribe (mosq, NULL, priv->topic, (int) priv->qos);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt: subscribe('%s') failed: %s",
                       priv->topic, mosquitto_strerror (err));
    }

    post_conn_state_on_main (self, TRUE);
}

static void
on_mqtt_disconnect (
        struct mosquitto *mosq,
        void             *obj,
        int               rc)
{
    PnMqtt *self = PN_MQTT (obj);

    (void) mosq;

    if (rc != 0)
    {
        /* Unexpected disconnect — libmosquitto's threaded loop will
         * try to reconnect on its own using the configured back-off,
         * so we just flip the visual state and let it work. */
        g_message ("pn-mqtt: disconnected (%s); reconnecting…",
                   mosquitto_strerror (rc));
    }

    post_conn_state_on_main (self, FALSE);
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
    PnMessage   *msg;
    gchar       *payload = NULL;
    gboolean     is_utf8 = FALSE;
    gboolean     is_json = FALSE;
    JsonNode    *json_root = NULL;
    JsonParser  *parser  = NULL;
    gchar       *output;
    const gchar *topic_str = m->topic ? m->topic : "";

    /* Envelope topic is the MQTT topic itself so what the Debug pane
     * shows matches what the broker published, and a downstream Filter
     * routes on the same string the user typed at the broker. */
    msg = pn_message_new (PN_NODE (self), topic_str);

    /* mosquitto_message.payload is not NUL-terminated; copy it out
     * and decide whether the bytes are valid UTF-8 before exposing
     * them as a string.  Binary payloads land as "". */
    if (m->payloadlen > 0 && m->payload != NULL)
    {
        gchar *raw = g_strndup ((const gchar *) m->payload,
                                (gsize) m->payloadlen);
        if (g_utf8_validate (raw, (gssize) m->payloadlen, NULL))
        {
            payload = raw;
            is_utf8 = TRUE;
        }
        else
        {
            g_free (raw);
            payload = g_strdup ("");
        }
    }
    else
    {
        payload = g_strdup ("");
    }

    pn_message_set_int     (msg, "qos",      m->qos);
    pn_message_set_boolean (msg, "retained", m->retain);

    /* Try to parse the payload as JSON so a Tasmota-style
     * `{"Time":...,"AM2301":{...}}` lands on `data.payload` as a
     * proper nested object the downstream Filter / Format nodes can
     * pluck values out of with a `payload.AM2301.Temperature` path
     * instead of having to peel a string-escaped JSON blob first. */
    if (is_utf8 && payload[0] != '\0')
    {
        parser = json_parser_new ();
        if (json_parser_load_from_data (parser, payload,
                                        (gssize) m->payloadlen, NULL))
        {
            json_root = json_parser_get_root (parser);
            if (json_root != NULL)
                is_json = TRUE;
        }
    }

    if (is_json)
    {
        /* json_node_copy detaches the root from the parser so we can
         * destroy the parser right after; pn_message_set_member
         * takes ownership of the copy. */
        pn_message_set_member (msg, "payload", json_node_copy (json_root));

        /* Bare JSON number → mirror onto data.value so a Graph just
         * works without an extra Format step. */
        if (JSON_NODE_HOLDS_VALUE (json_root) &&
            (json_node_get_value_type (json_root) == G_TYPE_DOUBLE ||
             json_node_get_value_type (json_root) == G_TYPE_INT64))
        {
            pn_message_set_double (msg, "value",
                                   json_node_get_double (json_root));
        }
    }
    else
    {
        pn_message_set_string (msg, "payload", payload);

        /* Non-JSON best-effort numeric coercion: a bare `23.5` from a
         * sensor that sends raw text rather than JSON still becomes
         * data.value.  (JSON-number payloads already took the branch
         * above.) */
        if (is_utf8 && payload[0] != '\0')
        {
            gchar  *endptr;
            gdouble v;
            errno = 0;
            v = g_ascii_strtod (payload, &endptr);
            if (errno == 0 && endptr != payload && *endptr == '\0')
                pn_message_set_double (msg, "value", v);
        }
    }

    /* Short, human-readable summary suitable for the Debug pane's
     * one-liner mode.  We deliberately do NOT inline the payload —
     * it can be megabytes (camera image base64, firmware blobs,
     * batched telemetry) and dumping it into the log gets unreadable
     * fast.  A reader who wants the payload can drop the Debug node
     * into JSON mode or wire a Format template. */
    output = g_strdup_printf ("MQTT message on %s",
                              *topic_str ? topic_str : "(no topic)");
    pn_message_set_string  (msg, "output",  output);
    pn_message_set_boolean (msg, "success", TRUE);

    g_free (output);
    g_free (payload);
    g_clear_object (&parser);
    return msg;
}

static void
on_mqtt_message (
        struct mosquitto                 *mosq,
        void                             *obj,
        const struct mosquitto_message   *m)
{
    PnMqtt      *self  = PN_MQTT (obj);
    PnMqttClass *klass = PN_MQTT_GET_CLASS (self);
    PnMessage   *msg;

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

    if (priv->client == NULL)
        return;

    /* Order matters: ask for a clean MQTT-level disconnect first so
     * the broker sees a DISCONNECT rather than a TCP RST, then stop
     * the loop thread (force=TRUE because the loop may be in a
     * read() that won't return on its own), then destroy the client.
     * mosquitto_loop_stop blocks until the network thread has joined
     * so the callbacks cannot fire after this returns. */
    mosquitto_disconnect (priv->client);
    if (priv->loop_running)
    {
        mosquitto_loop_stop (priv->client, TRUE);
        priv->loop_running = FALSE;
    }
    mosquitto_destroy (priv->client);
    priv->client    = NULL;
    priv->connected = FALSE;
}

/** Tear down any running client and start a fresh one with the
 *  current configuration.  Called from every property setter and after the
 *  legacy-credentials migration changes the referenced profile. */
static void
restart_client (PnMqtt *self)
{
    PnMqttPrivate *priv = PRIV (self);
    gchar         *host = NULL;
    int            port = 0;
    gboolean       tls  = FALSE;
    int            err;
    gchar         *url  = NULL;
    gchar         *user = NULL;
    gchar         *pass = NULL;
    gchar         *cid  = NULL;

    stop_client (self);

    resolve_connection (self, &url, &user, &pass, &cid);

    if (url == NULL || *url == '\0')
    {
        g_free (url); g_free (user); g_free (pass); g_free (cid);
        apply_visual_state (self);
        return;
    }

    if (!parse_mqtt_url (url, &host, &port, &tls))
    {
        g_warning ("pn-mqtt: invalid URL '%s'", url);
        g_free (url); g_free (user); g_free (pass); g_free (cid);
        apply_visual_state (self);
        return;
    }

    ensure_mosquitto_initialised ();

    /* clean_session = TRUE: we do not persist subscriptions across
     * disconnects (we always re-subscribe in on_connect anyway), so
     * the broker need not remember us between sessions.  NULL
     * client_id lets libmosquitto generate a random one for us when
     * the user has not pinned a fixed id. */
    priv->client = mosquitto_new ((cid != NULL && *cid != '\0') ? cid : NULL,
                                  TRUE,
                                  self);
    if (priv->client == NULL)
    {
        g_warning ("pn-mqtt: mosquitto_new failed: %s", g_strerror (errno));
        g_free (host);
        g_free (url); g_free (user); g_free (pass); g_free (cid);
        apply_visual_state (self);
        return;
    }

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
            g_warning ("pn-mqtt: username_pw_set failed: %s",
                       mosquitto_strerror (err));
    }

    if (tls)
    {
        /* No CA file / cert pinning yet — let the system trust store
         * verify the broker's certificate.  Users with a private CA
         * can supply the cert through the OS trust store; richer TLS
         * knobs (CAfile, mTLS) can land later as additional
         * properties without breaking this path. */
        err = mosquitto_tls_set (priv->client, NULL, NULL, NULL, NULL, NULL);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt: tls_set failed: %s",
                       mosquitto_strerror (err));
    }

    err = mosquitto_connect_async (priv->client, host, port,
                                   PN_MQTT_KEEPALIVE_SECONDS);
    if (err != MOSQ_ERR_SUCCESS)
    {
        g_warning ("pn-mqtt: connect_async(%s:%d) failed: %s",
                   host, port, mosquitto_strerror (err));
        mosquitto_destroy (priv->client);
        priv->client = NULL;
        g_free (host);
        g_free (url); g_free (user); g_free (pass); g_free (cid);
        apply_visual_state (self);
        return;
    }

    err = mosquitto_loop_start (priv->client);
    if (err != MOSQ_ERR_SUCCESS)
    {
        g_warning ("pn-mqtt: loop_start failed: %s",
                   mosquitto_strerror (err));
        mosquitto_destroy (priv->client);
        priv->client = NULL;
        g_free (host);
        g_free (url); g_free (user); g_free (pass); g_free (cid);
        apply_visual_state (self);
        return;
    }
    priv->loop_running = TRUE;

    g_free (host);
    g_free (url); g_free (user); g_free (pass); g_free (cid);
    /* Visual state stays red until on_mqtt_connect lands a successful
     * CONNACK on the main thread — which is when we actually have a
     * working subscription, not just a TCP socket pending. */
    apply_visual_state (self);
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

    if (priv->broker_profile != NULL && *priv->broker_profile != '\0')
        return G_SOURCE_REMOVE;
    if ((priv->username == NULL || *priv->username == '\0') &&
        (priv->password == NULL || *priv->password == '\0'))
        return G_SOURCE_REMOVE;

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
        restart_client (self);
        break;
    case PROP_URL:
        g_free (priv->url);
        priv->url = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_TOPIC:
        g_free (priv->topic);
        priv->topic = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_USERNAME:
        g_free (priv->username);
        priv->username = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_PASSWORD:
        g_free (priv->password);
        priv->password = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_CLIENT_ID:
        g_free (priv->client_id);
        priv->client_id = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_QOS:
        priv->qos = g_value_get_uint (value);
        restart_client (self);
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
    restart_client (PN_MQTT (self));
}

static void
pn_mqtt_constructed (GObject *object)
{
    PnMqtt        *self = PN_MQTT (object);
    PnMqttPrivate *priv = PRIV (self);

    G_OBJECT_CLASS (pn_mqtt_parent_class)->constructed (object);

    /* See migrate_legacy_credentials(): deferred so it runs after the loader
     * applies properties. */
    priv->migrate_idle_id = g_idle_add (migrate_legacy_credentials, self);

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
    /* stop_client joins libmosquitto's network thread first, so no new
     * messages can land in the pending queue past this point.  Then
     * cancel the drain idle and free anything still queued — dropping
     * buffered traffic at shutdown is the right call: nobody is left to
     * see the messages, and processing them would only delay exit. */
    stop_client (self);

    {
        PnMessage *msg;
        g_mutex_lock (&priv->pending_lock);
        if (priv->flush_idle_id != 0)
        {
            g_source_remove (priv->flush_idle_id);
            priv->flush_idle_id = 0;
        }
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
            "to target a different server.  The broker URL and credentials "
            "live in the host vault, not in the workflow file.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_profile_ref (props[PROP_BROKER_PROFILE],
                                   PN_PROFILE_TYPE_MQTT_BROKER);

    props[PROP_URL] = g_param_spec_string (
            "url", "Broker URL",
            "MQTT broker URL.  Accepts tcp://host[:port] (plain MQTT, "
            "default port 1883), ssl://host[:port] or "
            "mqtts://host[:port] (MQTT over TLS, default port 8883), "
            "or a bare host[:port] which is treated as plain TCP.  "
            "Legacy inline fallback — empty by default; new configurations "
            "set the broker address in the mqtt-broker vault profile.",
            PN_MQTT_DEFAULT_URL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TOPIC] = g_param_spec_string (
            "subscribe-topic", "Subscribe topic",
            "MQTT topic filter to subscribe to.  Honours the standard "
            "MQTT wildcards: + matches a single level, # matches the "
            "remainder of the topic.  Defaults to \"#\" so the node "
            "produces output as soon as it connects.",
            PN_MQTT_DEFAULT_TOPIC,
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
    priv->client          = NULL;
    priv->loop_running    = FALSE;
    priv->connected       = FALSE;

    g_mutex_init (&priv->pending_lock);
    g_queue_init (&priv->pending);
    priv->flush_idle_id   = 0;

    pn_node_set_class_name (node, "MQTT Source");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Try to connect immediately with the default URL/topic so a
     * freshly dropped node already produces output once the broker
     * answers; the visual flips green from on_mqtt_connect. */
    restart_client (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMqtt *
pn_mqtt_new (void)
{
    return g_object_new (PN_TYPE_MQTT, NULL);
}
