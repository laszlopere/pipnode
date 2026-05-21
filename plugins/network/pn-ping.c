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

#include "pn-ping.h"
#include "pn-message.h"

#include <string.h>

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_PING_NORMAL_ICON  "\xef\x87\xab"      /* fa-wifi U+F1EB */
#define PN_PING_WARNING_ICON "\xe2\x9d\x97"      /* ❗ U+2757 */

#define PN_PING_DEFAULT_PERIOD 5u

struct _PnPing
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @host while the main thread may
     * overwrite it via the property setter, so accesses are
     * serialised through @mutex.  The string itself is owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *host;
};

G_DEFINE_TYPE (PnPing, pn_ping, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOST,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Flip the node body between the configured (blue 📡) and
 *  unconfigured (red ❗) appearance based on whether @configured. */
static void
apply_visual_state (
        PnPing  *self,
        gboolean configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        GdkRGBA blue = { 0.42, 0.62, 0.86, 1.0 };
        pn_node_set_color (node, &blue);
        pn_node_set_icon  (node, PN_PING_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_PING_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Host accessor (thread-safe)                                        */
/* ------------------------------------------------------------------ */

static gchar *
ping_dup_host_locked (PnPing *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
ping_set_host (
        PnPing      *self,
        const gchar *host)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  configured;

    /* Treat NULL and "" as the same unconfigured state but still
     * round-trip whatever the caller passed so `notify::host` reports
     * a stable value. */
    replacement = (host != NULL) ? g_strdup (host) : NULL;
    configured  = (host != NULL && *host != '\0');

    g_mutex_lock (&self->mutex);
    old = self->host;
    self->host = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOST]);
}

/* ------------------------------------------------------------------ */
/*  Output parsing                                                     */
/* ------------------------------------------------------------------ */

/** Pull the round-trip time out of a successful iputils ping reply.
 *  Looks for the "time=" token; on no match leaves @out_have FALSE. */
static void
parse_rtt_ms (
        const gchar *output,
        gboolean    *out_have,
        gdouble     *out_rtt)
{
    const gchar *p;
    gchar       *end = NULL;
    gdouble      value;

    *out_have = FALSE;
    *out_rtt  = 0.0;

    if (output == NULL)
        return;

    p = strstr (output, "time=");
    if (p == NULL)
        return;

    p += 5;  /* skip "time=" */
    value = g_ascii_strtod (p, &end);
    if (end != p)
    {
        *out_have = TRUE;
        *out_rtt  = value;
    }
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  We      */
/*  shell out to the system ping utility — easier than rolling our    */
/*  own raw-socket ICMP client and keeps us out of CAP_NET_RAW       */
/*  territory.                                                         */
/* ------------------------------------------------------------------ */

static void
pn_ping_trigger (PnAutoTrigger *trigger)
{
    PnPing      *self        = PN_PING (trigger);
    PnNode      *node        = PN_NODE (self);
    gchar       *host        = ping_dup_host_locked (self);
    gchar       *cmd         = NULL;
    gchar       *quoted_host = NULL;
    gchar       *stdout_text = NULL;
    gchar       *stderr_text = NULL;
    gint         exit_status = 0;
    GError      *error       = NULL;
    PnMessage   *msg;
    gboolean     spawned;
    gboolean     success     = FALSE;
    gboolean     have_rtt    = FALSE;
    gdouble      rtt_ms      = 0.0;
    guint        period;
    guint        timeout;

    /* Skip work entirely while the node is in its "configuration
     * required" state.  The visual marker on the canvas already tells
     * the user; emitting nothing keeps downstream nodes idle. */
    if (host == NULL || *host == '\0')
    {
        g_free (host);
        return;
    }

    /* iputils -W is a per-reply timeout in whole seconds; cap it one
     * below the period so a dead host never delays the next tick. */
    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    quoted_host = g_shell_quote (host);
    cmd         = g_strdup_printf ("ping -c 1 -W %u %s", timeout, quoted_host);

    spawned = g_spawn_command_line_sync (cmd,
                                         &stdout_text,
                                         &stderr_text,
                                         &exit_status,
                                         &error);

    if (!spawned)
    {
        g_warning ("pn-ping: failed to spawn '%s': %s",
                   cmd, error ? error->message : "(unknown)");
        g_clear_error (&error);
    }
    else
    {
        success = g_spawn_check_wait_status (exit_status, NULL);
        parse_rtt_ms (stdout_text, &have_rtt, &rtt_ms);
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",    host);
    pn_message_set_boolean (msg, "success", success);
    if (have_rtt)
        pn_message_set_double (msg, "value", rtt_ms);

    /* A short, TTS-friendly sentence summarising the probe.  The raw
     * ping(8) stdout is intentionally not propagated -- downstream
     * sinks (debug log, speech synthesiser) do better with a fixed
     * shape than with kernel-time-stamped trace lines. */
    {
        gchar *summary;

        if (success && have_rtt)
        {
            /* Round to whole milliseconds for the spoken sentence;
             * sub-millisecond RTTs become "less than a millisecond"
             * rather than the misleading "0 milliseconds", and a
             * one-millisecond round trip drops the plural "s". */
            long ms = (long) (rtt_ms + 0.5);

            if (ms <= 0)
                summary = g_strdup_printf (
                        "The host %s is reachable, "
                        "round trip time less than a millisecond.",
                        host);
            else if (ms == 1)
                summary = g_strdup_printf (
                        "The host %s is reachable, "
                        "round trip time 1 millisecond.",
                        host);
            else
                summary = g_strdup_printf (
                        "The host %s is reachable, "
                        "round trip time %ld milliseconds.",
                        host, ms);
        }
        else if (success)
            summary = g_strdup_printf ("The host %s is reachable.", host);
        else
            summary = g_strdup_printf ("The host %s is not reachable.", host);

        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (quoted_host);
    g_free (cmd);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (host);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ping_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPing *self = PN_PING (object);

    switch (prop_id)
    {
    case PROP_HOST:
        g_value_take_string (value, ping_dup_host_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_ping_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPing *self = PN_PING (object);

    switch (prop_id)
    {
    case PROP_HOST:
        ping_set_host (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ping_finalize (GObject *object)
{
    PnPing *self = PN_PING (object);

    g_clear_pointer (&self->host, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_ping_parent_class)->finalize (object);
}

static void
pn_ping_class_init (PnPingClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_ping_get_property;
    object_class->set_property = pn_ping_set_property;
    object_class->finalize     = pn_ping_finalize;
    trigger_class->trigger     = pn_ping_trigger;

    /* The instance icon flips between 📡 and ❗ depending on whether
     * a host has been configured.  The palette wants a stable glyph
     * regardless of state, so pin the normal one here. */
    node_class->palette_icon   = PN_PING_NORMAL_ICON;
    node_class->class_name     = "Network Ping";
    node_class->icon           = PN_PING_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Network";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOST] = g_param_spec_string (
            "host", "Host",
            "Hostname or IP address to ping; while empty the node is "
            "marked as needing configuration and emits nothing",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_ping_init (PnPing *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->host = NULL;

    pn_node_set_class_name (node, "Network Ping");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Default period is more relaxed than the base-class 1s so a
     * fresh ping node does not immediately flood once configured. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_PING_DEFAULT_PERIOD);

    /* Start in the unconfigured (red ❗) state; the host setter will
     * flip it to the normal blue 📡 once a host is supplied. */
    apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPing *
pn_ping_new (void)
{
    return g_object_new (PN_TYPE_PING, NULL);
}
