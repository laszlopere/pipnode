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
static const PnColor PN_WS_DEFAULT_COLOR = { 0.30, 0.62, 0.70, 1.0 };

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

    /* ---- credentials-profile binding (pn_ws_bind_profile) -------- */

    /* Name of the profile-ref property the subclass installed, or %NULL
     * when no binding is active.  Owned. */
    gchar    *profile_prop;

    /* Subscription on self::notify::<profile_prop> so a setter triggers
     * a resync.  0 when no binding is active or the subscription was
     * already torn down. */
    gulong    notify_handler;

    /* Subscription on the default vault's ::changed so a manager edit
     * (rename, field change, delete) takes effect live.  0 when no
     * binding is active. */
    gulong    vault_handler;

    /* One-shot idle that performs the initial resolve once property
     * loading has settled — the subclass calls bind_profile from
     * _init / constructed, but at that point the deserialiser has not
     * yet applied the saved id.  0 once fired or cancelled. */
    guint     initial_resync_id;
} PnWebsocketPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnWebsocket, pn_websocket, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_URL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static gboolean default_is_configured     (PnWebsocket *self);
static void     default_connected         (PnWebsocket *self);
static void     default_text_message      (PnWebsocket *self, const gchar *text);
static void     default_profile_resolved  (PnWebsocket *self, PnProfile *profile);
static void     default_prepare_message   (PnWebsocket *self, SoupMessage *msg);
static gboolean default_accept_certificate (PnWebsocket          *self,
                                            GTlsCertificate      *cert,
                                            GTlsCertificateFlags  errors);
static void     default_connection_failed  (PnWebsocket *self,
                                            const gchar *reason);
static void     notify_connection_failed   (PnWebsocket *self,
                                            const gchar *reason);

static gboolean on_msg_accept_certificate (SoupMessage          *msg,
                                           GTlsCertificate      *cert,
                                           GTlsCertificateFlags  errors,
                                           gpointer              user_data);

static void start_connect    (PnWebsocket *self);
static void schedule_retry   (PnWebsocket *self);
static void clear_connection (PnWebsocket *self);
static void clear_reconnect  (PnWebsocket *self);

static void     resync_profile        (PnWebsocket *self);
static void     on_self_notify_profile (GObject *object,
                                        GParamSpec *pspec,
                                        gpointer user_data);
static void     on_vault_changed      (gpointer self);
static gboolean initial_resync_idle   (gpointer user_data);

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

        const PnColor *color = (klass->normal_color.alpha > 0.0)
                ? &klass->normal_color
                : &PN_WS_DEFAULT_COLOR;

        pn_node_set_color (node, color);
        pn_node_set_icon  (node, icon);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
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

/** Default profile resolver: read the profile's "url" field (or "" when
 *  the profile didn't resolve) and assign it to PnWebsocket:url, which
 *  drives the existing connect/reconnect loop.  Subclasses override to
 *  also pull secrets, or to compose the URL from multiple fields. */
static void
default_profile_resolved (
        PnWebsocket *self,
        PnProfile   *profile)
{
    gchar *url = (profile != NULL)
            ? pn_profile_get_string (profile, "url")
            : g_strdup ("");

    g_object_set (self, "url", url != NULL ? url : "", NULL);
    g_free (url);
}

static void
default_prepare_message (
        PnWebsocket *self,
        SoupMessage *msg)
{
    (void) self;
    (void) msg;
}

/** Default: defer to the system trust store.  Returning %FALSE here
 *  leaves room for any extra accept-certificate handler the subclass
 *  installed via prepare_message — libsoup ORs the handler results, so
 *  one TRUE anywhere accepts the cert. */
static gboolean
default_accept_certificate (
        PnWebsocket          *self,
        GTlsCertificate      *cert,
        GTlsCertificateFlags  errors)
{
    (void) self;
    (void) cert;
    (void) errors;
    return FALSE;
}

/** Default failure seam: do nothing.  The base class also g_warning()s
 *  the same line for developer diagnostics, so this default leaves no
 *  trace silently — it is the subclass's responsibility (per PLUGINS
 *  §12) to override and emit a `success = FALSE` PnMessage so the
 *  failure becomes visible on the canvas. */
static void
default_connection_failed (
        PnWebsocket *self,
        const gchar *reason)
{
    (void) self;
    (void) reason;
}

