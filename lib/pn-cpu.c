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

#include "pn-cpu.h"
#include "pn-message.h"

#include <string.h>

/* fa-microchip U+F2DB -- the canonical "CPU" glyph, added to FA-4 in
 * 4.7 (still well within the FA-4 ceiling the bundled icon font
 * supports; same FA-5 caveat as the other host-monitoring icons). */
#define PN_CPU_ICON           "\xef\x8b\x9b"

#define PN_CPU_DEFAULT_PERIOD 2u
/* See PN_LOAD_DEFAULT_HOST -- empty string is the canonical "this
 * machine" marker, resolved to g_get_host_name() for the emitted
 * data.host and rendered as a gray placeholder hint in the dialog. */
#define PN_CPU_DEFAULT_HOST   ""
#define PN_CPU_DEFAULT_CORE   ""

struct _PnCpu
{
    PnAutoTrigger parent_instance;

    /* @hostname and @core are touched from both the worker thread
     * (in the trigger) and the main thread (via property setters),
     * so the mutex serialises access.  The same mutex guards the
     * cumulative-jiffy state below: a hostname or core change
     * invalidates the previous sample (otherwise the delta would
     * subtract counters from the wrong machine or the wrong row),
     * so the setter resets @have_prev under the lock and the
     * trigger reads it under the lock too. */
    GMutex   mutex;
    gchar   *hostname;
    gchar   *core;

    /* Cumulative jiffy counters from the previous tick, decomposed
     * into the conventional "busy" and "idle" buckets the percentage
     * is computed from:
     *   busy  = user + nice + system + irq + softirq + steal
     *   idle  = idle + iowait
     *   total = busy + idle
     * The "guest" and "guest_nice" columns the kernel started
     * exposing in 2.6.24 / 2.6.33 are intentionally not double-
     * counted: they are already included in "user" / "nice" by
     * the kernel's accounting (see Documentation/filesystems/
     * proc.rst), and tools like top / htop follow the same rule. */
    guint64  prev_busy;
    guint64  prev_idle;
    guint64  prev_total;
    /* iowait split out separately so a downstream graph can show it
     * as its own series; emitted on data.iowait. */
    guint64  prev_iowait;
    gint64   prev_time_us;
    gboolean have_prev;
};

G_DEFINE_TYPE (PnCpu, pn_cpu, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_CORE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Property accessors (thread-safe)                                   */
/* ------------------------------------------------------------------ */

static gchar *
cpu_dup_hostname_locked (PnCpu *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static gchar *
cpu_dup_core_locked (PnCpu *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->core);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
cpu_set_hostname (
        PnCpu       *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_CPU_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static void
cpu_set_core (
        PnCpu       *self,
        const gchar *core)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((core != NULL) ? core : PN_CPU_DEFAULT_CORE);

    g_mutex_lock (&self->mutex);
    old = self->core;
    self->core = replacement;
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CORE]);
}

static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/stat parsing                                                 */
/*                                                                     */
/*  Each cpu line has the shape:                                       */
/*    cpu[<N>]  user nice system idle iowait irq softirq steal         */
/*              guest guest_nice                                       */
/*  All counters are in USER_HZ jiffies (typically 100 Hz on x86,      */
/*  250/300/1000 on other configs).  The percentage we compute is     */
/*  dimensionless -- it only cares about the ratio of deltas, so the  */
/*  absolute jiffy resolution drops out of the math.                   */
/* ------------------------------------------------------------------ */

