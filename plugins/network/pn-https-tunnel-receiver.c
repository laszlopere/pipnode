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

#include "pn-https-tunnel-receiver.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <errno.h>
#include <string.h>
#include <time.h>

/* Visual states.  Body colour carries the alert; the icon panel is
 * rendered in white on top. */
#define PN_HTTPS_TUNNEL_RECEIVER_NORMAL_ICON  "\xef\x88\xb3"  /* fa-server U+F233 */
#define PN_HTTPS_TUNNEL_RECEIVER_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

/* Default listening port.  8443 is the conventional non-privileged
 * HTTPS port; pipnode runs as a regular user so binding 443 is not an
 * option without elevated capabilities. */
#define PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_PORT 8443u

/* The auto-generated self-signed certificate's validity window.  Long
 * enough that a left-running pipnode does not surprise the user with a
 * sudden expiry, short enough that the cert rolls over within a single
 * pipnode major version. */
#define PN_HTTPS_TUNNEL_RECEIVER_CERT_VALIDITY_SECONDS  (3650 * 24 * 60 * 60)  /* ~10 years */

/* Sub-directory under XDG_DATA_HOME (typically ~/.local/share) used
 * for the auto-generated default cert/key pair.  Same directory the
 * pipnode application data lives in; created on demand. */
#define PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_DIR        "pipnode"
#define PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_CERT_FILE  "default-cert.pem"
#define PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_KEY_FILE   "default-key.pem"

struct _PnHttpsTunnelReceiver
{
    PnNode parent_instance;

    /* Configuration.  All accessed and written from the main thread
     * exclusively, so no mutex is needed. */
    guint   port;
    gchar  *username;
    gchar  *password;
    gchar  *cert_path;
    gchar  *key_path;

    /* Live SoupServer + the auth domain it currently holds, both
     * NULL while the node sits idle.  Recreated on every config
     * change through restart_server(). */
    SoupServer       *server;
    SoupAuthDomain   *auth_domain;

    /* Cached self-signed cert: lazily minted the first time the node
     * needs to start without user-supplied PEM files.  Held across
     * restarts so successive starts do not have to regenerate the
     * RSA key (which costs ~100 ms even at 2048 bits). */
    GTlsCertificate  *self_signed;
};

