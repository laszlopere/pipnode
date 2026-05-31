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

#include "pn-dns.h"
#include "pn-message.h"

#define PN_DNS_NORMAL_ICON  "\xef\x83\xa8"        /* fa-sitemap U+F0E8 */
#define PN_DNS_WARNING_ICON "\xe2\x9d\x97"        /* ❗ U+2757 */

#define PN_DNS_DEFAULT_PERIOD 30u

struct _PnDns
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @server and @names while the main
     * thread may overwrite them via the property setters, so accesses
     * are serialised through @mutex.  The strings are owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *server;     /* DNS server; NULL/"" means use the system resolver */
    gchar  *names;      /* whitespace/comma-separated list of names to query */
};

G_DEFINE_TYPE (PnDns, pn_dns, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_SERVER,
    PROP_NAMES,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the configured (blue 📚) and
 *  unconfigured (red ❗) appearance based on whether @configured.
 *  Only the names list governs configuration: the server is optional
 *  (empty falls back to the system resolver). */
static void
apply_visual_state (
        PnDns   *self,
        gboolean configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        PnColor blue = { 0.42, 0.62, 0.86, 1.0 };
        pn_node_set_color (node, &blue);
        pn_node_set_icon  (node, PN_DNS_NORMAL_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_DNS_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Property helpers (thread-safe)                                     */
/* ------------------------------------------------------------------ */

static gchar *
dns_dup_server_locked (PnDns *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->server);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static gchar *
dns_dup_names_locked (PnDns *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->names);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
dns_set_server (
        PnDns       *self,
        const gchar *server)
{
    gchar *old;
    gchar *replacement;

    /* Round-trip whatever the caller passed (including "") so
     * notify::server reports a stable value, but apply NULL on the
     * empty string for cleanliness in the worker. */
    replacement = (server != NULL && *server != '\0')
                      ? g_strdup (server)
                      : NULL;

    g_mutex_lock (&self->mutex);
    old = self->server;
    self->server = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SERVER]);
}

static void
dns_set_names (
        PnDns       *self,
        const gchar *names)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  configured;

    replacement = (names != NULL) ? g_strdup (names) : NULL;
    configured  = (names != NULL && *names != '\0');

    g_mutex_lock (&self->mutex);
    old = self->names;
    self->names = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NAMES]);
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

/** Split @raw on whitespace and commas into a NULL-terminated array
 *  of non-empty tokens.  Caller frees with g_strfreev(). */
static gchar **
split_names (const gchar *raw)
{
    gchar  **tokens;
    gchar  **out;
    guint    i;
    guint    n_in  = 0;
    guint    n_out = 0;

    if (raw == NULL)
        return g_new0 (gchar *, 1);

    tokens = g_strsplit_set (raw, ", \t\r\n", -1);
    for (i = 0; tokens[i] != NULL; i++)
        n_in++;

    out = g_new0 (gchar *, n_in + 1);
    for (i = 0; i < n_in; i++)
        if (*tokens[i] != '\0')
            out[n_out++] = g_strdup (tokens[i]);
    out[n_out] = NULL;

    g_strfreev (tokens);
    return out;
}

/** Run dig for a single name and report whether at least one answer
 *  record came back.  The purpose of this node is to verify the DNS
 *  server, so we deliberately avoid NSS/getaddrinfo: that would let
 *  /etc/hosts or mDNS mask a broken DNS server.  When @server is
 *  NULL the dig invocation has no @-argument, so dig falls back to
 *  the resolvers listed in /etc/resolv.conf — still pure DNS.
 *  @timeout is the per-query timeout in whole seconds. */
static gboolean
query_one (
        PnDns       *self,
        const gchar *server,
        const gchar *name,
        guint        timeout)
{
    gchar    *quoted_server = NULL;
    gchar    *quoted_name;
    gchar    *cmd;
    gchar    *stdout_text = NULL;
    gchar    *stderr_text = NULL;
    gint      exit_status = 0;
    GError   *error       = NULL;
    gboolean  spawned;
    gboolean  success     = FALSE;

    quoted_name = g_shell_quote (name);

    if (server != NULL && *server != '\0')
    {
        quoted_server = g_shell_quote (server);
        cmd = g_strdup_printf (
                "dig +short +time=%u +tries=1 @%s %s",
                timeout, quoted_server, quoted_name);
    }
    else
    {
        cmd = g_strdup_printf (
                "dig +short +time=%u +tries=1 %s",
                timeout, quoted_name);
    }

    spawned = g_spawn_command_line_sync (cmd,
                                         &stdout_text,
                                         &stderr_text,
                                         &exit_status,
                                         &error);

    if (!spawned)
    {
        pn_auto_trigger_log_on_main (
                PN_AUTO_TRIGGER (self), PN_LOG_LEVEL_ERROR,
                "Could not start the DNS lookup for '%s': %s",
                name, error ? error->message : "(unknown)");
        g_clear_error (&error);
    }
    else if (g_spawn_check_wait_status (exit_status, NULL))
    {
        /* dig exits 0 even when nothing was returned (NXDOMAIN, empty
         * answer); treat an empty stdout as a failed lookup. */
        if (stdout_text != NULL)
        {
            const gchar *p = stdout_text;
            while (*p != '\0' && g_ascii_isspace (*p))
                p++;
            if (*p != '\0')
                success = TRUE;
        }
    }

    g_free (quoted_server);
    g_free (quoted_name);
    g_free (cmd);
    g_free (stdout_text);
    g_free (stderr_text);

    return success;
}

