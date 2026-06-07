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

#include "pn-ollama.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

/* fa-microchip U+F2DB.  FontAwesome 5 added fa-robot (U+F544) which
 * would have been a more on-the-nose fit, but the bundled icon font
 * on most distros is still FA 4.7 and renders unknown glyphs as the
 * "tofu" missing-character box; fa-microchip is the closest "AI /
 * compute" symbol present in FA 4. */
#define PN_OLLAMA_ICON         "\xef\x8b\x9b"

/* Stored default is the empty string so the settings dialog can show
 * the local computer's name as a gray placeholder hint (consistent
 * with the host-monitoring nodes); the empty value is then resolved
 * to the literal "localhost" inside build_endpoint() because the URL
 * goes through libsoup, which needs a name that always resolves to
 * the loopback interface regardless of how the host's mDNS / DNS is
 * configured -- g_get_host_name() does not satisfy that on every
 * distro. */
#define PN_OLLAMA_DEFAULT_HOSTNAME   ""
#define PN_OLLAMA_HTTP_LOCAL_FALLBACK "localhost"
#define PN_OLLAMA_DEFAULT_PORT       11434
#define PN_OLLAMA_DEFAULT_KEEP_ALIVE "5m"

struct _PnOllama
{
    PnNode parent_instance;

    gchar *hostname;
    gint   port;
    gchar *model;

    /* Forwarded to Ollama as the `keep_alive` request field.  Accepts
     * a duration string ("5m", "1h", "30s"), `0` to unload immediately
     * after the call, or `-1` to keep the model resident forever. */
    gchar *keep_alive;

    /* Text concatenated around the incoming `data.output` before the
     * combined string is sent to the model -- the place to put the
     * system / role framing the model expects. */
    gchar *prefix;
    gchar *suffix;

    /* libsoup-3 session reused across requests; Soup's internal
     * connection cache plus the server-side `keep_alive` window keep
     * the model resident between calls when both line up. */
    SoupSession  *session;
    GCancellable *cancellable;
};

G_DEFINE_TYPE (PnOllama, pn_ollama, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_PORT,
    PROP_MODEL,
    PROP_KEEP_ALIVE,
    PROP_PREFIX,
    PROP_SUFFIX,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (
        PnOllama *self,
        gboolean  configured)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.36, 0.72, 1.0 };

    /* Keep the healthy indigo identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &indigo);
    pn_node_set_icon      (node, PN_OLLAMA_ICON);
    pn_node_set_has_error (node, !configured);
}

static void
refresh_visual_state (PnOllama *self)
{
    /* An empty hostname is fine -- build_endpoint() resolves it to
     * the loopback fallback ("localhost") because the placeholder
     * hint in the dialog already tells the user what an empty value
     * means.  Only the model has to be filled in for the node to be
     * runnable; that one really is required because Ollama's API
     * needs the model name in every request. */
    gboolean ok = self->model != NULL && *self->model != '\0';
    apply_visual_state (self, ok);
}

/* ------------------------------------------------------------------ */
/*  URL helpers                                                        */
/* ------------------------------------------------------------------ */

static gchar *
build_endpoint (
        PnOllama    *self,
        const gchar *path)
{
    const gchar *host = (self->hostname && *self->hostname)
                            ? self->hostname
                            : PN_OLLAMA_HTTP_LOCAL_FALLBACK;
    return g_strdup_printf ("http://%s:%d%s", host, self->port, path);
}

/* ------------------------------------------------------------------ */
/*  Receive: POST /api/generate, await the JSON reply, emit            */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnOllama    *self;
    SoupMessage *msg;
} GenerateCtx;

static void
generate_ctx_free (GenerateCtx *ctx)
{
    g_clear_object (&ctx->msg);
    g_clear_object (&ctx->self);
    g_free (ctx);
}

/** Display label for a node: instance name if set, otherwise the
 *  class name -- same shape pn-debug.c uses for `from_long_name`. */
static const gchar *
node_display_name (PnNode *node)
{
    const gchar *n;

    if (node == NULL)
        return NULL;

    n = pn_node_get_name (node);
    if (n != NULL && *n != '\0')
        return n;

    return pn_node_get_class_name (node);
}

/** Extract the `.response` string from a non-streaming Ollama reply.
 *  Returns a freshly-allocated string the caller must free; %NULL on
 *  parse failure (with @error_out set when non-NULL). */
