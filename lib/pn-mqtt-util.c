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

#include "pn-mqtt-util.h"

#include "pn-json-path.h"
#include "pn-subst.h"

#include <json-glib/json-glib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  URL parsing                                                        */
/* ------------------------------------------------------------------ */

/** Parse a decimal port (1..65535) from @s into @out_port.  @s must be
 *  the whole port token (the strtol has to consume all of it).  Returns
 *  %FALSE on a non-numeric, empty, or out-of-range value. */
static gboolean
parse_port_str (const gchar *s, int *out_port)
{
    gchar *endp;
    glong  parsed;

    errno  = 0;
    parsed = strtol (s, &endp, 10);
    if (errno != 0 || endp == s || *endp != '\0' || parsed <= 0 || parsed > 65535)
        return FALSE;

    *out_port = (int) parsed;
    return TRUE;
}

gboolean
pn_mqtt_parse_url (
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

    port = tls ? 8883 : 1883;

    if (*p == '[')
    {
        /* Bracketed IPv6 literal: host is everything between the
         * brackets, so the ':' separators inside the address are not
         * mistaken for the port delimiter.  An optional ":port" may
         * follow the closing ']'. */
        const gchar *close = strchr (p, ']');

        if (close == NULL || close == p + 1)   /* unterminated or empty "[]" */
            return FALSE;

        host = g_strndup (p + 1, (gsize) (close - (p + 1)));
        p    = close + 1;

        if (*p == ':')
        {
            if (!parse_port_str (p + 1, &port))
            {
                g_free (host);
                return FALSE;
            }
        }
        else if (*p != '\0')   /* junk after the closing bracket */
        {
            g_free (host);
            return FALSE;
        }
    }
    else
    {
        /* Reject an empty host part (e.g. just "tcp://"). */
        if (*p == '\0' || *p == ':' || *p == '/')
            return FALSE;

        colon = strchr (p, ':');
        if (colon != NULL)
        {
            host = g_strndup (p, (gsize) (colon - p));
            if (!parse_port_str (colon + 1, &port))
            {
                g_free (host);
                return FALSE;
            }
        }
        else
        {
            host = g_strdup (p);
        }
    }

    *out_host = host;
    *out_port = port;
    *out_tls  = tls;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Connection identity resolution                                     */
/* ------------------------------------------------------------------ */

void
pn_mqtt_resolve_connection (
        PnProfile      *profile,
        const gchar    *inline_url,
        const gchar    *inline_user,
        const gchar    *inline_pass,
        const gchar    *inline_cid,
        gchar         **out_url,
        gchar         **out_user,
        gchar         **out_pass,
        gchar         **out_cid,
        PnMqttConnOpts *out_opts)
{
    if (profile != NULL)
    {
        if (out_url)  *out_url  = pn_profile_get_string (profile, "url");
        if (out_user) *out_user = pn_profile_get_string (profile, "username");
        if (out_pass) *out_pass = pn_profile_get_secret (profile, "password");
        if (out_cid)  *out_cid  = pn_profile_get_string (profile, "client-id");
        if (out_opts)
        {
            /* TLS material + keep-alive live only in the profile.  ca-file and
             * the mTLS cert/key are filesystem paths the host trusts, not
             * credentials, so they are PN_FIELD_FILE (unmasked, resolved as
             * strings), not secrets; keepalive defaults to 60 via the schema. */
            out_opts->ca_file      = pn_profile_get_string (profile, "ca-file");
            out_opts->client_cert  = pn_profile_get_string (profile, "client-cert");
            out_opts->client_key   = pn_profile_get_string (profile, "client-key");
            out_opts->tls_insecure = pn_profile_get_bool   (profile, "tls-insecure");
            out_opts->keepalive    = (gint) pn_profile_get_int (profile, "keepalive");
        }
    }
    else
    {
        if (out_url)  *out_url  = g_strdup (inline_url  ? inline_url  : "");
        if (out_user) *out_user = g_strdup (inline_user ? inline_user : "");
        if (out_pass) *out_pass = g_strdup (inline_pass ? inline_pass : "");
        if (out_cid)  *out_cid  = g_strdup (inline_cid  ? inline_cid  : "");
        if (out_opts)
        {
            /* Custom-settings inline mode exposes no TLS / keep-alive UI: leave
             * the unset defaults so the node keeps today's behaviour (system
             * trust store, no mTLS, default keep-alive). */
            out_opts->ca_file      = g_strdup ("");
            out_opts->client_cert  = g_strdup ("");
            out_opts->client_key   = g_strdup ("");
            out_opts->tls_insecure = FALSE;
            out_opts->keepalive    = 0;
        }
    }
}

void
pn_mqtt_conn_opts_clear (PnMqttConnOpts *opts)
{
    if (opts == NULL)
        return;

    g_clear_pointer (&opts->ca_file,     g_free);
    g_clear_pointer (&opts->client_cert, g_free);
    g_clear_pointer (&opts->client_key,  g_free);
    opts->tls_insecure = FALSE;
    opts->keepalive    = 0;
}

int
pn_mqtt_keepalive_seconds (gint configured)
{
    if (configured <= 0)
        return PN_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    if (configured > 65535)   /* MQTT keep-alive is a 16-bit field. */
        return 65535;
    return configured;
}

/* ------------------------------------------------------------------ */
/*  Inbound message construction                                       */
/* ------------------------------------------------------------------ */

PnMessage *
pn_mqtt_build_message (
        PnNode       *source,
        const gchar  *topic,
        const guchar *payload_bytes,
        gsize         payload_len,
        int           qos,
        gboolean      retained)
{
    PnMessage   *msg;
    gchar       *payload = NULL;
    gboolean     is_utf8 = FALSE;
    gboolean     is_json = FALSE;
    JsonNode    *json_root = NULL;
    JsonParser  *parser  = NULL;
    gchar       *output;
    const gchar *topic_str = topic ? topic : "";

    /* Envelope topic is the MQTT topic itself so what the Debug pane
     * shows matches what the broker published, and a downstream Filter
     * routes on the same string the user typed at the broker. */
    msg = pn_message_new (source, topic_str);

    /* mosquitto_message.payload is not NUL-terminated; copy it out
     * and decide whether the bytes are valid UTF-8 before exposing
     * them as a string.  Binary payloads land as "". */
    if (payload_len > 0 && payload_bytes != NULL)
    {
        gchar *raw = g_strndup ((const gchar *) payload_bytes, payload_len);
        if (g_utf8_validate (raw, (gssize) payload_len, NULL))
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

    pn_message_set_int     (msg, "qos",      qos);
    pn_message_set_boolean (msg, "retained", retained);

    /* Try to parse the payload as JSON so a Tasmota-style
     * `{"Time":...,"AM2301":{...}}` lands on `data.payload` as a
     * proper nested object the downstream Filter / Format nodes can
     * pluck values out of with a `payload.AM2301.Temperature` path
     * instead of having to peel a string-escaped JSON blob first. */
    if (is_utf8 && payload[0] != '\0')
    {
        parser = json_parser_new ();
        if (json_parser_load_from_data (parser, payload,
                                        (gssize) payload_len, NULL))
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

        /* Bare JSON scalar → mirror onto data.value so a Graph just works
         * without an extra Format step.  A JSON boolean follows the
         * project's 0.0/1.0 boolean convention (false → 0.0, true → 1.0)
         * so a `{"POWER":true}`-style payload — or a bare `true` — drives a
         * downstream gate the same way a numeric 1 would. */
        if (JSON_NODE_HOLDS_VALUE (json_root))
        {
            GType vt = json_node_get_value_type (json_root);

            if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
                pn_message_set_double (msg, "value",
                                       json_node_get_double (json_root));
            else if (vt == G_TYPE_BOOLEAN)
                pn_message_set_double (msg, "value",
                                       json_node_get_boolean (json_root) ? 1.0
                                                                         : 0.0);
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

/* ------------------------------------------------------------------ */
/*  Outbound payload construction                                       */
/* ------------------------------------------------------------------ */

/** Expand every "${path}" in @tmpl to the plain-text form of the JSON
 *  value at that path against @root.  Same `${path/to/field}` syntax
 *  PnFormat / PnRewrite use; unknown paths render as the empty string,
 *  an unterminated "${" is emitted verbatim. */
static gchar *
expand_placeholders (
        const gchar *tmpl,
        JsonObject  *root,
        PnFlow      *flow)
{
    PnSubstResolver  resolver;
    PnSubstResolver  globals;
    PnSubstResolver *chain[3];
    PnSubstContext   ctx;

    pn_subst_resolver_json (&resolver, root);
    pn_flow_subst_resolver_globals (&globals, flow);
    chain[0] = &resolver;   /* message fields win */
    chain[1] = &globals;    /* then document globals */
    chain[2] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_TEXT;
    ctx.miss      = PN_SUBST_MISS_EMPTY;

    return pn_subst_expand (tmpl, &ctx);
}

gboolean
pn_mqtt_build_payload (
        const gchar *payload_template,
        PnMessage   *message,
        PnFlow      *flow,
        gchar      **out_bytes,
        gsize       *out_len)
{
    if (payload_template != NULL && *payload_template != '\0')
    {
        JsonObject *root     = pn_json_lookup_root_for_message (message);
        gchar      *expanded = expand_placeholders (payload_template, root,
                                                    flow);
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
/*  Offline publish queue                                              */
/* ------------------------------------------------------------------ */

void
pn_mqtt_pending_free (gpointer entry)
{
    PnMqttPending *p = entry;

    if (p == NULL)
        return;

    g_free (p->topic);
    g_free (p->payload);
    g_free (p);
}

void
pn_mqtt_pending_enqueue (
        GQueue      *queue,
        const gchar *topic,
        gchar       *payload,
        gsize        payload_len,
        int          qos,
        gboolean     retain,
        gint64       now_us)
{
    PnMqttPending *p;

    while (g_queue_get_length (queue) >= PN_MQTT_SINK_QUEUE_MAX_LEN)
        pn_mqtt_pending_free (g_queue_pop_head (queue));

    p = g_new0 (PnMqttPending, 1);
    p->topic       = g_strdup (topic);
    p->payload     = payload;          /* transfer */
    p->payload_len = payload_len;
    p->qos         = qos;
    p->retain      = retain;
    p->received_us = now_us;

    g_queue_push_tail (queue, p);
}

gboolean
pn_mqtt_payload_within_limit (gsize payload_len)
{
    return payload_len <= PN_MQTT_MAX_PAYLOAD_BYTES;
}

guint
pn_mqtt_pending_retarget (
        GQueue      *queue,
        const gchar *old_identity,
        const gchar *new_identity)
{
    guint dropped;

    g_return_val_if_fail (queue != NULL, 0);

    /* First resolve (no prior identity) or a reconnect to the same broker:
     * keep the backlog — surviving a transient drop / vault touch to the
     * same broker is the whole reason the queue exists. */
    if (old_identity == NULL || g_strcmp0 (old_identity, new_identity) == 0)
        return 0;

    /* The broker we are about to talk to differs from the one these
     * publishes were buffered for: they were meant for a broker we have
     * retargeted away from, so drop them rather than misdeliver. */
    dropped = g_queue_get_length (queue);
    g_queue_clear_full (queue, pn_mqtt_pending_free);
    return dropped;
}

GPtrArray *
pn_mqtt_pending_collect_fresh (
        GQueue *queue,
        gint64  now_us,
        gint64  max_age_us)
{
    GPtrArray     *fresh = g_ptr_array_new_with_free_func (pn_mqtt_pending_free);
    PnMqttPending *p;

    while ((p = g_queue_pop_head (queue)) != NULL)
    {
        if (now_us - p->received_us <= max_age_us)
            g_ptr_array_add (fresh, p);
        else
            pn_mqtt_pending_free (p);   /* too stale to still be meaningful */
    }

    return fresh;
}
