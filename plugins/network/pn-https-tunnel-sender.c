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

#include "pn-https-tunnel-sender.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-vault.h"
#include "pn-network-profiles.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <string.h>

/* Visual states.  Body colour carries the alert; the icon panel is
 * rendered in white on top. */
#define PN_HTTPS_TUNNEL_SENDER_NORMAL_ICON  "\xef\x87\x98"  /* fa-paper-plane U+F1D8 */
#define PN_HTTPS_TUNNEL_SENDER_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* Default POST target.  Points at the loopback on the HTTPS Tunnel Receiver's
 * default port (8443) so dropping a Client and a Server onto a
 * worksheet — or onto two pipnodes on the same machine — talks
 * end-to-end with zero configuration.  Override to a real peer URL
 * once the topology grows past localhost. */
#define PN_HTTPS_TUNNEL_SENDER_DEFAULT_URL  "https://127.0.0.1:8443/"

struct _PnHttpsTunnelSender
{
    PnNode parent_instance;

    /* All configuration is read and written from the main thread
     * (property setters land there, the SoupSession dispatches its
     * callbacks on the default GMainContext), so no mutex is needed. */
    gchar    *url;
    /* Basic-auth identity — prefer the referenced "http-basic" vault
     * profile (auth_profile holds its id, "" follows the primary); the
     * inline username/password are the legacy fallback that keeps pre-v5
     * files working and seeds the one-time profile import. */
    gchar    *auth_profile;
    gchar    *username;
    gchar    *password;
    gboolean  verify_tls;

    /* One-shot post-load migration idle (see PnMqtt); 0 when unscheduled. */
    guint     migrate_idle_id;

    /* Long-lived session reused across every POST so connection,
     * proxy and TLS state survive between requests.  Created in init,
     * destroyed in dispose. */
    SoupSession  *session;

    /* Cancellable handed to every in-flight `soup_session_send_async`
     * so disposal aborts pending requests cleanly instead of letting
     * them complete after the node is gone. */
    GCancellable *cancellable;
};