static gchar *
parse_generate_reply (
        const gchar  *body,
        gsize         len,
        gchar       **error_out)
{
    JsonParser *parser = json_parser_new ();
    GError     *error  = NULL;
    JsonObject *root;
    const gchar *response;
    gchar      *out = NULL;

    if (!json_parser_load_from_data (parser, body, (gssize) len, &error))
    {
        if (error_out)
            *error_out = g_strdup (error ? error->message : "parse error");
        g_clear_error (&error);
        g_object_unref (parser);
        return NULL;
    }

    /* json-glib parses an empty / whitespace-only body as success with a
     * NULL root, and json_node_get_object() g_critical()s on a NULL or
     * non-object node -- noisy and invisible under the desktop launcher.
     * Check the node first so a blank or malformed-shape reply degrades
     * to a clean parse error instead. */
    {
        JsonNode *root_node = json_parser_get_root (parser);
        if (root_node == NULL || !JSON_NODE_HOLDS_OBJECT (root_node))
        {
            if (error_out)
                *error_out = g_strdup ("response root is not an object");
            g_object_unref (parser);
            return NULL;
        }
        root = json_node_get_object (root_node);
    }

    /* Ollama surfaces request-level errors as `{"error": "..."}` with
     * a 2xx status, so we have to inspect the body before declaring
     * success. */
    if (json_object_has_member (root, "error"))
    {
        if (error_out)
            *error_out = g_strdup (json_object_get_string_member (root,
                                                                  "error"));
        g_object_unref (parser);
        return NULL;
    }

    response = json_object_has_member (root, "response")
                   ? json_object_get_string_member (root, "response")
                   : NULL;
    out = g_strdup (response ? response : "");

    g_object_unref (parser);
    return out;
}

static void
on_generate_done (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    SoupSession *session = SOUP_SESSION (source);
    GenerateCtx *ctx     = user_data;
    GError      *error   = NULL;
    GBytes      *bytes;
    gsize        len     = 0;
    const gchar *body;
    gchar       *response;
    gchar       *parse_err = NULL;
    guint        status;

    /* The LLM request has returned (success, HTTP error, parse failure
     * or cancellation all land here exactly once): close the processing
     * glow opened in pn_ollama_receive() before any early return. */
    pn_node_processing_end (PN_NODE (ctx->self));

    bytes = soup_session_send_and_read_finish (session, result, &error);
    if (bytes == NULL)
    {
        if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                               G_IO_ERROR_CANCELLED))
            pn_node_log_error (PN_NODE (ctx->self),
                               "POST failed: %s", error->message);
        g_clear_error (&error);
        generate_ctx_free (ctx);
        return;
    }

    status = soup_message_get_status (ctx->msg);
    body   = g_bytes_get_data (bytes, &len);

    if (status < 200 || status >= 300)
    {
        GUri  *uri = soup_message_get_uri (ctx->msg);
        gchar *uri_str = uri ? g_uri_to_string (uri) : NULL;
        pn_node_log_error (PN_NODE (ctx->self), "HTTP %u from %s",
                           status, uri_str ? uri_str : "(unknown)");
        g_free (uri_str);
        g_bytes_unref (bytes);
        generate_ctx_free (ctx);
        return;
    }

    response = parse_generate_reply (body, len, &parse_err);
    g_bytes_unref (bytes);

    if (response == NULL)
    {
        pn_node_log_error (PN_NODE (ctx->self),
                           "Could not parse the model reply: %s",
                           parse_err ? parse_err : "(unknown)");
        g_free (parse_err);
        generate_ctx_free (ctx);
        return;
    }

    /* Emit a fresh message rather than mutating the incoming one: the
     * topic / id / created envelope is owned by this node, and the
     * data bag shape (output + success + from_long_name) is the
     * shared "chat-style" contract PnChat consumes via
     * `sender-path = data/from_long_name`.  Passing NULL for the
     * topic lets pn_message_new resolve the node's own topic
     * template, same as every other source-style emitter. */
    {
        PnNode      *node = PN_NODE (ctx->self);
        const gchar *who  = node_display_name (node);
        PnMessage   *out  = pn_message_new (node, NULL);

        pn_message_set_string  (out, "output",         response);
        pn_message_set_boolean (out, "success",        TRUE);
        pn_message_set_string  (out, "from_long_name", who ? who : "");

        pn_node_emit_message (node, out);
        g_object_unref (out);
    }

    g_free (response);
    generate_ctx_free (ctx);
}

static gchar *
build_generate_body (
        PnOllama    *self,
        const gchar *prompt,
        gsize       *len_out)
{
    JsonBuilder *b     = json_builder_new ();
    JsonGenerator *g   = json_generator_new ();
    JsonNode    *root;
    gchar       *out;
    gsize        len;

    json_builder_begin_object (b);

    json_builder_set_member_name (b, "model");
    json_builder_add_string_value (b, self->model ? self->model : "");

    json_builder_set_member_name (b, "prompt");
    json_builder_add_string_value (b, prompt ? prompt : "");

    json_builder_set_member_name (b, "stream");
    json_builder_add_boolean_value (b, FALSE);

    if (self->keep_alive && *self->keep_alive)
    {
        json_builder_set_member_name (b, "keep_alive");
        json_builder_add_string_value (b, self->keep_alive);
    }

    json_builder_end_object (b);

    root = json_builder_get_root (b);
    json_generator_set_root (g, root);
    out = json_generator_to_data (g, &len);

    json_node_unref (root);
    g_object_unref (g);
    g_object_unref (b);

    if (len_out)
        *len_out = len;
    return out;
}