G_DEFINE_TYPE (PnHttpsTunnelReceiver, pn_https_tunnel_receiver, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PORT,
    PROP_USERNAME,
    PROP_PASSWORD,
    PROP_CERT_PATH,
    PROP_KEY_PATH,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void restart_server (PnHttpsTunnelReceiver *self);

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the running (green 🖥) and stopped
 *  (red ❗) appearance based on whether a server is listening. */
static void
apply_visual_state (PnHttpsTunnelReceiver *self)
{
    PnNode  *node    = PN_NODE (self);
    gboolean running = (self->server != NULL);

    if (running)
    {
        PnColor green = { 0.36, 0.66, 0.36, 1.0 };
        pn_node_set_color (node, &green);
        pn_node_set_icon  (node, PN_HTTPS_TUNNEL_RECEIVER_NORMAL_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_HTTPS_TUNNEL_RECEIVER_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Self-signed certificate generation                                 */
/*                                                                     */
/*  GIO's GTlsCertificate has no built-in "make me one" constructor,   */
/*  so we use GnuTLS to mint a fresh RSA 2048 keypair, wrap it in a    */
/*  minimal X.509 v3 self-signed cert (CN=pipnode), serialise both    */
/*  parts as PEM, and feed the concatenation to                        */
/*  g_tls_certificate_new_from_pem(), which accepts a single blob     */
/*  containing both certificate and private key.  The whole thing      */
/*  runs once per node lifetime — held in @self_signed across server   */
/*  restarts so successive port changes don't burn ~100 ms each on RSA */
/*  key generation.                                                    */
/* ------------------------------------------------------------------ */

/** Compute the XDG default path for the auto-generated certificate.
 *  Caller frees with g_free(). */
static gchar *
default_cert_path (void)
{
    return g_build_filename (g_get_user_data_dir (),
                             PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_DIR,
                             PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_CERT_FILE, NULL);
}

/** Companion to default_cert_path(): the matching private-key path. */
static gchar *
default_key_path (void)
{
    return g_build_filename (g_get_user_data_dir (),
                             PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_DIR,
                             PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_KEY_FILE, NULL);
}

/** Mint a fresh self-signed X.509 certificate.  When @out_cert_pem
 *  and @out_key_pem are non-%NULL the freshly-allocated PEM blobs are
 *  handed back to the caller (who frees them with g_free()) so they
 *  can be persisted to disk; pass %NULL on either to skip. */
static GTlsCertificate *
mint_self_signed_certificate (gchar  **out_cert_pem,
                              gchar  **out_key_pem,
                              GError **error)
{
    gnutls_x509_privkey_t key  = NULL;
    gnutls_x509_crt_t     cert = NULL;
    gnutls_datum_t        cert_pem = { NULL, 0 };
    gnutls_datum_t        key_pem  = { NULL, 0 };
    GTlsCertificate      *result   = NULL;
    GError               *local    = NULL;
    gchar                *blob     = NULL;
    gsize                 blob_len = 0;
    int                   rc;
    time_t                now;
    guint32               serial_be;

    if (out_cert_pem != NULL)
        *out_cert_pem = NULL;
    if (out_key_pem != NULL)
        *out_key_pem = NULL;

    rc = gnutls_x509_privkey_init (&key);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_privkey_generate (key, GNUTLS_PK_RSA, 2048, 0);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_init (&cert);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_set_key (cert, key);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_set_version (cert, 3);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    /* A short, fixed-shape DN.  Using the same string for subject
     * and issuer is what makes the certificate self-signed. */
    rc = gnutls_x509_crt_set_dn_by_oid (cert,
            GNUTLS_OID_X520_COMMON_NAME, 0, "pipnode", 7);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_set_issuer_dn_by_oid (cert,
            GNUTLS_OID_X520_COMMON_NAME, 0, "pipnode", 7);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    /* Serial: a 4-byte big-endian counter derived from the current
     * time.  Need not be unique across cert generations — any client
     * pinning this certificate already has its full SPKI to compare. */
    now       = time (NULL);
    serial_be = g_htonl ((guint32) now);
    rc = gnutls_x509_crt_set_serial (cert, &serial_be, sizeof (serial_be));
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_set_activation_time (cert, now);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_set_expiration_time (cert,
            now + PN_HTTPS_TUNNEL_RECEIVER_CERT_VALIDITY_SECONDS);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_sign2 (cert, cert, key, GNUTLS_DIG_SHA256, 0);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_crt_export2 (cert, GNUTLS_X509_FMT_PEM, &cert_pem);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    rc = gnutls_x509_privkey_export2 (key, GNUTLS_X509_FMT_PEM, &key_pem);
    if (rc != GNUTLS_E_SUCCESS)
        goto fail_gnutls;

    /* g_tls_certificate_new_from_pem accepts a single PEM blob that
     * contains both the certificate and the private key. */
    blob_len = cert_pem.size + key_pem.size;
    blob     = g_malloc (blob_len + 1);
    memcpy (blob, cert_pem.data, cert_pem.size);
    memcpy (blob + cert_pem.size, key_pem.data, key_pem.size);
    blob[blob_len] = '\0';

    result = g_tls_certificate_new_from_pem (blob, (gssize) blob_len, &local);
    if (result == NULL)
    {
        g_propagate_error (error, local);
        goto out;
    }

    /* Hand the freshly-generated PEM data back to the caller so it can
     * persist the pair to disk.  We strdup rather than steal the
     * gnutls_datum_t buffers because those go through gnutls_free in
     * the cleanup block below. */
    if (out_cert_pem != NULL)
        *out_cert_pem = g_strndup ((const gchar *) cert_pem.data, cert_pem.size);
    if (out_key_pem != NULL)
        *out_key_pem = g_strndup ((const gchar *) key_pem.data, key_pem.size);

    goto out;

fail_gnutls:
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "GnuTLS error while generating self-signed certificate: %s",
                 gnutls_strerror (rc));

out:
    if (cert_pem.data != NULL)
        gnutls_free (cert_pem.data);
    if (key_pem.data != NULL)
        gnutls_free (key_pem.data);
    g_free (blob);
    if (cert != NULL)
        gnutls_x509_crt_deinit (cert);
    if (key != NULL)
        gnutls_x509_privkey_deinit (key);

    return result;
}

/** Mint a self-signed cert and persist its PEM blobs to @cert_path
 *  and @key_path, creating parent directories as needed.  Returns the
 *  loaded #GTlsCertificate on success; on a write failure the cert is
 *  still returned (so the server comes up with an ephemeral identity
 *  this run) but a warning is logged so the operator sees why the
 *  files did not land on disk. */
static GTlsCertificate *
mint_and_persist (const gchar *cert_path,
                  const gchar *key_path,
                  GError     **error)
{
    GTlsCertificate *cert;
    gchar           *cert_pem = NULL;
    gchar           *key_pem  = NULL;
    gchar           *dir;
    GError          *werr     = NULL;

    cert = mint_self_signed_certificate (&cert_pem, &key_pem, error);
    if (cert == NULL)
        return NULL;

    /* Ensure both parent directories exist.  In practice they are the
     * same dir, but g_mkdir_with_parents is cheap enough that doing it
     * twice does not warrant deduplication. */
    dir = g_path_get_dirname (cert_path);
    if (g_mkdir_with_parents (dir, 0700) != 0)
        g_warning ("pn-https-tunnel-receiver: failed to create '%s': %s",
                   dir, g_strerror (errno));
    g_free (dir);

    dir = g_path_get_dirname (key_path);
    if (g_mkdir_with_parents (dir, 0700) != 0)
        g_warning ("pn-https-tunnel-receiver: failed to create '%s': %s",
                   dir, g_strerror (errno));
    g_free (dir);

    /* The cert is public — world-readable is fine.  The key is not;
     * chmod 0600 right after the write closes the brief window where
     * g_file_set_contents leaves the file at the umask default. */
    if (!g_file_set_contents (cert_path, cert_pem, -1, &werr))
    {
        g_warning ("pn-https-tunnel-receiver: failed to persist certificate to "
                   "'%s': %s — server will use an ephemeral cert this "
                   "run", cert_path, werr->message);
        g_clear_error (&werr);
    }

    if (!g_file_set_contents (key_path, key_pem, -1, &werr))
    {
        g_warning ("pn-https-tunnel-receiver: failed to persist private key to "
                   "'%s': %s — server will use an ephemeral cert this "
                   "run", key_path, werr->message);
        g_clear_error (&werr);
    }
    else if (g_chmod (key_path, 0600) != 0)
    {
        g_warning ("pn-https-tunnel-receiver: failed to chmod 0600 on '%s': %s",
                   key_path, g_strerror (errno));
    }

    g_free (cert_pem);
    g_free (key_pem);
    return cert;
}

/** Resolve the certificate the next server start should use.  Returns
 *  a freshly-referenced #GTlsCertificate the caller owns, or %NULL
 *  with @error set on failure. */
static GTlsCertificate *
load_certificate (PnHttpsTunnelReceiver *self, GError **error)
{
    /* When both paths are set (the default — see pn_https_tunnel_receiver_init
     * — points them at XDG_DATA_HOME so even a never-configured node
     * picks up the lazily-persisted self-signed pair) we try to load
     * from disk first.  Missing files trigger a one-shot
     * mint-and-persist, so the second start of a freshly-installed
     * pipnode skips the ~100 ms RSA key generation step.  A typo in a
     * user-supplied path that points at an existing-but-unreadable
     * file still surfaces as a load error rather than as a silent
     * identity swap. */
    if (self->cert_path != NULL && *self->cert_path != '\0' &&
        self->key_path  != NULL && *self->key_path  != '\0')
    {
        if (g_file_test (self->cert_path, G_FILE_TEST_EXISTS) &&
            g_file_test (self->key_path,  G_FILE_TEST_EXISTS))
        {
            return g_tls_certificate_new_from_files (self->cert_path,
                                                     self->key_path,
                                                     error);
        }

        return mint_and_persist (self->cert_path, self->key_path, error);
    }

    /* Both paths empty — happens with older worksheets saved before
     * the XDG defaults landed, or when the user explicitly clears the
     * fields.  Mint and cache the self-signed cert lazily; held for
     * the rest of the node's lifetime so changing the port doesn't
     * regenerate a fresh RSA key on every restart. */
    if (self->self_signed == NULL)
        self->self_signed = mint_self_signed_certificate (NULL, NULL, error);

    return (self->self_signed != NULL)
               ? g_object_ref (self->self_signed)
               : NULL;
}

/* ------------------------------------------------------------------ */
/*  Auth                                                               */
/* ------------------------------------------------------------------ */

static gboolean
basic_auth_callback (
        SoupAuthDomain    *domain,
        SoupServerMessage *msg,
        const char        *username,
        const char        *password,
        gpointer           user_data)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (user_data);

    (void) domain;
    (void) msg;

    /* Constant-time comparison would be nicer here, but the credential
     * is exchanged in clear (well, TLS-protected) on every request so
     * a timing oracle would only narrow the password between attempts
     * a script could just try in bulk anyway.  Keeping it simple. */
    return self->username != NULL && self->password != NULL
        && username != NULL && password != NULL
        && g_strcmp0 (username, self->username) == 0
        && g_strcmp0 (password, self->password) == 0;
}

/* ------------------------------------------------------------------ */
/*  Request handler                                                    */
/* ------------------------------------------------------------------ */

/** Pull the request body out of a SoupServerMessage as a NUL-terminated
 *  C string.  Returns %NULL when the body is empty.  Caller frees with
 *  g_free(). */
static gchar *
dup_request_body (SoupServerMessage *msg)
{
    SoupMessageBody *body = soup_server_message_get_request_body (msg);
    GBytes          *bytes;
    gconstpointer    data;
    gsize            len;
    gchar           *copy;

    if (body == NULL)
        return NULL;

    bytes = soup_message_body_flatten (body);
    if (bytes == NULL)
        return NULL;

    data = g_bytes_get_data (bytes, &len);
    if (len == 0)
    {
        g_bytes_unref (bytes);
        return NULL;
    }

    copy = g_strndup ((const gchar *) data, len);
    g_bytes_unref (bytes);
    return copy;
}

/** Build a #PnMessage from a JSON object: copy `topic` (and `id`,
 *  `created` if present) onto the envelope and the entire `data`
 *  member onto the data bag.  When the JSON has no `data` member the
 *  whole top-level object is used as the data bag instead, so a
 *  client can POST a flat `{"foo": 1}` and have it land as
 *  `data.foo`. */
static PnMessage *
build_message_from_json (PnNode *source, JsonObject *root)
{
    PnMessage   *msg;
    const gchar *topic = NULL;
    JsonNode    *data_node;
    JsonObject  *data_obj = NULL;

    if (json_object_has_member (root, "topic"))
    {
        JsonNode *n = json_object_get_member (root, "topic");
        if (JSON_NODE_HOLDS_VALUE (n) &&
            json_node_get_value_type (n) == G_TYPE_STRING)
            topic = json_node_get_string (n);
    }

    msg = pn_message_new (source, topic);

    if (json_object_has_member (root, "id"))
    {
        JsonNode *n = json_object_get_member (root, "id");
        if (JSON_NODE_HOLDS_VALUE (n) &&
            json_node_get_value_type (n) == G_TYPE_STRING)
            pn_message_set_id (msg, json_node_get_string (n));
    }

    if (json_object_has_member (root, "created"))
    {
        JsonNode *n = json_object_get_member (root, "created");
        if (JSON_NODE_HOLDS_VALUE (n) &&
            json_node_get_value_type (n) == G_TYPE_STRING)
            pn_message_set_created (msg, json_node_get_string (n));
    }

    if (json_object_has_member (root, "data"))
    {
        data_node = json_object_get_member (root, "data");
        if (JSON_NODE_HOLDS_OBJECT (data_node))
            data_obj = json_node_get_object (data_node);
    }
    else
    {
        /* No explicit data wrapper: treat the whole envelope (minus
         * the standard fields we already consumed) as the data bag.
         * Easier for ad-hoc clients that just want to POST a JSON
         * blob without learning the envelope shape. */
        data_obj = root;
    }

    if (data_obj != NULL)
    {
        JsonObject *dst = pn_message_get_data (msg);
        GList      *members;
        GList      *iter;

        members = json_object_get_members (data_obj);
        for (iter = members; iter != NULL; iter = iter->next)
        {
            const gchar *name = iter->data;

            /* When falling back to using the envelope as data, skip
             * the fields we already mapped onto the envelope itself
             * so they do not also appear inside `data`. */
            if (data_obj == root &&
                (g_strcmp0 (name, "topic")   == 0 ||
                 g_strcmp0 (name, "id")      == 0 ||
                 g_strcmp0 (name, "created") == 0))
                continue;

            json_object_set_member (dst, name,
                json_node_copy (json_object_get_member (data_obj, name)));
        }
        g_list_free (members);
    }

    return msg;
}

static void
on_request (
        SoupServer        *server,
        SoupServerMessage *msg,
        const char        *path,
        GHashTable        *query,
        gpointer           user_data)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (user_data);
    const char    *method;
    gchar         *body;
    JsonParser    *parser;
    JsonNode      *root_node;
    JsonObject    *root_obj;
    PnMessage     *out;
    GError        *error = NULL;

    (void) server;
    (void) path;
    (void) query;

    method = soup_server_message_get_method (msg);

    /* Trivial liveness probe: GET / returns a one-line plain-text
     * banner so an operator can curl the endpoint to confirm the
     * server is reachable without having to construct a JSON body. */
    if (g_strcmp0 (method, "GET") == 0)
    {
        const char *banner = "pipnode HTTPS server\n";
        soup_server_message_set_response (msg, "text/plain",
            SOUP_MEMORY_STATIC, banner, strlen (banner));
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        return;
    }

    if (g_strcmp0 (method, "POST") != 0)
    {
        soup_server_message_set_status (msg,
            SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return;
    }

    body = dup_request_body (msg);
    if (body == NULL)
    {
        soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST,
                                        "empty body");
        return;
    }

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, body, -1, &error))
    {
        gchar *resp = g_strdup_printf ("invalid JSON: %s",
                                       error ? error->message : "(unknown)");
        soup_server_message_set_response (msg, "text/plain",
            SOUP_MEMORY_TAKE, resp, strlen (resp));
        soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
        g_clear_error (&error);
        g_object_unref (parser);
        g_free (body);
        return;
    }

    root_node = json_parser_get_root (parser);
    if (root_node == NULL || !JSON_NODE_HOLDS_OBJECT (root_node))
    {
        soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST,
                                        "JSON root must be an object");
        g_object_unref (parser);
        g_free (body);
        return;
    }

    root_obj = json_node_get_object (root_node);

    /* Build and emit synchronously: the SoupServer dispatches its
     * callbacks on the default GMainContext, so we are already on
     * the same thread the rest of pipnode runs on and can talk to
     * the node graph directly. */
    out = build_message_from_json (PN_NODE (self), root_obj);
    pn_node_emit_message (PN_NODE (self), out);
    g_object_unref (out);

    soup_server_message_set_response (msg, "application/json",
        SOUP_MEMORY_STATIC, "{\"ok\":true}\n", 12);
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);

    g_object_unref (parser);
    g_free (body);
}

