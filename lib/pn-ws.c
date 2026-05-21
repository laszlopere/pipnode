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

#include "pn-ws.h"

#include <libsoup/soup.h>

/* Visual states.  Body colour carries the alert; the icon panel is
 * rendered in white on top. */
#define PN_WS_NORMAL_ICON  "\xef\x87\xa6"  /* fa-plug U+F1E6 */
#define PN_WS_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757         */

/* Re-connect backoff bounds.  Starting small lets a transient blip
 * recover quickly; the cap prevents runaway hammering of an endpoint
 * that is going to stay down for hours. */
#define PN_WS_RECONNECT_INITIAL_MS  1000u
#define PN_WS_RECONNECT_MAX_MS     60000u

/* Default body colour applied when a subclass leaves
 * PnWebsocketClass.normal_color zero-initialised.  Teal for "live
 * stream" — visually distinct from the HTTP green so a glance at the
 * canvas tells request/response from push. */
static const GdkRGBA PN_WS_DEFAULT_COLOR = { 0.30, 0.62, 0.70, 1.0 };

typedef struct
{
    /* Set by the property setter, read on the main thread when
     * starting a fresh connect.  Owned by the node. */
    gchar *url;

    /* Lazily-created SoupSession reused across reconnects so cookies,
     * proxy resolution and TLS state survive a drop. */
    SoupSession              *session;

    /* Live connection (only set between handshake and "closed").
     * Strong reference; dropped in the close handler and dispose. */
    SoupWebsocketConnection  *connection;

    /* Cancellable handed to soup_session_websocket_connect_async so a
     * URL change or disposal aborts an in-flight handshake.  Replaced
     * by every connect attempt; cancelled on dispose. */
    GCancellable             *cancellable;

    /* GLib timeout source ids for the reconnect/handshake watchdog.
     * 0 means "no source pending"; cleared from the handlers that
     * fire them. */
    guint                     reconnect_source;

    /* Current reconnect backoff in milliseconds.  Doubles after each
     * failure, capped at PN_WS_RECONNECT_MAX_MS, reset to
     * PN_WS_RECONNECT_INITIAL_MS after a successful handshake. */
    guint                     reconnect_delay_ms;

    /* Tri-state used to gate the start_connect path: while a connect
     * is already in flight we ignore additional triggers, otherwise
     * we'd queue duplicate handshakes. */
    gboolean                  connecting;

    /* %TRUE between successful handshake and the "closed" signal.
     * Surfaces through pn_ws_is_connected() for subclass introspection. */
    gboolean                  connected;

    /* Set during dispose so handlers that fire after the cancellable
     * cancels know to bail out instead of poking the dying object. */
    gboolean                  disposing;
} PnWebsocketPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnWebsocket, pn_websocket, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_URL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static gboolean default_is_configured (PnWebsocket *self);
static void     default_connected    (PnWebsocket *self);
static void     default_text_message (PnWebsocket *self, const gchar *text);

static void start_connect    (PnWebsocket *self);
static void schedule_retry   (PnWebsocket *self);
static void clear_connection (PnWebsocket *self);
static void clear_reconnect  (PnWebsocket *self);

/* ------------------------------------------------------------------ */
/*  URL accessor                                                       */
/* ------------------------------------------------------------------ */