static void
pn_ollama_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnOllama   *self = PN_OLLAMA (node);
    JsonObject *data;
    const gchar *input = "";
    gchar      *prompt;
    gchar      *body;
    gsize       body_len;
    GBytes     *body_bytes;
    SoupMessage *msg;
    gchar      *url;
    GenerateCtx *ctx;

    /* Empty hostname is the intended default (build_endpoint resolves it to
     * localhost, and refresh_visual_state shows the node as ready with only a
     * model set), so guard on the model alone -- matching that visual state. */
    if (self->model == NULL || *self->model == '\0')
        return;

    data = pn_message_get_data (message);
    if (data != NULL && json_object_has_member (data, "output"))
    {
        JsonNode *n = json_object_get_member (data, "output");
        if (JSON_NODE_HOLDS_VALUE (n)
            && json_node_get_value_type (n) == G_TYPE_STRING)
            input = json_node_get_string (n);
    }

    prompt = g_strconcat (self->prefix ? self->prefix : "",
                          input,
                          self->suffix ? self->suffix : "",
                          NULL);

    body = build_generate_body (self, prompt, &body_len);
    g_free (prompt);

    url = build_endpoint (self, "/api/generate");
    msg = soup_message_new (SOUP_METHOD_POST, url);
    g_free (url);
    if (msg == NULL)
    {
        pn_node_log_error (PN_NODE (self), "Invalid endpoint URL.");
        g_free (body);
        return;
    }

    body_bytes = g_bytes_new_take (body, body_len);
    soup_message_set_request_body_from_bytes (msg, "application/json",
                                              body_bytes);
    g_bytes_unref (body_bytes);

    ctx = g_new0 (GenerateCtx, 1);
    ctx->self    = g_object_ref (self);
    ctx->msg     = g_object_ref (msg);

    /* Light the processing glow across the whole LLM round-trip; the
     * synchronous receive() wrap only covers building the request.
     * Balanced by the pn_node_processing_end() in on_generate_done(). */
    pn_node_processing_begin (PN_NODE (self));

    soup_session_send_and_read_async (self->session,
                                      msg,
                                      G_PRIORITY_DEFAULT,
                                      self->cancellable,
                                      on_generate_done,
                                      ctx);
    g_object_unref (msg);
}