/* ------------------------------------------------------------------ */
/*  Server lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
stop_server (PnHttpsTunnelReceiver *self)
{
    if (self->server == NULL)
        return;

    soup_server_disconnect (self->server);
    g_clear_object (&self->server);
    g_clear_object (&self->auth_domain);
}

/** Tear down any running server and start a fresh one with the
 *  current configuration.  Called from every property setter that
 *  affects the wire (port, credentials, certificate paths). */
static void
restart_server (PnHttpsTunnelReceiver *self)
{
    GTlsCertificate *cert;
    GError          *error = NULL;
    gboolean         have_basic_auth;

    stop_server (self);

    if (self->port == 0)
    {
        apply_visual_state (self);
        return;
    }

    cert = load_certificate (self, &error);
    if (cert == NULL)
    {
        g_warning ("pn-https-tunnel-receiver: certificate load failed: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        apply_visual_state (self);
        return;
    }

    self->server = soup_server_new ("tls-certificate", cert, NULL);
    g_object_unref (cert);

    have_basic_auth = self->username != NULL && *self->username != '\0'
                   && self->password != NULL && *self->password != '\0';

    if (have_basic_auth)
    {
        self->auth_domain = soup_auth_domain_basic_new (
                "realm", "pipnode",
                NULL);
        soup_auth_domain_add_path (self->auth_domain, "/");
        soup_auth_domain_basic_set_auth_callback (self->auth_domain,
                basic_auth_callback, self, NULL);
        soup_server_add_auth_domain (self->server, self->auth_domain);
    }

    soup_server_add_handler (self->server, NULL, on_request, self, NULL);

    if (!soup_server_listen_all (self->server, self->port,
                                 SOUP_SERVER_LISTEN_HTTPS, &error))
    {
        g_warning ("pn-https-tunnel-receiver: failed to listen on port %u: %s",
                   self->port, error ? error->message : "(unknown)");
        g_clear_error (&error);
        stop_server (self);
    }

    apply_visual_state (self);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_https_tunnel_receiver_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (object);

    switch (prop_id)
    {
    case PROP_PORT:
        g_value_set_uint (value, self->port);
        break;
    case PROP_USERNAME:
        g_value_set_string (value, self->username);
        break;
    case PROP_PASSWORD:
        g_value_set_string (value, self->password);
        break;
    case PROP_CERT_PATH:
        g_value_set_string (value, self->cert_path);
        break;
    case PROP_KEY_PATH:
        g_value_set_string (value, self->key_path);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_https_tunnel_receiver_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (object);

    switch (prop_id)
    {
    case PROP_PORT:
        self->port = g_value_get_uint (value);
        restart_server (self);
        break;
    case PROP_USERNAME:
        g_free (self->username);
        self->username = g_value_dup_string (value);
        restart_server (self);
        break;
    case PROP_PASSWORD:
        g_free (self->password);
        self->password = g_value_dup_string (value);
        restart_server (self);
        break;
    case PROP_CERT_PATH:
        g_free (self->cert_path);
        self->cert_path = g_value_dup_string (value);
        restart_server (self);
        break;
    case PROP_KEY_PATH:
        g_free (self->key_path);
        self->key_path = g_value_dup_string (value);
        restart_server (self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_https_tunnel_receiver_dispose (GObject *object)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (object);

    stop_server (self);
    g_clear_object (&self->self_signed);

    G_OBJECT_CLASS (pn_https_tunnel_receiver_parent_class)->dispose (object);
}

static void
pn_https_tunnel_receiver_finalize (GObject *object)
{
    PnHttpsTunnelReceiver *self = PN_HTTPS_SERVER (object);

    g_clear_pointer (&self->username,  g_free);
    g_clear_pointer (&self->password,  g_free);
    g_clear_pointer (&self->cert_path, g_free);
    g_clear_pointer (&self->key_path,  g_free);

    G_OBJECT_CLASS (pn_https_tunnel_receiver_parent_class)->finalize (object);
}

static void
pn_https_tunnel_receiver_class_init (PnHttpsTunnelReceiverClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_https_tunnel_receiver_get_property;
    object_class->set_property = pn_https_tunnel_receiver_set_property;
    object_class->dispose      = pn_https_tunnel_receiver_dispose;
    object_class->finalize     = pn_https_tunnel_receiver_finalize;

    node_class->palette_icon   = PN_HTTPS_TUNNEL_RECEIVER_NORMAL_ICON;
    node_class->class_name     = "HTTPS Tunnel Receiver";
    node_class->icon           = PN_HTTPS_TUNNEL_RECEIVER_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.36, 0.66, 0.36, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_PORT] = g_param_spec_uint (
            "port", "Port",
            "TCP port to listen on (0 disables the server). "
            "Defaults to 8443; pipnode runs unprivileged so binding "
            "the standard 443 requires root or a CAP_NET_BIND_SERVICE "
            "capability set on the binary.",
            0u, 65535u, PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_PORT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_USERNAME] = g_param_spec_string (
            "username", "Username",
            "HTTP Basic auth username; leave empty (along with "
            "password) to leave the endpoint open.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PASSWORD] = g_param_spec_string (
            "password", "Password",
            "HTTP Basic auth password; leave empty (along with "
            "username) to leave the endpoint open.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_CERT_PATH] = g_param_spec_string (
            "cert-path", "Certificate path",
            "Path to a PEM-encoded TLS certificate.  Defaults to "
            "`$XDG_DATA_HOME/pipnode/default-cert.pem` (typically "
            "`~/.local/share/pipnode/default-cert.pem`); on the first "
            "start the server mints a self-signed `CN=pipnode` cert "
            "there and reuses it on every subsequent run, so the node "
            "works out of the box without any PKI setup.  Point at a "
            "real cert (e.g. a Let's Encrypt one) to swap in a "
            "trusted identity.  Clearing both this and key-path "
            "falls back to an ephemeral, in-memory self-signed cert.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_KEY_PATH] = g_param_spec_string (
            "key-path", "Private key path",
            "Path to the PEM-encoded private key matching cert-path. "
            "Defaults to `$XDG_DATA_HOME/pipnode/default-key.pem`; see "
            "cert-path for the auto-generation behaviour.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_https_tunnel_receiver_init (PnHttpsTunnelReceiver *self)
{
    PnNode *node = PN_NODE (self);

    self->port        = PN_HTTPS_TUNNEL_RECEIVER_DEFAULT_PORT;
    self->username    = NULL;
    self->password    = NULL;
    /* Default the cert/key fields to real, user-writable paths under
     * XDG_DATA_HOME so the configuration dialog shows meaningful file
     * locations from the very first instantiation, instead of empty
     * entries that suggest the server is broken until the user fills
     * them in.  The PEM files are created lazily on first start
     * through mint_and_persist(); on subsequent runs they are loaded
     * straight from disk, so the server picks up the same identity
     * across restarts.  Loading a saved worksheet replays the
     * property setters and overrides these with whatever the user
     * had configured. */
    self->cert_path   = default_cert_path ();
    self->key_path    = default_key_path ();
    self->server      = NULL;
    self->auth_domain = NULL;
    self->self_signed = NULL;

    pn_node_set_class_name (node, "HTTPS Tunnel Receiver");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Start the server as soon as the instance comes up — the default
     * port + auto-generated cert combination is enough to be useful
     * without any further configuration. */
    restart_server (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnHttpsTunnelReceiver *
pn_https_tunnel_receiver_new (void)
{
    return g_object_new (PN_TYPE_HTTPS_TUNNEL_RECEIVER, NULL);
}