gchar *
pn_ws_dup_url (PnWebsocket *self)
{
    PnWebsocketPrivate *priv;

    g_return_val_if_fail (PN_IS_WEBSOCKET (self), NULL);

    priv = pn_websocket_get_instance_private (self);
    return g_strdup (priv->url);
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

void
pn_ws_apply_visual_state (
        PnWebsocket *self,
        gboolean     configured)
{
    PnWebsocketClass *klass;
    PnNode           *node;

    g_return_if_fail (PN_IS_WEBSOCKET (self));

    klass = PN_WEBSOCKET_GET_CLASS (self);
    node  = PN_NODE (self);

    if (configured)
    {
        const gchar *icon = klass->normal_icon
                ? klass->normal_icon
                : PN_WS_NORMAL_ICON;

        const GdkRGBA *color = (klass->normal_color.alpha > 0.0)
                ? &klass->normal_color
                : &PN_WS_DEFAULT_COLOR;

        pn_node_set_color (node, color);
        pn_node_set_icon  (node, icon);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_WS_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API: send / status                                          */
/* ------------------------------------------------------------------ */

void
pn_ws_send_text (
        PnWebsocket *self,
        const gchar *text)
{
    PnWebsocketPrivate *priv;

    g_return_if_fail (PN_IS_WEBSOCKET (self));
    g_return_if_fail (text != NULL);

    priv = pn_websocket_get_instance_private (self);

    /* Silently drop while disconnected: subclasses call this from
     * connected() or in response to other events and should not have
     * to track the link state themselves. */
    if (priv->connection == NULL || !priv->connected)
        return;

    soup_websocket_connection_send_text (priv->connection, text);
}

gboolean
pn_ws_is_connected (PnWebsocket *self)
{
    PnWebsocketPrivate *priv;

    g_return_val_if_fail (PN_IS_WEBSOCKET (self), FALSE);

    priv = pn_websocket_get_instance_private (self);
    return priv->connected;
}

/* ------------------------------------------------------------------ */
/*  Default vfunc implementations                                      */
/* ------------------------------------------------------------------ */

static gboolean
default_is_configured (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    return priv->url != NULL && *priv->url != '\0';
}

static void
default_connected (PnWebsocket *self)
{
    (void) self;
}

static void
default_text_message (
        PnWebsocket *self,
        const gchar *text)
{
    (void) self;
    (void) text;
}

/* ------------------------------------------------------------------ */
/*  Connection lifecycle                                               */
/*                                                                     */
/*  All of this runs on the main thread.  The connect call is async   */
/*  and the inbound message/close signals are delivered through the   */
/*  default GMainContext, so a single thread serialises every         */
/*  transition we need to coordinate.                                  */
/* ------------------------------------------------------------------ */

static void
on_ws_message (
        SoupWebsocketConnection *conn,
        gint                     type,
        GBytes                  *data,
        gpointer                 user_data)
{
    PnWebsocket      *self = PN_WEBSOCKET (user_data);
    PnWebsocketClass *klass;
    gconstpointer     payload;
    gsize             len;
    gchar            *text;

    (void) conn;

    /* Binary frames don't have a defined shape for our subclasses
     * (the JSON-RPC dialects we target all send text), so we drop
     * them.  Adding a binary_message vfunc later is straightforward. */
    if (type != SOUP_WEBSOCKET_DATA_TEXT)
        return;

    payload = g_bytes_get_data (data, &len);
    /* libsoup hands us bytes that are not guaranteed NUL-terminated;
     * copy into a NUL-terminated buffer so subclasses can treat the
     * payload as a plain C string. */
    text    = g_strndup ((const gchar *) payload, len);

    klass = PN_WEBSOCKET_GET_CLASS (self);
    if (klass->text_message != NULL)
        klass->text_message (self, text);

    g_free (text);
}

static void
on_ws_closed (
        SoupWebsocketConnection *conn,
        gpointer                 user_data)
{
    PnWebsocket        *self = PN_WEBSOCKET (user_data);
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    (void) conn;

    if (priv->disposing)
        return;

    /* Drop our reference to the dead connection and queue a retry.
     * The backoff is reset by every successful handshake, so a
     * stable connection that drops once does not penalise the next. */
    priv->connected = FALSE;
    g_clear_object (&priv->connection);

    if (PN_WEBSOCKET_GET_CLASS (self)->is_configured (self))
        schedule_retry (self);
}

static void
on_ws_error (
        SoupWebsocketConnection *conn,
        GError                  *error,
        gpointer                 user_data)
{
    PnWebsocket *self = PN_WEBSOCKET (user_data);

    (void) conn;

    /* Soup raises this for protocol/transport errors; the "closed"
     * signal follows so the actual reconnect happens in on_ws_closed.
     * Logging the cause helps the user diagnose certificate or
     * handshake problems. */
    g_warning ("pn-ws: connection error on '%s': %s",
               PN_WEBSOCKET_GET_CLASS (self)->is_configured (self)
                   ? "" : "(unconfigured)",
               error ? error->message : "(unknown)");
}

static void
on_connect_done (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    SoupSession             *session = SOUP_SESSION (source);
    PnWebsocket             *self    = PN_WEBSOCKET (user_data);
    PnWebsocketPrivate      *priv    = pn_websocket_get_instance_private (self);
    SoupWebsocketConnection *conn;
    GError                  *error   = NULL;
    PnWebsocketClass        *klass;

    conn = soup_session_websocket_connect_finish (session, result, &error);

    priv->connecting = FALSE;

    if (priv->disposing)
    {
        /* The cancellable was cancelled in dispose; the finish call
         * gave us a connection or an error, both of which we drop. */
        g_clear_error  (&error);
        g_clear_object (&conn);
        g_object_unref (self);
        return;
    }

    if (conn == NULL)
    {
        /* Cancellation while still alive: the URL was cleared or
         * replaced.  No retry; the URL setter has already started
         * the new connect (if any). */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_clear_error (&error);
            g_object_unref (self);
            return;
        }

        g_warning ("pn-ws: handshake failed: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);

        if (PN_WEBSOCKET_GET_CLASS (self)->is_configured (self))
            schedule_retry (self);

        g_object_unref (self);
        return;
    }

    /* Successful handshake: take ownership, wire up signals, and
     * notify the subclass.  Resetting the backoff means a stable
     * link that has been up for a while reconnects quickly when
     * its endpoint blips. */
    priv->connection         = conn;  /* transfer */
    priv->connected          = TRUE;
    priv->reconnect_delay_ms = PN_WS_RECONNECT_INITIAL_MS;

    /* libsoup3's default incoming-frame cap is 128 KiB, which is well
     * below what some endpoints we target push at us — some servers
     * routinely return multi-MB responses in a single frame, and tripping
     * the limit aborts the connection with "Received WebSocket payload
     * from the server larger than configured max-incoming-payload-size".
     * Disable the cap; the user has chosen the endpoint and we trust
     * what it sends. */
    soup_websocket_connection_set_max_incoming_payload_size (conn, 0);

    g_signal_connect (conn, "message", G_CALLBACK (on_ws_message), self);
    g_signal_connect (conn, "closed",  G_CALLBACK (on_ws_closed),  self);
    g_signal_connect (conn, "error",   G_CALLBACK (on_ws_error),   self);

    klass = PN_WEBSOCKET_GET_CLASS (self);
    if (klass->connected != NULL)
        klass->connected (self);

    g_object_unref (self);
}

/** Begin one handshake attempt.  Aborts any in-flight connect first
 *  so we never have two parallel handshakes — handy when a URL change
 *  triggers a reconnect while the previous attempt is still pending. */
static void
start_connect (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);
    SoupMessage        *msg;

    if (priv->disposing)
        return;
    if (priv->connecting)
        return;
    if (priv->connection != NULL)
        return;
    if (priv->url == NULL || *priv->url == '\0')
        return;

    if (priv->session == NULL)
        priv->session = soup_session_new ();

    g_clear_object (&priv->cancellable);
    priv->cancellable = g_cancellable_new ();

    msg = soup_message_new (SOUP_METHOD_GET, priv->url);
    if (msg == NULL)
    {
        g_warning ("pn-ws: '%s' is not a valid URL", priv->url);
        schedule_retry (self);
        return;
    }

    priv->connecting = TRUE;

    /* Hand a strong ref to the async callback so the object cannot
     * be finalised between the call and the result. */
    soup_session_websocket_connect_async (priv->session,
                                          msg,
                                          NULL,   /* origin    */
                                          NULL,   /* protocols */
                                          G_PRIORITY_DEFAULT,
                                          priv->cancellable,
                                          on_connect_done,
                                          g_object_ref (self));

    g_object_unref (msg);
}

static gboolean
on_reconnect_timer (gpointer user_data)
{
    PnWebsocket        *self = PN_WEBSOCKET (user_data);
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    priv->reconnect_source = 0;

    if (priv->disposing)
        return G_SOURCE_REMOVE;

    if (PN_WEBSOCKET_GET_CLASS (self)->is_configured (self))
        start_connect (self);

    return G_SOURCE_REMOVE;
}

static void
schedule_retry (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);
    guint               delay;

    if (priv->reconnect_source != 0)
        return;

    delay = priv->reconnect_delay_ms;
    /* Exponential backoff capped at PN_WS_RECONNECT_MAX_MS.  Compute
     * the next delay before installing the source so a fast burst of
     * failures still progresses through the curve. */
    priv->reconnect_delay_ms = MIN (priv->reconnect_delay_ms * 2u,
                                    PN_WS_RECONNECT_MAX_MS);

    priv->reconnect_source = g_timeout_add (delay,
                                            on_reconnect_timer,
                                            self);
}

static void
clear_reconnect (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    if (priv->reconnect_source != 0)
    {
        g_source_remove (priv->reconnect_source);
        priv->reconnect_source = 0;
    }
}

/** Tear down the live connection (or in-flight handshake) leaving
 *  the SoupSession alive for reuse.  Safe to call from any state. */
static void
clear_connection (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    if (priv->cancellable != NULL)
    {
        g_cancellable_cancel (priv->cancellable);
        g_clear_object (&priv->cancellable);
    }

    if (priv->connection != NULL)
    {
        /* Politely close so the peer sees a clean shutdown frame.
         * Signals fire synchronously enough that the close handler
         * may run before this returns; that handler handles the
         * disposing flag. */
        soup_websocket_connection_close (priv->connection,
                                         SOUP_WEBSOCKET_CLOSE_NORMAL,
                                         NULL);
        g_clear_object (&priv->connection);
    }

    priv->connected  = FALSE;
    priv->connecting = FALSE;
}

/* ------------------------------------------------------------------ */
/*  URL setter                                                         */
/* ------------------------------------------------------------------ */

static void
ws_set_url (
        PnWebsocket *self,
        const gchar *url)
{
    PnWebsocketPrivate *priv  = pn_websocket_get_instance_private (self);
    PnWebsocketClass   *klass = PN_WEBSOCKET_GET_CLASS (self);
    gchar              *replacement;
    gboolean            configured;

    if (g_strcmp0 (priv->url, url) == 0)
        return;

    replacement = (url != NULL) ? g_strdup (url) : NULL;

    /* Drop any in-flight handshake or live connection before swapping
     * the URL: the new value defines the new target so the old socket
     * is no longer interesting. */
    clear_reconnect  (self);
    clear_connection (self);

    g_free (priv->url);
    priv->url = replacement;
    /* A fresh URL is treated as a fresh attempt cycle: the user
     * presumably typed a working endpoint and shouldn't have to wait
     * out the previous backoff. */
    priv->reconnect_delay_ms = PN_WS_RECONNECT_INITIAL_MS;

    configured = klass->is_configured (self);
    pn_ws_apply_visual_state (self, configured);

    if (configured)
        start_connect (self);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_URL]);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_websocket_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnWebsocket *self = PN_WEBSOCKET (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_value_take_string (value, pn_ws_dup_url (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_websocket_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnWebsocket *self = PN_WEBSOCKET (object);

    switch (prop_id)
    {
    case PROP_URL:
        ws_set_url (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_websocket_dispose (GObject *object)
{
    PnWebsocket        *self = PN_WEBSOCKET (object);
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    /* The flag tells late-arriving callbacks (handshake completion,
     * "closed", reconnect timer) to bail out instead of poking the
     * dying object — the cancellable handles the in-flight handshake
     * but the close handler can still run synchronously from
     * clear_connection() below. */
    priv->disposing = TRUE;

    clear_reconnect  (self);
    clear_connection (self);

    if (priv->session != NULL)
    {
        soup_session_abort (priv->session);
        g_clear_object (&priv->session);
    }

    G_OBJECT_CLASS (pn_websocket_parent_class)->dispose (object);
}

static void
pn_websocket_finalize (GObject *object)
{
    PnWebsocket        *self = PN_WEBSOCKET (object);
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    g_clear_pointer (&priv->url, g_free);

    G_OBJECT_CLASS (pn_websocket_parent_class)->finalize (object);
}

static void
pn_websocket_class_init (PnWebsocketClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_websocket_get_property;
    object_class->set_property = pn_websocket_set_property;
    object_class->dispose      = pn_websocket_dispose;
    object_class->finalize     = pn_websocket_finalize;

    klass->normal_icon  = PN_WS_NORMAL_ICON;
    klass->normal_color = PN_WS_DEFAULT_COLOR;

    klass->is_configured = default_is_configured;
    klass->connected     = default_connected;
    klass->text_message  = default_text_message;

    /* Stable palette glyph regardless of instance state. */
    node_class->palette_icon = PN_WS_NORMAL_ICON;
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;

    props[PROP_URL] = g_param_spec_string (
            "url", "URL",
            "ws:// or wss:// URL of the WebSocket endpoint to connect "
            "to; while empty the node is marked as needing "
            "configuration and no connection is attempted",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_websocket_init (PnWebsocket *self)
{
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);
    PnNode             *node = PN_NODE (self);

    priv->url                = NULL;
    priv->session            = NULL;
    priv->connection         = NULL;
    priv->cancellable        = NULL;
    priv->reconnect_source   = 0;
    priv->reconnect_delay_ms = PN_WS_RECONNECT_INITIAL_MS;
    priv->connecting         = FALSE;
    priv->connected          = FALSE;
    priv->disposing          = FALSE;

    pn_node_set_class_name (node, "WebSocket");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Start unconfigured (red ❗); setting the URL flips us. */
    pn_ws_apply_visual_state (self, FALSE);
}

PnWebsocket *
pn_websocket_new (void)
{
    return g_object_new (PN_TYPE_WEBSOCKET, NULL);
}