/* The /api/tags model enumeration (pn_ollama_list_models) used to live
 * here so the public header could expose it to the gui settings dialog.
 * It is only ever called by that dialog, so since the Ollama node became
 * a two-tier plugin it lives entirely in the companion module
 * (plugins/ollama/pn-ollama-gui.c) — the logic half never queries the
 * model list. */

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ollama_get_property (GObject *object, guint prop_id,
                        GValue *value, GParamSpec *pspec)
{
    PnOllama *self = PN_OLLAMA (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:   g_value_set_string (value, self->hostname);   break;
    case PROP_PORT:       g_value_set_int    (value, self->port);       break;
    case PROP_MODEL:      g_value_set_string (value, self->model);      break;
    case PROP_KEEP_ALIVE: g_value_set_string (value, self->keep_alive); break;
    case PROP_PREFIX:     g_value_set_string (value, self->prefix);     break;
    case PROP_SUFFIX:     g_value_set_string (value, self->suffix);     break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_string (gchar **slot, const gchar *value)
{
    g_free (*slot);
    *slot = g_strdup (value ? value : "");
}

static void
pn_ollama_set_property (GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec)
{
    PnOllama *self = PN_OLLAMA (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->hostname, s) != 0)
            {
                set_string (&self->hostname, s);
                refresh_visual_state (self);
                g_object_notify_by_pspec (object, props[PROP_HOSTNAME]);
            }
        }
        break;
    case PROP_PORT:
        {
            gint v = g_value_get_int (value);
            if (self->port != v)
            {
                self->port = v;
                g_object_notify_by_pspec (object, props[PROP_PORT]);
            }
        }
        break;
    case PROP_MODEL:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->model, s) != 0)
            {
                set_string (&self->model, s);
                refresh_visual_state (self);
                g_object_notify_by_pspec (object, props[PROP_MODEL]);
            }
        }
        break;
    case PROP_KEEP_ALIVE:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->keep_alive, s) != 0)
            {
                set_string (&self->keep_alive, s);
                g_object_notify_by_pspec (object, props[PROP_KEEP_ALIVE]);
            }
        }
        break;
    case PROP_PREFIX:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->prefix, s) != 0)
            {
                set_string (&self->prefix, s);
                g_object_notify_by_pspec (object, props[PROP_PREFIX]);
            }
        }
        break;
    case PROP_SUFFIX:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->suffix, s) != 0)
            {
                set_string (&self->suffix, s);
                g_object_notify_by_pspec (object, props[PROP_SUFFIX]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ollama_dispose (GObject *object)
{
    PnOllama *self = PN_OLLAMA (object);

    /* Abort any in-flight generate request here rather than in finalize.
     * on_generate_done holds a strong ref on the node via GenerateCtx,
     * so finalize cannot run until the request completes -- a
     * finalize-time cancel is dead code for the in-flight case and the
     * node (and the closed worksheet) stays pinned until Ollama replies.
     * dispose runs when the worksheet drops its ref, before that ctx ref;
     * cancelling here makes soup invoke on_generate_done with
     * G_IO_ERROR_CANCELLED, which drops the last ref and lets finalize
     * proceed.  dispose may run more than once, so guard on non-NULL and
     * rely on g_clear_object being a no-op the second time. */
    if (self->cancellable != NULL)
        g_cancellable_cancel (self->cancellable);

    g_clear_object (&self->cancellable);
    g_clear_object (&self->session);

    G_OBJECT_CLASS (pn_ollama_parent_class)->dispose (object);
}

static void
pn_ollama_finalize (GObject *object)
{
    PnOllama *self = PN_OLLAMA (object);

    g_clear_pointer (&self->hostname,   g_free);
    g_clear_pointer (&self->model,      g_free);
    g_clear_pointer (&self->keep_alive, g_free);
    g_clear_pointer (&self->prefix,     g_free);
    g_clear_pointer (&self->suffix,     g_free);

    G_OBJECT_CLASS (pn_ollama_parent_class)->finalize (object);
}

static void
pn_ollama_class_init (PnOllamaClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_ollama_get_property;
    object_class->set_property = pn_ollama_set_property;
    object_class->dispose      = pn_ollama_dispose;
    object_class->finalize     = pn_ollama_finalize;

    node_class->receive               = pn_ollama_receive;
    /* build_property_editor installed by the gui tier
     * (pn_ollama_gui_install). */

    node_class->palette_icon = PN_OLLAMA_ICON;
    node_class->class_name   = "Ollama";
    node_class->icon         = PN_OLLAMA_ICON;
    node_class->color        = (PnColor){ 0.42, 0.36, 0.72, 1.0 };
    node_class->category     = "Filters/Compute & AI";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Hostname or IP of the Ollama server (e.g. \"localhost\", "
            "\"gpu.lan\").  Leave empty to talk to an Ollama instance "
            "running on this machine -- the empty value is resolved "
            "to the loopback name when building the URL, so libsoup "
            "always reaches it regardless of mDNS / DNS setup.",
            PN_OLLAMA_DEFAULT_HOSTNAME,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_PORT] = g_param_spec_int (
            "port", "Port",
            "TCP port of the Ollama HTTP API (default 11434)",
            1, 65535, PN_OLLAMA_DEFAULT_PORT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODEL] = g_param_spec_string (
            "model", "Model",
            "Ollama model id (e.g. \"llama3.2:latest\"); the dialog "
            "populates this from the server's /api/tags listing",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_KEEP_ALIVE] = g_param_spec_string (
            "keep-alive", "Keep alive",
            "How long Ollama keeps the model loaded between requests: "
            "a duration like \"5m\" or \"1h\", \"0\" to unload "
            "immediately after each call, or \"-1\" to keep it "
            "resident forever",
            PN_OLLAMA_DEFAULT_KEEP_ALIVE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PREFIX] = g_param_spec_string (
            "prefix", "Prefix",
            "Text prepended to the incoming data.output before it is "
            "sent to the model -- useful for system / role framing",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SUFFIX] = g_param_spec_string (
            "suffix", "Suffix",
            "Text appended to the incoming data.output before it is "
            "sent to the model",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_ollama_init (PnOllama *self)
{
    PnNode *node = PN_NODE (self);

    self->hostname    = g_strdup (PN_OLLAMA_DEFAULT_HOSTNAME);
    self->port        = PN_OLLAMA_DEFAULT_PORT;
    self->model       = g_strdup ("");
    self->keep_alive  = g_strdup (PN_OLLAMA_DEFAULT_KEEP_ALIVE);
    self->prefix      = g_strdup ("");
    self->suffix      = g_strdup ("");

    self->session     = soup_session_new ();
    self->cancellable = g_cancellable_new ();

    pn_node_set_class_name (node, "Ollama");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    refresh_visual_state (self);
}

PnOllama *
pn_ollama_new (void)
{
    return g_object_new (PN_TYPE_OLLAMA, NULL);
}