/** Single entry point for every failure site to forward the reason to
 *  the subclass-facing PnWebsocketClass::connection_failed vfunc.  No
 *  base-class log: a network source that auto-reconnects will hit
 *  failures on every DNS hiccup and peer restart, and a WARNING-level
 *  line per retry just spams the journal.  Per PLUGINS §12 the
 *  emitted PnMessage (built by the subclass override) is the user-
 *  facing channel; a developer who wants to trace failures should run
 *  the subclass that overrides this vfunc, not eyeball the journal. */
static void
notify_connection_failed (
        PnWebsocket *self,
        const gchar *reason)
{
    PnWebsocketClass *klass = PN_WEBSOCKET_GET_CLASS (self);
    const gchar      *text  = reason ? reason : "(unknown)";

    if (klass->connection_failed != NULL)
        klass->connection_failed (self, text);
}

static gboolean
on_msg_accept_certificate (
        SoupMessage          *msg,
        GTlsCertificate      *cert,
        GTlsCertificateFlags  errors,
        gpointer              user_data)
{
    PnWebsocket      *self = PN_WEBSOCKET (user_data);
    PnWebsocketClass *klass;

    (void) msg;

    klass = PN_WEBSOCKET_GET_CLASS (self);
    if (klass->accept_certificate != NULL)
        return klass->accept_certificate (self, cert, errors);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Profile binding                                                    */
/*                                                                     */
/*  Subclasses install their own profile-ref property and call         */
/*  pn_ws_bind_profile() once to delegate the resolve/reconnect glue   */
/*  to this base class.  Three triggers re-resolve the profile:        */
/*    - notify on the bound property (a setter fired)                  */
/*    - the vault's ::changed signal (manager edit / rename / delete)  */
/*    - a one-shot idle scheduled by bind_profile to cover the         */
/*      initial-load case where the saved id arrives via g_object_set  */
/*      AFTER the subclass's _init / constructed.                      */
/* ------------------------------------------------------------------ */

static void
resync_profile (PnWebsocket *self)
{
    PnWebsocketPrivate *priv  = pn_websocket_get_instance_private (self);
    PnWebsocketClass   *klass = PN_WEBSOCKET_GET_CLASS (self);
    PnProfile          *p;

    if (priv->disposing)
        return;
    if (priv->profile_prop == NULL)
        return;

    p = pn_node_get_profile (PN_NODE (self), priv->profile_prop);

    if (klass->profile_resolved != NULL)
        klass->profile_resolved (self, p);
}

static void
on_self_notify_profile (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) pspec;
    (void) user_data;
    resync_profile (PN_WEBSOCKET (object));
}

static void
on_vault_changed (gpointer self)
{
    resync_profile (PN_WEBSOCKET (self));
}

static gboolean
initial_resync_idle (gpointer user_data)
{
    PnWebsocket        *self = PN_WEBSOCKET (user_data);
    PnWebsocketPrivate *priv = pn_websocket_get_instance_private (self);

    priv->initial_resync_id = 0;
    resync_profile (self);
    return G_SOURCE_REMOVE;
}

