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

#include <json-glib/json-glib.h>
#include <mosquitto.h>

#include <errno.h>
#include <string.h>

/* Visual states.  Same colour split as PnMqtt so the source/sink pair
 * reads as a matched set: green (fa-paper-plane) while a session is up
 * AND the publish topic field is non-empty; red (❗) otherwise. */
#define PN_MQTT_SINK_NORMAL_ICON  "\xef\x87\x98"  /* fa-paper-plane U+F1D8 */
#define PN_MQTT_SINK_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* Default URL points at the same conventional homelab broker the
 * source uses so a paired Source + Sink dropped onto a fresh
 * worksheet talk to the same machine without configuration. */
#define PN_MQTT_SINK_DEFAULT_URL "tcp://mqtt.homelab.local:1883"

/* MQTT keep-alive + reconnect bounds.  Same values as PnMqtt -- this
 * is per-broker rather than per-direction, so the publisher uses the
 * same numbers a subscriber would. */
#define PN_MQTT_SINK_KEEPALIVE_SECONDS    60
#define PN_MQTT_SINK_RECONNECT_DELAY_MIN   1u
#define PN_MQTT_SINK_RECONNECT_DELAY_MAX  30u

struct _PnMqttSink
{
    PnNode parent_instance;

    /* Configuration.  Mutated only on the main thread (property
     * setters), read by both the main thread (receive path) and
     * libmosquitto's network thread (callbacks).  The callbacks only
     * read self->connected (via the conn-state trampoline) so no
     * mutex is required -- the publish path itself happens on the
     * main thread inside receive, before handing bytes to
     * mosquitto_publish which has its own internal locking. */
    gchar  *url;
    gchar  *payload_template; /* "" -> use inbound data.payload      */
    gchar  *username;
    gchar  *password;
    gchar  *client_id;
    guint   qos;
    gboolean retain;

    struct mosquitto *client;
    gboolean          loop_running;
    gboolean          connected;
};