static gboolean
parse_proc_stat (
        const gchar  *text,
        const gchar  *want_core,
        guint64      *out_busy,
        guint64      *out_idle,
        guint64      *out_iowait,
        guint64      *out_total,
        gchar       **out_label)
{
    const gchar *cursor = text;
    gchar        want[32];

    /* Build the row prefix we are looking for:
     *   ""  -> "cpu "   (aggregate row -- the kernel writes a single
     *                    space between "cpu" and the first jiffy)
     *   "0" -> "cpu0 "
     *   "1" -> "cpu1 "
     *   ...
     * Anchoring on the trailing space prevents "cpu1" from spuriously
     * matching the prefix of "cpu12". */
    if (want_core == NULL || *want_core == '\0')
    {
        g_strlcpy (want, "cpu ", sizeof (want));
    }
    else
    {
        g_snprintf (want, sizeof (want), "cpu%s ", want_core);
    }

    while (cursor != NULL && *cursor != '\0')
    {
        const gchar *line_end;
        const gchar *p;
        gchar       *end;
        guint64      fields[8];
        gint         i;

        line_end = strchr (cursor, '\n');

        p = cursor;
        cursor = (line_end != NULL) ? line_end + 1 : NULL;

        if (!g_str_has_prefix (p, want))
            continue;

        /* Step past the prefix to the first jiffy column. */
        p += strlen (want);
        while (*p == ' ' || *p == '\t') ++p;

        /* Pull eight columns: user, nice, system, idle, iowait,
         * irq, softirq, steal.  The guest / guest_nice columns
         * that follow are intentionally skipped -- the kernel
         * already includes them in user / nice, so adding them
         * would double-count. */
        for (i = 0; i < 8; ++i)
        {
            while (*p == ' ' || *p == '\t') ++p;
            fields[i] = g_ascii_strtoull (p, &end, 10);
            if (end == p)
                return FALSE;
            p = end;
        }

        /* fields[]: 0=user 1=nice 2=system 3=idle 4=iowait 5=irq
         *           6=softirq 7=steal */
        *out_busy = fields[0] + fields[1] + fields[2]
                  + fields[5] + fields[6] + fields[7];
        *out_idle = fields[3] + fields[4];
        *out_iowait = fields[4];
        *out_total = *out_busy + *out_idle;

        if (out_label != NULL)
        {
            /* The label trims the trailing space off @want so the
             * emitted data.core reads as "cpu" / "cpu0" / ... */
            *out_label = g_strndup (want, strlen (want) - 1);
        }

        return TRUE;
    }

    if (out_label != NULL)
        *out_label = NULL;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Sample acquisition                                                 */
/* ------------------------------------------------------------------ */

static gboolean
sample_local (
        gchar  **out_text,
        GError **error)
{
    return g_file_get_contents ("/proc/stat", out_text, NULL, error);
}

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
    /* `head -1` would be cheaper bytes-wise but we may also want a
     * per-core row, which is line N+1 of the file; just transfer the
     * whole thing -- it is at most a few KB even on a 128-core host. */
    cmd = g_strdup_printf (
            "ssh -o BatchMode=yes "
            "-o ConnectTimeout=%u "
            "-o StrictHostKeyChecking=accept-new "
            "%s cat /proc/stat",
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
/* ------------------------------------------------------------------ */

static void
pn_cpu_trigger (PnAutoTrigger *trigger)
{
    PnCpu       *self     = PN_CPU (trigger);
    PnNode      *node     = PN_NODE (self);
    gchar       *hostname = cpu_dup_hostname_locked (self);
    gchar       *core     = cpu_dup_core_locked     (self);
    gchar       *raw      = NULL;
    gchar       *errbuf   = NULL;
    gchar       *matched  = NULL;
    GError      *error    = NULL;
    PnMessage   *msg;
    gboolean     success  = FALSE;
    gboolean     local    = hostname_is_local (hostname);
    /* Empty / "localhost" both resolve to the actual machine name on
     * the wire and in the summary -- same convention as #PnLoad. */
    const gchar *display  = local ? g_get_host_name () : hostname;
    guint        period;
    guint        timeout;
    guint64      busy = 0, idle = 0, iowait = 0, total = 0;
    gint64       now_us;
    gdouble      pct_busy   = 0.0;
    gdouble      pct_idle   = 0.0;
    gdouble      pct_iowait = 0.0;
    gboolean     have_prev = FALSE;
    guint64      prev_busy = 0, prev_idle = 0, prev_total = 0, prev_iowait = 0;
    gint64       prev_us = 0;

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
        success = parse_proc_stat (raw, core,
                                   &busy, &idle, &iowait, &total, &matched);

    now_us = g_get_monotonic_time ();

    g_mutex_lock (&self->mutex);
    have_prev = self->have_prev;
    if (have_prev)
    {
        prev_busy   = self->prev_busy;
        prev_idle   = self->prev_idle;
        prev_total  = self->prev_total;
        prev_iowait = self->prev_iowait;
        prev_us     = self->prev_time_us;
    }
    if (success)
    {
        self->prev_busy    = busy;
        self->prev_idle    = idle;
        self->prev_total   = total;
        self->prev_iowait  = iowait;
        self->prev_time_us = now_us;
        self->have_prev    = TRUE;
    }
    g_mutex_unlock (&self->mutex);

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_string  (msg, "core",
                            (matched != NULL) ? matched
                                              : (core != NULL ? core : ""));
    pn_message_set_boolean (msg, "success", success);

    if (success && have_prev && total > prev_total)
    {
        guint64 delta_busy   = (busy   >= prev_busy)   ? (busy   - prev_busy)   : 0;
        guint64 delta_idle   = (idle   >= prev_idle)   ? (idle   - prev_idle)   : 0;
        guint64 delta_iowait = (iowait >= prev_iowait) ? (iowait - prev_iowait) : 0;
        guint64 delta_total  = total - prev_total;

        pct_busy   = 100.0 * (gdouble) delta_busy   / (gdouble) delta_total;
        pct_idle   = 100.0 * (gdouble) delta_idle   / (gdouble) delta_total;
        pct_iowait = 100.0 * (gdouble) delta_iowait / (gdouble) delta_total;

        pn_message_set_double (msg, "value",  pct_busy);
        pn_message_set_double (msg, "idle",   pct_idle);
        pn_message_set_double (msg, "iowait", pct_iowait);
        pn_message_set_string (msg, "unit",   "%");

        (void) prev_us;  /* prev_us is staged for future use; the
                          * delta is anchored on the kernel's own
                          * jiffy counters rather than wall clock so
                          * the per-tick math is intentionally
                          * independent of monotonic time. */

        {
            gchar *summary = g_strdup_printf (
                    "CPU on %s [%s]: %.1f%% busy "
                    "(idle %.1f%%, iowait %.1f%%).",
                    display,
                    (matched != NULL) ? matched : "cpu",
                    pct_busy, pct_idle, pct_iowait);
            pn_message_set_string (msg, "output", summary);
            g_free (summary);
        }
    }
    else if (success && !have_prev)
    {
        pn_message_set_double (msg, "value",  0.0);
        pn_message_set_double (msg, "idle",   0.0);
        pn_message_set_double (msg, "iowait", 0.0);
        pn_message_set_string (msg, "unit",   "%");

        {
            gchar *summary = g_strdup_printf (
                    "CPU on %s [%s]: warming up "
                    "(next tick has the first delta).",
                    display,
                    (matched != NULL) ? matched : "cpu");
            pn_message_set_string (msg, "output", summary);
            g_free (summary);
        }
    }
    else
    {
        const gchar *reason = NULL;
        gchar       *summary;

        if (errbuf != NULL && *errbuf != '\0')
            reason = g_strchomp (errbuf);
        else if (error != NULL)
            reason = error->message;
        else if (raw != NULL && matched == NULL && core != NULL && *core != '\0')
            reason = "core not present in /proc/stat";
        else if (raw != NULL && matched == NULL)
            reason = "no cpu row in /proc/stat";

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read CPU from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read CPU from %s.",
                    display);

        pn_message_set_boolean (msg, "success", FALSE);
        pn_message_set_string  (msg, "output",  summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_clear_error (&error);
    g_free (raw);
    g_free (errbuf);
    g_free (matched);
    g_free (hostname);
    g_free (core);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_cpu_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnCpu *self = PN_CPU (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, cpu_dup_hostname_locked (self));
        break;
    case PROP_CORE:
        g_value_take_string (value, cpu_dup_core_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_cpu_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnCpu *self = PN_CPU (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        cpu_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_CORE:
        cpu_set_core (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_cpu_finalize (GObject *object)
{
    PnCpu *self = PN_CPU (object);

    g_clear_pointer (&self->hostname, g_free);
    g_clear_pointer (&self->core,     g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_cpu_parent_class)->finalize (object);
}

static void
pn_cpu_class_init (PnCpuClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_cpu_get_property;
    object_class->set_property = pn_cpu_set_property;
    object_class->finalize     = pn_cpu_finalize;
    trigger_class->trigger     = pn_cpu_trigger;

    node_class->palette_icon   = PN_CPU_ICON;
    node_class->class_name     = "CPU";
    node_class->icon           = PN_CPU_ICON;
    node_class->color          = (GdkRGBA){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose /proc/stat should be sampled.  An empty "
            "string or \"localhost\" reads the local file directly; "
            "any other value is treated as an SSH target and the "
            "sample is fetched via passwordless ssh.",
            PN_CPU_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_CORE] = g_param_spec_string (
            "core", "Core",
            "Which /proc/stat row to measure: an empty string (the "
            "default) reads the aggregate \"cpu\" line so data.value "
            "represents the whole machine; a numeric value (\"0\", "
            "\"1\", ...) selects the matching per-core row (\"cpu0\", "
            "\"cpu1\", ...) so a worksheet can graph a single core "
            "in isolation.",
            PN_CPU_DEFAULT_CORE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_cpu_init (PnCpu *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname  = g_strdup (PN_CPU_DEFAULT_HOST);
    self->core      = g_strdup (PN_CPU_DEFAULT_CORE);
    self->have_prev = FALSE;

    pn_node_set_class_name (node, "CPU");
    pn_node_set_icon       (node, PN_CPU_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_CPU_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnCpu *
pn_cpu_new (void)
{
    return g_object_new (PN_TYPE_CPU, NULL);
}
