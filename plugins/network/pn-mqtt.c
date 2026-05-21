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

#include <json-glib/json-glib.h>
#include <mosquitto.h>

#include <errno.h>
#include <string.h>

/* Visual states.  Body colour carries the alert; the icon panel is
 * rendered in white on top. */
#define PN_MQTT_NORMAL_ICON  "\xef\x82\x9e"  /* fa-rss U+F09E */
#define PN_MQTT_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* Default URL + topic.  The URL points at the user's homelab MQTT
 * broker by convention (per TODO #18); plain TCP on the standard
 * 1883 port.  The topic filter "#" matches every topic on the broker
 * so a freshly dropped node already produces output without any
 * configuration. */
#define PN_MQTT_DEFAULT_URL   "tcp://mqtt.homelab.local:1883"
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

struct _PnMqtt
{
    PnNode parent_instance;

    /* Configuration.  All accessed and written from the main thread
     * exclusively (property setters land there), so no mutex is
     * needed — libmosquitto's network thread reads them only via
     * pointers that we hand to it before mosquitto_loop_start() and
     * never mutate while a session is up. */
    gchar  *url;
    gchar  *topic;
    gchar  *username;
    gchar  *password;
    gchar  *client_id;
    guint   qos;

    /* Live mosquitto client + a flag tracking whether its background
     * loop thread is running.  Both are NULL / FALSE while the node
     * sits in its disconnected state. */
    struct mosquitto *client;
    gboolean          loop_running;

    /* Sticky "are we currently connected" flag, flipped by the
     * on_connect / on_disconnect callbacks (marshalled to the main
     * thread).  Used to drive the visual state. */
    gboolean          connected;
};

G_DEFINE_TYPE (PnMqtt, pn_mqtt, PN_TYPE_NODE)

enum {
    PROP_0,
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
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the connected (green RSS) and the
 *  unconfigured / disconnected (red ❗) appearance based on whether
 *  @self->connected is set and the URL is non-empty. */
static void
apply_visual_state (PnMqtt *self)
{
    PnNode  *node = PN_NODE (self);
    gboolean ok   = self->connected
                 && self->url != NULL && *self->url != '\0';

    if (ok)
    {
        GdkRGBA green = { 0.36, 0.66, 0.36, 1.0 };
        pn_node_set_color (node, &green);
        pn_node_set_icon  (node, PN_MQTT_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
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

typedef struct
{
    PnNode    *node;     /* strong ref */
    PnMessage *message;  /* strong ref */
} PnMqttEmitClosure;

static gboolean
emit_on_main_trampoline (gpointer data)
{
    PnMqttEmitClosure *c = data;
    pn_node_emit_message (c->node, c->message);
    return G_SOURCE_REMOVE;
}

static void
emit_closure_free (gpointer data)
{
    PnMqttEmitClosure *c = data;
    g_object_unref (c->node);
    g_object_unref (c->message);
    g_free (c);
}

static void
emit_message_on_main (PnMqtt *self, PnMessage *message)
{
    PnMqttEmitClosure *c = g_new0 (PnMqttEmitClosure, 1);
    c->node    = g_object_ref (PN_NODE (self));
    c->message = message;  /* transfer */
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                emit_on_main_trampoline,
                                c,
                                emit_closure_free);
}

typedef struct
{
    PnMqtt   *self;        /* strong ref */
    gboolean  connected;
} PnMqttConnStateClosure;

static gboolean
conn_state_trampoline (gpointer data)
{
    PnMqttConnStateClosure *c = data;
    /* Only honour the update if the node has not torn down its
     * client in the meantime — a stale on_disconnect from an old
     * session must not flip a freshly reconnected one back to red. */
    if (c->self->client != NULL)
    {
        c->self->connected = c->connected;
        apply_visual_state (c->self);
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
    PnMqtt *self = PN_MQTT (obj);

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
    if (self->topic != NULL && *self->topic != '\0')
    {
        int err = mosquitto_subscribe (mosq, NULL, self->topic, (int) self->qos);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt: subscribe('%s') failed: %s",
                       self->topic, mosquitto_strerror (err));
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
 *  "mqtt/<mqtt-topic>" so a downstream Filter can route on the
 *  envelope alone (e.g. match `mqtt/tele/+/SENSOR`) without peeking
 *  into the data bag; the data bag follows the standard contract:
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
    gchar       *envelope_topic;
    gchar       *payload = NULL;
    gboolean     is_utf8 = FALSE;
    gboolean     is_json = FALSE;
    JsonNode    *json_root = NULL;
    JsonParser  *parser  = NULL;
    gchar       *output;
    const gchar *topic_str = m->topic ? m->topic : "";

    /* Compose the envelope topic as "mqtt/<mqtt-topic>" so a Filter
     * downstream can route on `topic == "mqtt/tele/sonoff37/SENSOR"`
     * (or a wildcard match against the family) without having to dig
     * into the data bag. */
    envelope_topic = g_strconcat ("mqtt/", topic_str, NULL);
    msg = pn_message_new (PN_NODE (self), envelope_topic);
    g_free (envelope_topic);

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
    PnMqtt    *self = PN_MQTT (obj);
    PnMessage *msg;

    (void) mosq;

    if (m == NULL)
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
    if (self->client == NULL)
        return;

    /* Order matters: ask for a clean MQTT-level disconnect first so
     * the broker sees a DISCONNECT rather than a TCP RST, then stop
     * the loop thread (force=TRUE because the loop may be in a
     * read() that won't return on its own), then destroy the client.
     * mosquitto_loop_stop blocks until the network thread has joined
     * so the callbacks cannot fire after this returns. */
    mosquitto_disconnect (self->client);
    if (self->loop_running)
    {
        mosquitto_loop_stop (self->client, TRUE);
        self->loop_running = FALSE;
    }
    mosquitto_destroy (self->client);
    self->client    = NULL;
    self->connected = FALSE;
}

/** Tear down any running client and start a fresh one with the
 *  current configuration.  Called from every property setter. */
static void
restart_client (PnMqtt *self)
{
    gchar    *host = NULL;
    int       port = 0;
    gboolean  tls  = FALSE;
    int       err;

    stop_client (self);

    if (self->url == NULL || *self->url == '\0')
    {
        apply_visual_state (self);
        return;
    }

    if (!parse_mqtt_url (self->url, &host, &port, &tls))
    {
        g_warning ("pn-mqtt: invalid URL '%s'", self->url);
        apply_visual_state (self);
        return;
    }

    ensure_mosquitto_initialised ();

    /* clean_session = TRUE: we do not persist subscriptions across
     * disconnects (we always re-subscribe in on_connect anyway), so
     * the broker need not remember us between sessions.  NULL
     * client_id lets libmosquitto generate a random one for us when
     * the user has not pinned a fixed id. */
    self->client = mosquitto_new (
            (self->client_id != NULL && *self->client_id != '\0')
                ? self->client_id : NULL,
            TRUE,
            self);
    if (self->client == NULL)
    {
        g_warning ("pn-mqtt: mosquitto_new failed: %s", g_strerror (errno));
        g_free (host);
        apply_visual_state (self);
        return;
    }

    mosquitto_connect_callback_set    (self->client, on_mqtt_connect);
    mosquitto_disconnect_callback_set (self->client, on_mqtt_disconnect);
    mosquitto_message_callback_set    (self->client, on_mqtt_message);

    mosquitto_reconnect_delay_set (self->client,
            PN_MQTT_RECONNECT_DELAY_MIN,
            PN_MQTT_RECONNECT_DELAY_MAX,
            TRUE);

    if (self->username != NULL && *self->username != '\0')
    {
        err = mosquitto_username_pw_set (self->client,
                self->username,
                (self->password != NULL && *self->password != '\0')
                    ? self->password : NULL);
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
        err = mosquitto_tls_set (self->client, NULL, NULL, NULL, NULL, NULL);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt: tls_set failed: %s",
                       mosquitto_strerror (err));
    }

    err = mosquitto_connect_async (self->client, host, port,
                                   PN_MQTT_KEEPALIVE_SECONDS);
    if (err != MOSQ_ERR_SUCCESS)
    {
        g_warning ("pn-mqtt: connect_async(%s:%d) failed: %s",
                   host, port, mosquitto_strerror (err));
        mosquitto_destroy (self->client);
        self->client = NULL;
        g_free (host);
        apply_visual_state (self);
        return;
    }

    err = mosquitto_loop_start (self->client);
    if (err != MOSQ_ERR_SUCCESS)
    {
        g_warning ("pn-mqtt: loop_start failed: %s",
                   mosquitto_strerror (err));
        mosquitto_destroy (self->client);
        self->client = NULL;
        g_free (host);
        apply_visual_state (self);
        return;
    }
    self->loop_running = TRUE;

    g_free (host);
    /* Visual state stays red until on_mqtt_connect lands a successful
     * CONNACK on the main thread — which is when we actually have a
     * working subscription, not just a TCP socket pending. */
    apply_visual_state (self);
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
    PnMqtt *self = PN_MQTT (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_value_set_string (value, self->url);
        break;
    case PROP_TOPIC:
        g_value_set_string (value, self->topic);
        break;
    case PROP_USERNAME:
        g_value_set_string (value, self->username);
        break;
    case PROP_PASSWORD:
        g_value_set_string (value, self->password);
        break;
    case PROP_CLIENT_ID:
        g_value_set_string (value, self->client_id);
        break;
    case PROP_QOS:
        g_value_set_uint (value, self->qos);
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
    PnMqtt *self = PN_MQTT (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_free (self->url);
        self->url = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_TOPIC:
        g_free (self->topic);
        self->topic = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_USERNAME:
        g_free (self->username);
        self->username = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_PASSWORD:
        g_free (self->password);
        self->password = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_CLIENT_ID:
        g_free (self->client_id);
        self->client_id = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_QOS:
        self->qos = g_value_get_uint (value);
        restart_client (self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_mqtt_dispose (GObject *object)
{
    PnMqtt *self = PN_MQTT (object);

    stop_client (self);

    G_OBJECT_CLASS (pn_mqtt_parent_class)->dispose (object);
}

static void
pn_mqtt_finalize (GObject *object)
{
    PnMqtt *self = PN_MQTT (object);

    g_clear_pointer (&self->url,       g_free);
    g_clear_pointer (&self->topic,     g_free);
    g_clear_pointer (&self->username,  g_free);
    g_clear_pointer (&self->password,  g_free);
    g_clear_pointer (&self->client_id, g_free);

    G_OBJECT_CLASS (pn_mqtt_parent_class)->finalize (object);
}

static void
pn_mqtt_class_init (PnMqttClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_mqtt_get_property;
    object_class->set_property = pn_mqtt_set_property;
    object_class->dispose      = pn_mqtt_dispose;
    object_class->finalize     = pn_mqtt_finalize;

    node_class->palette_icon   = PN_MQTT_NORMAL_ICON;
    node_class->class_name     = "MQTT Source";
    node_class->icon           = PN_MQTT_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_URL] = g_param_spec_string (
            "url", "Broker URL",
            "MQTT broker URL.  Accepts tcp://host[:port] (plain MQTT, "
            "default port 1883), ssl://host[:port] or "
            "mqtts://host[:port] (MQTT over TLS, default port 8883), "
            "or a bare host[:port] which is treated as plain TCP.  "
            "Defaults to tcp://mqtt.homelab.local:1883.",
            PN_MQTT_DEFAULT_URL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TOPIC] = g_param_spec_string (
            "topic", "Subscribe topic",
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
            "MQTT password; only sent when username is also set.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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
    PnNode *node = PN_NODE (self);

    self->url          = g_strdup (PN_MQTT_DEFAULT_URL);
    self->topic        = g_strdup (PN_MQTT_DEFAULT_TOPIC);
    self->username     = NULL;
    self->password     = NULL;
    self->client_id    = NULL;
    self->qos          = 0;
    self->client       = NULL;
    self->loop_running = FALSE;
    self->connected    = FALSE;

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