G_DEFINE_TYPE (PnMqttSink, pn_mqtt_sink, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_URL,
    PROP_PAYLOAD,
    PROP_RETAIN,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_CLIENT_ID,
    PROP_QOS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void restart_client (PnMqttSink *self);

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

/** Flip the node body between the connected (green paper-plane) and
 *  the unconfigured / disconnected (red ❗) appearance.  Same rule as
 *  PnMqtt: needs a non-empty URL plus an active broker session.  The
 *  publish topic comes from the inbound message envelope rather than
 *  a per-node property, so per-message routing is the user's concern
 *  upstream (e.g. via PnRewrite) and the node itself has nothing it
 *  could complain about being "unconfigured" beyond the broker URL. */
static void
apply_visual_state (PnMqttSink *self)
{
    PnNode  *node = PN_NODE (self);
    gboolean ok   = self->connected
                 && self->url != NULL && *self->url != '\0';

    if (ok)
    {
        GdkRGBA green = { 0.36, 0.66, 0.36, 1.0 };
        pn_node_set_color (node, &green);
        pn_node_set_icon  (node, PN_MQTT_SINK_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_MQTT_SINK_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  URL parsing                                                        */
/*                                                                     */
/*  Mirror of PnMqtt's parser.  Duplicated rather than lifted into a   */
/*  shared header because the two are the only callers today and the   */
/*  helper is small enough that the cost of keeping the two in sync    */
/*  is lower than the cost of growing a `pn-mqtt-common.h` (the host   */
/*  does not link libmosquitto, so the helper has nowhere natural to   */
/*  live inside libpipnode).  If a third client lands later we will    */
/*  factor this and the ensure_mosquitto_initialised() guard at the    */
/*  same time.                                                          */
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
/*  Template expansion                                                 */
/*                                                                     */
/*  Same `${path/to/field}` syntax PnFormat / PnRewrite already use,   */
/*  so a user who learned the placeholder shape on one node can reuse  */
/*  it here without a second mental model.  The lookup root is built   */
/*  by pn_json_lookup_root_for_message() so paths like                 */
/*  `${data/device}` or `${topic}` resolve against the inbound         */
/*  envelope and data bag directly.                                    */
/* ------------------------------------------------------------------ */

/** Stringify a #JsonNode the way the placeholder expander wants:
 *  scalars (string, int, double, bool) render as their natural text
 *  form; objects and arrays render as compact JSON; %NULL renders as
 *  the empty string. */
static gchar *
node_to_string (JsonNode *node)
{
    if (node == NULL)
        return g_strdup ("");

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType t = json_node_get_value_type (node);

        if (t == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));
        if (t == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
        if (t == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (node));
        if (t == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node) ? "true" : "false");
        return g_strdup ("");
    }

    {
        JsonGenerator *gen = json_generator_new ();
        gchar         *out;

        json_generator_set_root (gen, node);
        out = json_generator_to_data (gen, NULL);
        g_object_unref (gen);
        return out != NULL ? out : g_strdup ("");
    }
}

static gchar *
expand_placeholders (
        const gchar *tmpl,
        JsonObject  *root)
{
    GString     *out;
    const gchar *p;

    if (tmpl == NULL)
        return g_strdup ("");

    out = g_string_new (NULL);
    p   = tmpl;

    while (*p != '\0')
    {
        if (p[0] == '$' && p[1] == '{')
        {
            const gchar *end = strchr (p + 2, '}');

            if (end != NULL)
            {
                gchar    *path  = g_strndup (p + 2, end - (p + 2));
                JsonNode *node  = pn_json_resolve_path (root, path);
                gchar    *value = node_to_string (node);

                g_string_append (out, value);

                g_free (value);
                g_free (path);
                p = end + 1;
                continue;
            }
        }

        g_string_append_c (out, *p);
        p++;
    }

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Payload extraction                                                 */
/*                                                                     */
/*  When the user has set a payload template, expand it against the    */
/*  message and ship the resulting UTF-8 bytes.  When the field is     */
/*  empty, fall back to whatever the inbound message carries in        */
/*  `data.payload` -- string members go out as raw bytes, structured   */
/*  members (objects, arrays, numbers, booleans) are serialised back   */
/*  to JSON so a Source -> Sink relay between two brokers round-trips  */
/*  the original JSON shape rather than turning every publish into a   */
/*  stringified `[object Object]`-style placeholder.                   */
/* ------------------------------------------------------------------ */

/** Build the payload bytes to publish, writing the result to
 *  *@out_bytes (caller frees) and *@out_len.  Returns %TRUE on
 *  success, %FALSE when no usable payload could be derived (the
 *  receive path then drops the message rather than publishing an
 *  empty string which would otherwise look like a deliberate null
 *  publish). */
static gboolean
build_payload (
        PnMqttSink *self,
        PnMessage  *message,
        gchar     **out_bytes,
        gsize      *out_len)
{
    if (self->payload_template != NULL && *self->payload_template != '\0')
    {
        JsonObject *root     = pn_json_lookup_root_for_message (message);
        gchar      *expanded = expand_placeholders (self->payload_template,
                                                    root);
        json_object_unref (root);

        *out_bytes = expanded;
        *out_len   = strlen (expanded);
        return TRUE;
    }

    {
        JsonNode *payload = pn_message_get_member (message, "payload");

        if (payload == NULL)
            return FALSE;

        if (JSON_NODE_HOLDS_VALUE (payload) &&
            json_node_get_value_type (payload) == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (payload);
            *out_bytes = g_strdup (s != NULL ? s : "");
            *out_len   = strlen (*out_bytes);
            return TRUE;
        }

        {
            JsonGenerator *gen = json_generator_new ();
            gsize          len = 0;
            gchar         *out;

            json_generator_set_root (gen, payload);
            out = json_generator_to_data (gen, &len);
            g_object_unref (gen);
            if (out == NULL)
                return FALSE;

            *out_bytes = out;
            *out_len   = len;
            return TRUE;
        }
    }
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
} PnMqttSinkConnStateClosure;

static gboolean
conn_state_trampoline (gpointer data)
{
    PnMqttSinkConnStateClosure *c = data;
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
    PnMqttSinkConnStateClosure *c = data;
    g_object_unref (c->self);
    g_free (c);
}

static void
post_conn_state_on_main (PnMqttSink *self, gboolean connected)
{
    PnMqttSinkConnStateClosure *c = g_new0 (PnMqttSinkConnStateClosure, 1);
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
    PnMqttSink *self = PN_MQTT_SINK (obj);

    (void) mosq;

    if (rc != 0)
    {
        g_warning ("pn-mqtt-sink: connect refused: %s",
                   mosquitto_connack_string (rc));
        post_conn_state_on_main (self, FALSE);
        return;
    }

    post_conn_state_on_main (self, TRUE);
}

static void
on_mqtt_disconnect (
        struct mosquitto *mosq,
        void             *obj,
        int               rc)
{
    PnMqttSink *self = PN_MQTT_SINK (obj);

    (void) mosq;

    if (rc != 0)
        g_message ("pn-mqtt-sink: disconnected (%s); reconnecting…",
                   mosquitto_strerror (rc));

    post_conn_state_on_main (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Strip the "mqtt/" prefix #PnMqtt prepends to every envelope topic
 *  it emits (`stat/sonoff19/POWER` arrives as `mqtt/stat/sonoff19/POWER`
 *  so a downstream Filter can pattern-match on a single envelope
 *  field).  A Source -> Sink relay between two brokers republishing
 *  the same topic shape would otherwise end up double-prefixing
 *  (`mqtt/mqtt/stat/...`) on the destination, which no real broker
 *  layout uses.  Topics that do not carry the prefix are passed
 *  through unchanged so a synthesised topic like
 *  `cmnd/sonoff19/POWER` published from a Format / Rewrite step lands
 *  exactly as the user wrote it. */
static const gchar *
strip_mqtt_prefix (const gchar *topic)
{
    if (topic == NULL)
        return NULL;
    if (g_str_has_prefix (topic, "mqtt/"))
        return topic + 5;
    return topic;
}

static void
pn_mqtt_sink_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnMqttSink  *self = PN_MQTT_SINK (node);
    const gchar *publish_topic;
    gchar       *payload = NULL;
    gsize        payload_len = 0;
    int          err;

    /* No broker session yet -- drop silently rather than queue, since
     * we have no bounded backlog and a flapping broker would otherwise
     * grow memory without limit.  The visual state is already red so
     * the user can see why nothing is going out. */
    if (self->client == NULL || !self->connected)
        return;

    /* Publish topic comes straight off the inbound envelope -- a
     * canvas pattern like `Tasmota Switch -> Rewrite (topic =
     * cmnd/${data/device}/POWER) -> MQTT Sink` then drives the right
     * broker topic per message without the sink itself having to
     * carry a topic template.  Messages without a topic are dropped. */
    publish_topic = strip_mqtt_prefix (pn_message_get_topic (message));
    if (publish_topic == NULL || *publish_topic == '\0')
        return;

    if (!build_payload (self, message, &payload, &payload_len))
        return;

    err = mosquitto_publish (self->client,
                             NULL,                 /* mid -- we do not track */
                             publish_topic,
                             (int) payload_len,
                             payload,
                             (int) self->qos,
                             self->retain);
    if (err != MOSQ_ERR_SUCCESS)
        g_warning ("pn-mqtt-sink: publish('%s') failed: %s",
                   publish_topic, mosquitto_strerror (err));

    g_free (payload);
}

/* ------------------------------------------------------------------ */
/*  Client lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
stop_client (PnMqttSink *self)
{
    if (self->client == NULL)
        return;

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

static void
restart_client (PnMqttSink *self)
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
        g_warning ("pn-mqtt-sink: invalid URL '%s'", self->url);
        apply_visual_state (self);
        return;
    }

    ensure_mosquitto_initialised ();

    self->client = mosquitto_new (
            (self->client_id != NULL && *self->client_id != '\0')
                ? self->client_id : NULL,
            TRUE,
            self);
    if (self->client == NULL)
    {
        g_warning ("pn-mqtt-sink: mosquitto_new failed: %s",
                   g_strerror (errno));
        g_free (host);
        apply_visual_state (self);
        return;
    }

    mosquitto_connect_callback_set    (self->client, on_mqtt_connect);
    mosquitto_disconnect_callback_set (self->client, on_mqtt_disconnect);

    mosquitto_reconnect_delay_set (self->client,
            PN_MQTT_SINK_RECONNECT_DELAY_MIN,
            PN_MQTT_SINK_RECONNECT_DELAY_MAX,
            TRUE);

    if (self->username != NULL && *self->username != '\0')
    {
        err = mosquitto_username_pw_set (self->client,
                self->username,
                (self->password != NULL && *self->password != '\0')
                    ? self->password : NULL);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt-sink: username_pw_set failed: %s",
                       mosquitto_strerror (err));
    }

    if (tls)
    {
        err = mosquitto_tls_set (self->client, NULL, NULL, NULL, NULL, NULL);
        if (err != MOSQ_ERR_SUCCESS)
            g_warning ("pn-mqtt-sink: tls_set failed: %s",
                       mosquitto_strerror (err));
    }

    err = mosquitto_connect_async (self->client, host, port,
                                   PN_MQTT_SINK_KEEPALIVE_SECONDS);
    if (err != MOSQ_ERR_SUCCESS)
    {
        g_warning ("pn-mqtt-sink: connect_async(%s:%d) failed: %s",
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
        g_warning ("pn-mqtt-sink: loop_start failed: %s",
                   mosquitto_strerror (err));
        mosquitto_destroy (self->client);
        self->client = NULL;
        g_free (host);
        apply_visual_state (self);
        return;
    }
    self->loop_running = TRUE;

    g_free (host);
    apply_visual_state (self);
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
    PnMqttSink *self = PN_MQTT_SINK (object);

    switch (prop_id)
    {
    case PROP_URL:       g_value_set_string  (value, self->url);              break;
    case PROP_PAYLOAD:   g_value_set_string  (value, self->payload_template); break;
    case PROP_RETAIN:    g_value_set_boolean (value, self->retain);           break;
    case PROP_USERNAME:  g_value_set_string  (value, self->username);         break;
    case PROP_PASSWORD:  g_value_set_string  (value, self->password);         break;
    case PROP_CLIENT_ID: g_value_set_string  (value, self->client_id);        break;
    case PROP_QOS:       g_value_set_uint    (value, self->qos);              break;
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
    PnMqttSink *self = PN_MQTT_SINK (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_free (self->url);
        self->url = g_value_dup_string (value);
        restart_client (self);
        break;
    case PROP_PAYLOAD:
        g_free (self->payload_template);
        self->payload_template = g_value_dup_string (value);
        break;
    case PROP_RETAIN:
        self->retain = g_value_get_boolean (value);
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
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_mqtt_sink_dispose (GObject *object)
{
    PnMqttSink *self = PN_MQTT_SINK (object);

    stop_client (self);

    G_OBJECT_CLASS (pn_mqtt_sink_parent_class)->dispose (object);
}

static void
pn_mqtt_sink_finalize (GObject *object)
{
    PnMqttSink *self = PN_MQTT_SINK (object);

    g_clear_pointer (&self->url,              g_free);
    g_clear_pointer (&self->payload_template, g_free);
    g_clear_pointer (&self->username,         g_free);
    g_clear_pointer (&self->password,         g_free);
    g_clear_pointer (&self->client_id,        g_free);

    G_OBJECT_CLASS (pn_mqtt_sink_parent_class)->finalize (object);
}

static void
pn_mqtt_sink_class_init (PnMqttSinkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_mqtt_sink_get_property;
    object_class->set_property = pn_mqtt_sink_set_property;
    object_class->dispose      = pn_mqtt_sink_dispose;
    object_class->finalize     = pn_mqtt_sink_finalize;

    node_class->receive        = pn_mqtt_sink_receive;

    /* Palette pin: the instance icon flips with connection state but
     * the palette wants a stable glyph -- pin the paper-plane so the
     * entry reads consistently. */
    node_class->palette_icon   = PN_MQTT_SINK_NORMAL_ICON;
    node_class->class_name     = "MQTT Sink";
    node_class->icon           = PN_MQTT_SINK_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    props[PROP_URL] = g_param_spec_string (
            "url", "Broker URL",
            "MQTT broker URL.  Accepts tcp://host[:port] (plain MQTT, "
            "default port 1883), ssl://host[:port] or "
            "mqtts://host[:port] (MQTT over TLS, default port 8883), "
            "or a bare host[:port] which is treated as plain TCP.  "
            "Defaults to tcp://mqtt.homelab.local:1883 so a Source + "
            "Sink pair dropped onto a fresh worksheet talk to the "
            "same broker without configuration.",
            PN_MQTT_SINK_DEFAULT_URL,
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
            "generate a random one -- fine for most cases.  Pin a "
            "fixed id when the broker enforces an ACL by client id "
            "(and use a *different* id from any paired MQTT Source on "
            "the same broker, since two clients with the same id kick "
            "each other off).",
            NULL,
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
    PnNode *node = PN_NODE (self);

    self->url              = g_strdup (PN_MQTT_SINK_DEFAULT_URL);
    self->payload_template = NULL;
    self->username         = NULL;
    self->password         = NULL;
    self->client_id        = NULL;
    self->qos              = 0;
    self->retain           = FALSE;
    self->client           = NULL;
    self->loop_running     = FALSE;
    self->connected        = FALSE;

    pn_node_set_class_name (node, "MQTT Sink");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* Start the broker session immediately so a freshly-dropped node
     * is reachable the moment a message arrives.  The visual state
     * stays red until on_mqtt_connect lands, exactly matching PnMqtt's
     * bootstrap. */
    restart_client (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMqttSink *
pn_mqtt_sink_new (void)
{
    return g_object_new (PN_TYPE_MQTT_SINK, NULL);
}