static void
pn_dns_trigger (PnAutoTrigger *trigger)
{
    PnDns      *self          = PN_DNS (trigger);
    PnNode     *node          = PN_NODE (self);
    gchar      *server        = dns_dup_server_locked (self);
    gchar      *names_raw     = dns_dup_names_locked (self);
    gchar     **names         = NULL;
    PnMessage  *msg;
    JsonObject *per_name_obj;
    JsonNode   *per_name_node;
    GString    *summary;
    guint       n_total       = 0;
    guint       n_ok          = 0;
    guint       i;
    guint       period;
    guint       timeout;

    /* "Configuration required" state: skip work entirely while no
     * names are set.  The visual marker on the canvas already tells
     * the user; emitting nothing keeps downstream nodes idle. */
    if (names_raw == NULL || *names_raw == '\0')
        goto out;

    names = split_names (names_raw);
    for (i = 0; names[i] != NULL; i++)
        n_total++;

    if (n_total == 0)
        goto out;

    /* Cap per-query timeout one second below the period so even a
     * fully unreachable server cannot delay the next tick.  When
     * many names share a single period, scale the budget so the
     * whole batch fits. */
    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;
    if (timeout / n_total >= 1u)
        timeout = timeout / n_total;
    else
        timeout = 1u;

    per_name_obj = json_object_new ();

    for (i = 0; names[i] != NULL; i++)
    {
        gboolean ok = query_one (self, server, names[i], timeout);
        if (ok)
            n_ok++;
        json_object_set_boolean_member (per_name_obj, names[i], ok);
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "server",
                            (server != NULL && *server != '\0')
                                ? server : "system");
    pn_message_set_boolean (msg, "success", n_ok == n_total);
    pn_message_set_int     (msg, "ok_count",   (gint) n_ok);
    pn_message_set_int     (msg, "name_count", (gint) n_total);

    per_name_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (per_name_node, per_name_obj);
    pn_message_set_member (msg, "results", per_name_node);

    /* TTS-friendly one-line summary of the probe. */
    summary = g_string_new (NULL);
    {
        const gchar *who = (server != NULL && *server != '\0')
                               ? server : "the system DNS server";

        if (n_ok == n_total)
            g_string_append_printf (summary,
                    "DNS server %s resolved all %u names.",
                    who, n_total);
        else if (n_ok == 0)
            g_string_append_printf (summary,
                    "DNS server %s failed to resolve any of %u names.",
                    who, n_total);
        else
            g_string_append_printf (summary,
                    "DNS server %s resolved %u of %u names.",
                    who, n_ok, n_total);
    }
    pn_message_set_string (msg, "output", summary->str);
    g_string_free (summary, TRUE);

    pn_auto_trigger_emit_on_main (trigger, msg);

out:
    g_strfreev (names);
    g_free (server);
    g_free (names_raw);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dns_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDns *self = PN_DNS (object);

    switch (prop_id)
    {
    case PROP_SERVER:
        g_value_take_string (value, dns_dup_server_locked (self));
        break;
    case PROP_NAMES:
        g_value_take_string (value, dns_dup_names_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_dns_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDns *self = PN_DNS (object);

    switch (prop_id)
    {
    case PROP_SERVER:
        dns_set_server (self, g_value_get_string (value));
        break;
    case PROP_NAMES:
        dns_set_names (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dns_finalize (GObject *object)
{
    PnDns *self = PN_DNS (object);

    g_clear_pointer (&self->server, g_free);
    g_clear_pointer (&self->names,  g_free);
    g_mutex_clear   (&self->mutex);

    G_OBJECT_CLASS (pn_dns_parent_class)->finalize (object);
}

static void
pn_dns_class_init (PnDnsClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_dns_get_property;
    object_class->set_property = pn_dns_set_property;
    object_class->finalize     = pn_dns_finalize;
    trigger_class->trigger     = pn_dns_trigger;

    /* Instance icon flips between 📚 and ❗ depending on whether names
     * have been configured.  Pin a stable glyph for the palette. */
    node_class->palette_icon   = PN_DNS_NORMAL_ICON;
    node_class->class_name     = "DNS Check";
    node_class->icon           = PN_DNS_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_SERVER] = g_param_spec_string (
            "server", "Server",
            "DNS server to query (hostname or IP).  Leave empty to "
            "query the system's configured resolver instead",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_NAMES] = g_param_spec_string (
            "names", "Names",
            "Whitespace- or comma-separated list of DNS names to "
            "look up on every tick; while empty the node is marked "
            "as needing configuration and emits nothing",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_dns_init (PnDns *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->server = NULL;
    self->names  = NULL;

    pn_node_set_class_name (node, "DNS Check");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* DNS lookups are cheap but flooding a public resolver from a
     * desktop tool is rude; default to one batch every 30 s. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_DNS_DEFAULT_PERIOD);

    apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDns *
pn_dns_new (void)
{
    return g_object_new (PN_TYPE_DNS, NULL);
}
