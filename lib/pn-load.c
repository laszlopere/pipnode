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

#include "pn-load.h"
#include "pn-message.h"

/* fa-tachometer U+F0E4 — a gauge-style glyph reads as "system load". */
#define PN_LOAD_ICON           "\xef\x83\xa4"

#define PN_LOAD_DEFAULT_PERIOD 5u
/* Default is the empty string -- treated identically to the literal
 * "localhost" by hostname_is_local() below, but kept blank in the
 * settings dialog so the gray placeholder text from
 * pn_param_spec_set_hostname_hint() can show the user the actual
 * machine name g_get_host_name() will substitute at sample time. */
#define PN_LOAD_DEFAULT_HOST   ""

struct _PnLoad
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @hostname while the main thread may
     * overwrite it via the property setter, so accesses are
     * serialised through @mutex.  The string itself is owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *hostname;
};

G_DEFINE_TYPE (PnLoad, pn_load, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOSTNAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Hostname accessor (thread-safe)                                    */
/* ------------------------------------------------------------------ */

static gchar *
load_dup_hostname_locked (PnLoad *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
load_set_hostname (
        PnLoad      *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    /* NULL collapses to the default (empty string) so a freshly-loaded
     * node missing the property still behaves sensibly -- the empty
     * marker is then resolved to g_get_host_name() at sample / emit
     * time, same as if the user had explicitly cleared the entry. */
    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_LOAD_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

/* True when @hostname selects the local /proc/loadavg path: NULL,
 * empty string, and the literal "localhost" all map there. */
static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/loadavg parsing                                              */
/*                                                                     */
/*  The kernel format is "X.XX Y.YY Z.ZZ R/T LASTPID\n" with the       */
/*  three averages we care about up front.  g_ascii_strtod() is used   */
/*  rather than sscanf() so the parse is locale-independent — a host   */
/*  with LC_NUMERIC=de_DE would otherwise silently fail on the dot.    */
/* ------------------------------------------------------------------ */

/* Exposed (non-static) for unit testing — see tests/unit/test-pn-load.c.
 * Kept out of the public header to keep the library surface clean; the
 * test declares a matching extern prototype. */
gboolean
pn_load_parse_loadavg (
        const gchar *text,
        gdouble     *out_l1,
        gdouble     *out_l5,
        gdouble     *out_l15)
{
    const gchar *p = text;
    gchar       *end;

    if (text == NULL)
        return FALSE;

    *out_l1  = g_ascii_strtod (p, &end);
    if (end == p) return FALSE;
    p = end;

    *out_l5  = g_ascii_strtod (p, &end);
    if (end == p) return FALSE;
    p = end;

    *out_l15 = g_ascii_strtod (p, &end);
    if (end == p) return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Sample acquisition                                                 */
/* ------------------------------------------------------------------ */

/** Pull /proc/loadavg straight off the local filesystem.  We use
 *  g_file_get_contents() rather than fopen() so the caller gets a
 *  GError describing any I/O problem — same shape as the SSH path. */
static gboolean
sample_local (
        gchar  **out_text,
        GError **error)
{
    return g_file_get_contents ("/proc/loadavg", out_text, NULL, error);
}

/** Fetch /proc/loadavg from @hostname over SSH.  Passwordless auth
 *  is required (BatchMode=yes); ConnectTimeout caps how long a dead
 *  host can stall the worker thread. */
static gboolean
sample_ssh (
        const gchar  *hostname,
        guint         timeout,
        gchar       **out_text,
        gchar       **out_stderr,
        GError      **error)
{
    gchar    *quoted_host;
    gchar    *cmd;
    gint      exit_status = 0;
    gboolean  spawned;
    gboolean  ok = FALSE;

    quoted_host = g_shell_quote (hostname);
    cmd = g_strdup_printf (
            "ssh -o BatchMode=yes "
            "-o ConnectTimeout=%u "
            "-o StrictHostKeyChecking=accept-new "
            "%s cat /proc/loadavg",
            timeout, quoted_host);

    spawned = g_spawn_command_line_sync (cmd,
                                         out_text,
                                         out_stderr,
                                         &exit_status,
                                         error);

    if (spawned)
        ok = g_spawn_check_wait_status (exit_status, error);

    g_free (quoted_host);
    g_free (cmd);

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger, so the    */
/*  potentially blocking SSH call cannot freeze the GUI.               */
/* ------------------------------------------------------------------ */

static void
pn_load_trigger (PnAutoTrigger *trigger)
{
    PnLoad      *self     = PN_LOAD (trigger);
    PnNode      *node     = PN_NODE (self);
    gchar       *hostname = load_dup_hostname_locked (self);
    gchar       *raw      = NULL;
    gchar       *errbuf   = NULL;
    GError      *error    = NULL;
    PnMessage   *msg;
    gboolean     success  = FALSE;
    gdouble      l1 = 0.0, l5 = 0.0, l15 = 0.0;
    gboolean     local    = hostname_is_local (hostname);
    /* When the configured hostname is empty / "localhost" we still want
     * downstream nodes to see the real machine name on data.host (and
     * in the summary string), so a worksheet rendered on host A vs
     * host B is self-describing rather than always saying "localhost". */
    const gchar *display  = local ? g_get_host_name () : hostname;
    guint        period;
    guint        timeout;

    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    if (local)
    {
        success = sample_local (&raw, &error);
    }
    else
    {
        success = sample_ssh (hostname, timeout, &raw, &errbuf, &error);
    }

    if (success)
        success = pn_load_parse_loadavg (raw, &l1, &l5, &l15);

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",    display);
    pn_message_set_boolean (msg, "success", success);

    if (success)
    {
        pn_message_set_double (msg, "value", l1);
        pn_message_set_double (msg, "load5",  l5);
        pn_message_set_double (msg, "load15", l15);

        {
            gchar *summary = g_strdup_printf (
                    "Load on %s is %.2f.",
                    display, l1);
            pn_message_set_string (msg, "output", summary);
            g_free (summary);
        }
    }
    else
    {
        /* Surface a single-line reason so a downstream Debug Print
         * tells the user what went wrong without them having to
         * spelunk in the journal.  Prefer GError, fall back to the
         * remote stderr (often "Permission denied (publickey)."). */
        const gchar *reason = NULL;
        gchar       *summary;

        /* Prefer the remote stderr over the GError text: the GError
         * for a non-zero exit is the generic "Child process exited
         * with code N", whereas stderr typically carries the actual
         * cause ("Permission denied (publickey).",
         * "ssh: connect to host X port 22: Connection refused"). */
        if (errbuf != NULL && *errbuf != '\0')
            reason = g_strchomp (errbuf);
        else if (error != NULL)
            reason = error->message;

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read load from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read load from %s.",
                    display);

        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_clear_error (&error);
    g_free (raw);
    g_free (errbuf);
    g_free (hostname);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_load_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnLoad *self = PN_LOAD (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, load_dup_hostname_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_load_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnLoad *self = PN_LOAD (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        load_set_hostname (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_load_finalize (GObject *object)
{
    PnLoad *self = PN_LOAD (object);

    g_clear_pointer (&self->hostname, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_load_parent_class)->finalize (object);
}

static void
pn_load_class_init (PnLoadClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_load_get_property;
    object_class->set_property = pn_load_set_property;
    object_class->finalize     = pn_load_finalize;
    trigger_class->trigger     = pn_load_trigger;

    node_class->palette_icon   = PN_LOAD_ICON;
    node_class->class_name     = "System Load";
    node_class->icon           = PN_LOAD_ICON;
    node_class->color          = (GdkRGBA){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose /proc/loadavg should be sampled.  An empty "
            "string or \"localhost\" reads the local file directly; "
            "any other value is treated as an SSH target and the "
            "sample is fetched via passwordless ssh.",
            PN_LOAD_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_load_init (PnLoad *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname = g_strdup (PN_LOAD_DEFAULT_HOST);

    pn_node_set_class_name (node, "System Load");
    pn_node_set_icon       (node, PN_LOAD_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Sampling more than once a second offers no real signal — the
     * kernel only updates loadavg every 5s anyway — and keeps a
     * fresh node from hammering an SSH target on first paint. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_LOAD_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnLoad *
pn_load_new (void)
{
    return g_object_new (PN_TYPE_LOAD, NULL);
}