G_DEFINE_TYPE (PnHttpsTunnelSender, pn_https_tunnel_sender, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_URL,
    PROP_AUTH_PROFILE,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_VERIFY_TLS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the configured (green ✈) and
 *  unconfigured (red ❗) appearance based on whether @configured. */
static void
apply_visual_state (
        PnHttpsTunnelSender *self,
        gboolean       configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        PnColor green = { 0.36, 0.66, 0.36, 1.0 };
        pn_node_set_color (node, &green);
        pn_node_set_icon  (node, PN_HTTPS_TUNNEL_SENDER_NORMAL_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_HTTPS_TUNNEL_SENDER_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Body construction                                                  */
/* ------------------------------------------------------------------ */

/** Render a #PnMessage as a JSON document of the shape #PnHttpsTunnelReceiver
 *  understands: `{"topic": ..., "id": ..., "created": ..., "data": {...}}`.
 *  The result is a freshly-allocated string the caller frees with
 *  g_free(); @out_len receives the byte length. */
static gchar *
build_request_body (PnMessage *msg, gsize *out_len)
{
    JsonBuilder *builder = json_builder_new ();
    JsonGenerator *gen;
    JsonNode    *root;
    const gchar *topic;
    const gchar *id;
    const gchar *created;
    JsonObject  *data;
    JsonNode    *data_node;
    gchar       *result;

    json_builder_begin_object (builder);

    topic = pn_message_get_topic (msg);
    if (topic != NULL)
    {
        json_builder_set_member_name  (builder, "topic");
        json_builder_add_string_value (builder, topic);
    }

    id = pn_message_get_id (msg);
    if (id != NULL)
    {
        json_builder_set_member_name  (builder, "id");
        json_builder_add_string_value (builder, id);
    }

    created = pn_message_get_created (msg);
    if (created != NULL)
    {
        json_builder_set_member_name  (builder, "created");
        json_builder_add_string_value (builder, created);
    }

    /* Always emit a "data" key, even when empty, so the receiver's
     * parser hits the structured branch and does not fall back to
     * the flat-envelope-as-data heuristic. */
    json_builder_set_member_name (builder, "data");
    data = pn_message_get_data (msg);
    if (data != NULL)
    {
        /* Wrap the borrowed JsonObject in a fresh node, ref it via
         * json_node_set_object, and hand it to the builder which
         * takes ownership of the node.  The PnMessage retains its
         * own reference to @data, so freeing the node tree later
         * just drops our extra ref. */
        data_node = json_node_new (JSON_NODE_OBJECT);
        json_node_set_object (data_node, data);
        json_builder_add_value (builder, data_node);
    }
    else
    {
        json_builder_begin_object (builder);
        json_builder_end_object   (builder);
    }

    json_builder_end_object (builder);

    root = json_builder_get_root (builder);
    gen  = json_generator_new ();
    json_generator_set_root (gen, root);
    result = json_generator_to_data (gen, out_len);

    json_node_unref  (root);
    g_object_unref   (gen);
    g_object_unref   (builder);

    return result;
}

/** Resolve the Basic-auth username/password from the referenced
 *  "http-basic" vault profile when one applies, else the legacy inline
 *  fields.  Each out string is freshly allocated; the caller frees. */
static void
resolve_auth (PnHttpsTunnelSender *self,
              gchar **out_user,
              gchar **out_pass)
{
    PnProfile *p = pn_node_get_profile (PN_NODE (self), "auth-profile");

    if (p != NULL)
    {
        if (out_user) *out_user = pn_profile_get_string (p, "username");
        if (out_pass) *out_pass = pn_profile_get_secret (p, "password");
    }
    else
    {
        if (out_user) *out_user = g_strdup (self->username ? self->username : "");
        if (out_pass) *out_pass = g_strdup (self->password ? self->password : "");
    }
}

/** Build the value for an "Authorization: Basic ..." header from the
 *  resolved credentials, or %NULL when either field is empty.
 *  Caller frees with g_free(). */
static gchar *
build_basic_auth_header (PnHttpsTunnelSender *self)
{
    gchar *user = NULL;
    gchar *pass = NULL;
    gchar *creds;
    gchar *encoded;
    gchar *result;

    resolve_auth (self, &user, &pass);

    if (user == NULL || *user == '\0' || pass == NULL || *pass == '\0')
    {
        g_free (user);
        g_free (pass);
        return NULL;
    }

    creds   = g_strdup_printf ("%s:%s", user, pass);
    encoded = g_base64_encode ((const guchar *) creds, strlen (creds));
    result  = g_strdup_printf ("Basic %s", encoded);

    g_free (user);
    g_free (pass);
    g_free (creds);
    g_free (encoded);
    return result;
}

/* ------------------------------------------------------------------ */
/*  TLS verification                                                   */
/* ------------------------------------------------------------------ */

/** Handler for SoupMessage::accept-certificate.  Returning %TRUE here
 *  overrides any verification failure on the peer's certificate, so a
 *  connection to a self-signed server's cert is accepted.  We hook
 *  this only when verify-tls is %FALSE; with verify-tls %TRUE the
 *  default behaviour (the GTlsBackend's normal path) applies. */
static gboolean
on_accept_certificate (
        SoupMessage         *msg,
        GTlsCertificate     *cert,
        GTlsCertificateFlags errors,
        gpointer             user_data)
{
    (void) msg;
    (void) cert;
    (void) errors;
    (void) user_data;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Send                                                               */
/* ------------------------------------------------------------------ */

/* The async POST callback runs after the node may have started to
 * dispose (the cancellable fires on dispose), so it carries a strong
 * node ref of its own to log against safely. */
typedef struct
{
    PnNode      *node;  /* strong ref */
    SoupMessage *msg;   /* borrowed; kept alive by the creation ref */
} SendCtx;

static void
on_send_finished (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    SoupSession  *session = SOUP_SESSION (source);
    SendCtx      *ctx     = user_data;
    SoupMessage  *msg     = ctx->msg;
    GInputStream *stream;
    GError       *error   = NULL;

    /* Sink semantics: we don't propagate the response anywhere, but
     * we still drain the body and check the status so transport
     * failures are logged and a slow server's TCP buffer eventually
     * clears.  An aborted request (cancelled on dispose) lands here
     * with G_IO_ERROR_CANCELLED, which we silently absorb. */
    stream = soup_session_send_finish (session, result, &error);
    if (stream == NULL)
    {
        if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                               G_IO_ERROR_CANCELLED))
            pn_node_log_error (ctx->node, "POST failed: %s", error->message);
        g_clear_error (&error);
    }
    else
    {
        guint status = soup_message_get_status (msg);
        if (status < 200 || status >= 300)
            pn_node_log_warning (ctx->node,
                                 "POST returned HTTP %u", status);
        g_object_unref (stream);
    }

    g_object_unref (msg);
    g_object_unref (ctx->node);
    g_free (ctx);
}

static void
pn_https_tunnel_sender_receive (PnNode *node, PnMessage *message)
{
    PnHttpsTunnelSender    *self    = PN_HTTPS_CLIENT (node);
    SoupMessage      *msg;
    SoupMessageHeaders *headers;
    GBytes           *body_bytes;
    gchar            *body;
    gsize             body_len;
    gchar            *auth;

    /* Skip work entirely while the URL is unset.  The visual marker
     * on the canvas already tells the user; silently dropping keeps
     * the upstream pipeline from blocking on a misconfigured sink. */
    if (self->url == NULL || *self->url == '\0')
        return;

    msg = soup_message_new (SOUP_METHOD_POST, self->url);
    if (msg == NULL)
    {
        pn_node_log_error (node, "Invalid URL '%s'", self->url);
        return;
    }

    /* Hook the verification override before the request goes on the
     * wire.  When verify-tls is TRUE we leave the signal alone and
     * let GTlsBackend run its normal path. */
    if (!self->verify_tls)
        g_signal_connect (msg, "accept-certificate",
                          G_CALLBACK (on_accept_certificate), self);

    /* Preemptive Basic auth: we set the Authorization header
     * directly instead of relying on a SoupAuthManager 401-reissue
     * loop.  The receiver always expects credentials when configured
     * and there is no auth challenge to negotiate against. */
    auth = build_basic_auth_header (self);
    if (auth != NULL)
    {
        headers = soup_message_get_request_headers (msg);
        soup_message_headers_replace (headers, "Authorization", auth);
        g_free (auth);
    }

    body = build_request_body (message, &body_len);
    body_bytes = g_bytes_new_take (body, body_len);
    soup_message_set_request_body_from_bytes (msg, "application/json",
                                              body_bytes);
    g_bytes_unref (body_bytes);

    /* The SoupMessage's creation ref keeps it alive across the async
     * call; on_send_finished releases it (together with the context's
     * node ref).  soup_session_send_async does not retain it for us. */
    SendCtx *ctx = g_new0 (SendCtx, 1);
    ctx->node = g_object_ref (node);
    ctx->msg  = msg;
    soup_session_send_async (self->session,
                             msg,
                             G_PRIORITY_DEFAULT,
                             self->cancellable,
                             on_send_finished,
                             ctx);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_https_tunnel_sender_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_value_set_string (value, self->url);
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    case PROP_USERNAME:
        g_value_set_string (value, self->username);
        break;
    case PROP_PASSWORD:
        g_value_set_string (value, self->password);
        break;
    case PROP_VERIFY_TLS:
        g_value_set_boolean (value, self->verify_tls);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_https_tunnel_sender_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_free (self->url);
        self->url = g_value_dup_string (value);
        apply_visual_state (self, self->url != NULL && *self->url != '\0');
        break;
    case PROP_AUTH_PROFILE:
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (value);
        break;
    case PROP_USERNAME:
        g_free (self->username);
        self->username = g_value_dup_string (value);
        break;
    case PROP_PASSWORD:
        g_free (self->password);
        self->password = g_value_dup_string (value);
        break;
    case PROP_VERIFY_TLS:
        self->verify_tls = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Legacy-credentials migration                                       */
/* ------------------------------------------------------------------ */

/** One-shot, post-load migration of inline Basic-auth credentials into an
 *  http-basic vault profile (see PnMqtt's migration for the rationale). */
static gboolean
migrate_legacy_credentials (gpointer data)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (data);
    const gchar *names[]  = { "username", "password" };
    const gchar *values[2];
    gchar       *id;

    self->migrate_idle_id = 0;

    if (self->auth_profile != NULL && *self->auth_profile != '\0')
        return G_SOURCE_REMOVE;
    if ((self->username == NULL || *self->username == '\0') &&
        (self->password == NULL || *self->password == '\0'))
        return G_SOURCE_REMOVE;

    values[0] = self->username ? self->username : "";
    values[1] = self->password ? self->password : "";

    id = pn_vault_import_inline_profile (PN_NETWORK_PROFILE_HTTP_BASIC,
                                         "HTTP auth",
                                         names, values, G_N_ELEMENTS (names));
    if (id != NULL)
    {
        g_object_set (self, "auth-profile", id, NULL);
        g_free (id);
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_https_tunnel_sender_constructed (GObject *object)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (object);

    G_OBJECT_CLASS (pn_https_tunnel_sender_parent_class)->constructed (object);

    self->migrate_idle_id = g_idle_add (migrate_legacy_credentials, self);
}

static void
pn_https_tunnel_sender_dispose (GObject *object)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (object);

    if (self->migrate_idle_id != 0)
    {
        g_source_remove (self->migrate_idle_id);
        self->migrate_idle_id = 0;
    }

    /* Cancel any in-flight POST so its async callback bails out
     * before it tries to touch the dying node.  Then drop the
     * session so libsoup tears down its connection pool. */
    if (self->cancellable != NULL)
        g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
    g_clear_object (&self->session);

    G_OBJECT_CLASS (pn_https_tunnel_sender_parent_class)->dispose (object);
}

static void
pn_https_tunnel_sender_finalize (GObject *object)
{
    PnHttpsTunnelSender *self = PN_HTTPS_CLIENT (object);

    g_clear_pointer (&self->url,          g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    g_clear_pointer (&self->username,     g_free);
    g_clear_pointer (&self->password,     g_free);

    G_OBJECT_CLASS (pn_https_tunnel_sender_parent_class)->finalize (object);
}

static void
pn_https_tunnel_sender_class_init (PnHttpsTunnelSenderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_https_tunnel_sender_get_property;
    object_class->set_property = pn_https_tunnel_sender_set_property;
    object_class->constructed  = pn_https_tunnel_sender_constructed;
    object_class->dispose      = pn_https_tunnel_sender_dispose;
    object_class->finalize     = pn_https_tunnel_sender_finalize;
    node_class->receive        = pn_https_tunnel_sender_receive;

    node_class->palette_icon   = PN_HTTPS_TUNNEL_SENDER_NORMAL_ICON;
    node_class->class_name     = "HTTPS Tunnel Sender";
    node_class->icon           = PN_HTTPS_TUNNEL_SENDER_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_URL] = g_param_spec_string (
            "url", "URL",
            "Target URL to POST every received message to.  Defaults "
            "to \"https://127.0.0.1:8443/\" so the node pairs with an "
            "HTTPS Tunnel Receiver running on its default port on the same "
            "machine without any further configuration; override with "
            "the peer's address once the topology grows past loopback "
            "(e.g. \"https://peer.example:8443/\").  While empty the "
            "node is marked as needing configuration and silently "
            "drops incoming messages.",
            PN_HTTPS_TUNNEL_SENDER_DEFAULT_URL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_AUTH_PROFILE] = g_param_spec_string (
            "auth-profile", "Auth profile",
            "Id of the http-basic credential profile this node authenticates "
            "with.  Empty follows the primary http-basic profile; pick another "
            "to use different credentials.  The username and password live in "
            "the host vault, not in the workflow file.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_profile_ref (props[PROP_AUTH_PROFILE],
                                   PN_NETWORK_PROFILE_HTTP_BASIC);

    props[PROP_USERNAME] = g_param_spec_string (
            "username", "Username",
            "HTTP Basic auth username.  Sent with every request "
            "(preemptive auth) when both this and password are "
            "non-empty; either field empty disables auth.  Legacy inline "
            "fallback — new configurations keep credentials in an http-basic "
            "vault profile.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PASSWORD] = g_param_spec_string (
            "password", "Password",
            "HTTP Basic auth password; see username.  Tagged secret so it is "
            "never written to the workflow file.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_secret (props[PROP_PASSWORD]);

    props[PROP_VERIFY_TLS] = g_param_spec_boolean (
            "verify-tls", "Verify TLS certificate",
            "When TRUE, validate the peer's certificate against the "
            "system trust store and refuse the connection on any "
            "verification error.  Defaults to FALSE so the node pairs "
            "with the HTTPS Tunnel Receiver's out-of-the-box self-signed cert "
            "without any setup; flip to TRUE when targeting a real "
            "cert.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_https_tunnel_sender_init (PnHttpsTunnelSender *self)
{
    PnNode *node = PN_NODE (self);

    /* Mirror the GParamSpec default so a freshly-instantiated node is
     * immediately able to talk to a same-machine HTTPS Tunnel Receiver on its
     * default port; loading a saved worksheet runs the property setter
     * afterwards, which overrides this with whatever URL the user had
     * configured. */
    self->url           = g_strdup (PN_HTTPS_TUNNEL_SENDER_DEFAULT_URL);
    self->auth_profile  = g_strdup ("");
    self->username      = NULL;
    self->password      = NULL;
    self->verify_tls    = FALSE;
    self->migrate_idle_id = 0;

    self->session     = soup_session_new ();
    self->cancellable = g_cancellable_new ();

    pn_node_set_class_name (node, "HTTPS Tunnel Sender");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* Default URL is non-empty, so the node comes up in the configured
     * (green ✈) state.  The URL setter flips back to red ❗ if the
     * user clears the field. */
    apply_visual_state (self, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnHttpsTunnelSender *
pn_https_tunnel_sender_new (void)
{
    return g_object_new (PN_TYPE_HTTPS_TUNNEL_SENDER, NULL);
}