void
pn_ws_bind_profile (
        PnWebsocket *self,
        const gchar *prop_name)
{
    PnWebsocketPrivate *priv;
    gchar              *signal_name;

    g_return_if_fail (PN_IS_WEBSOCKET (self));
    g_return_if_fail (prop_name != NULL && *prop_name != '\0');

    priv = pn_websocket_get_instance_private (self);

    /* Idempotent: drop any previous binding so a subclass can call this
     * twice (or be re-bound at runtime) without leaking handlers. */
    if (priv->notify_handler != 0)
    {
        g_signal_handler_disconnect (self, priv->notify_handler);
        priv->notify_handler = 0;
    }
    if (priv->vault_handler != 0)
    {
        g_signal_handler_disconnect (pn_vault_get_default (),
                                     priv->vault_handler);
        priv->vault_handler = 0;
    }
    if (priv->initial_resync_id != 0)
    {
        g_source_remove (priv->initial_resync_id);
        priv->initial_resync_id = 0;
    }
    g_clear_pointer (&priv->profile_prop, g_free);

    priv->profile_prop = g_strdup (prop_name);

    /* "notify::<prop_name>" — only fires when that one property changes,
     * which is the cheap path: most property changes on the node are not
     * profile-ref edits and we don't want to resolve for every keystroke
     * on, say, a topic field. */
    signal_name = g_strconcat ("notify::", prop_name, NULL);
    priv->notify_handler = g_signal_connect (self,
                                             signal_name,
                                             G_CALLBACK (on_self_notify_profile),
                                             NULL);
    g_free (signal_name);

    /* g_signal_connect_object would auto-disconnect on finalize, but we
     * disconnect explicitly in dispose so the handler can never fire on
     * a half-dead node; a plain connect is fine here. */
    priv->vault_handler = g_signal_connect_swapped (pn_vault_get_default (),
                                                    "changed",
                                                    G_CALLBACK (on_vault_changed),
                                                    self);

    /* Initial resolve goes through an idle: at this point the subclass
     * is mid-init and the deserialiser has not yet pushed the saved id
     * into our property.  Once the main loop spins, the loader has
     * applied properties (which would have hit the notify handler) or
     * left them at default (no notify fired but we still want to follow
     * the type's primary profile). */
    priv->initial_resync_id = g_idle_add (initial_resync_idle, self);
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
     * Fan the cause out through notify_connection_failed so a subclass
     * can emit a failure PnMessage (the developer-facing g_warning is
     * issued inside the helper). */
    notify_connection_failed (self, error ? error->message : NULL);
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

        notify_connection_failed (self, error ? error->message : NULL);
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
    PnWebsocketClass   *klass;
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
        notify_connection_failed (self, "invalid URL");
        schedule_retry (self);
        return;
    }

    /* TLS / upgrade-header seams.  Hook accept-certificate
     * unconditionally so the narrow accept_certificate vfunc has a
     * place to be called from; the default impl returns FALSE so a
     * subclass that overrides nothing still gets system-trust-store
     * behaviour.  Then invoke the broader prepare_message vfunc so a
     * subclass can replace headers, attach cookies, or install its
     * own additional accept-certificate handler in one place.  Self
     * is alive across the handshake via the ref handed to
     * on_connect_done as user_data, so a plain g_signal_connect is
     * safe here. */
    g_signal_connect (msg, "accept-certificate",
                      G_CALLBACK (on_msg_accept_certificate), self);

    klass = PN_WEBSOCKET_GET_CLASS (self);
    if (klass->prepare_message != NULL)
        klass->prepare_message (self, msg);

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

    /* Tear down the profile binding first: a vault::changed firing
     * between here and clear_connection would otherwise try to push a
     * fresh URL into a half-dead node. */
    if (priv->initial_resync_id != 0)
    {
        g_source_remove (priv->initial_resync_id);
        priv->initial_resync_id = 0;
    }
    if (priv->notify_handler != 0)
    {
        g_signal_handler_disconnect (self, priv->notify_handler);
        priv->notify_handler = 0;
    }
    if (priv->vault_handler != 0)
    {
        g_signal_handler_disconnect (pn_vault_get_default (),
                                     priv->vault_handler);
        priv->vault_handler = 0;
    }

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
    g_clear_pointer (&priv->profile_prop, g_free);

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

    klass->is_configured     = default_is_configured;
    klass->connected         = default_connected;
    klass->text_message      = default_text_message;
    klass->profile_resolved  = default_profile_resolved;
    klass->prepare_message   = default_prepare_message;
    klass->accept_certificate = default_accept_certificate;
    klass->connection_failed = default_connection_failed;

    /* Stable palette glyph regardless of instance state. */
    node_class->palette_icon = PN_WS_NORMAL_ICON;
    /* Pin the class label here (not per-instance in init): PnWebSocket is
     * a base class, and pn_node_get_class_name() falls back to this field,
     * so a subclass can override it from its own class_init without the
     * base stomping a per-instance "WebSocket" over the top. */
    node_class->class_name   = "WebSocket";
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

    priv->profile_prop       = NULL;
    priv->notify_handler     = 0;
    priv->vault_handler      = 0;
    priv->initial_resync_id  = 0;

    /* Class label is pinned on PnNodeClass.class_name in class_init (see
     * above) and resolved through pn_node_get_class_name()'s class
     * fallback; do NOT seed it per-instance here or it would shadow a
     * subclass's own label. */
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
