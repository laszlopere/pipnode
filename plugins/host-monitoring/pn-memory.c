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

#include "pn-memory.h"
#include "pn-message.h"

#include <string.h>

/* fa-th-large U+F009 -- a 2x2 grid glyph that reads as "memory
 * cells".  In the FA-4 range the bundled icon font supports (same
 * FA-5 caveat as the other host-monitoring icons).  fa-microchip is
 * already claimed by #PnCpu so we needed a visually distinct glyph;
 * the alternative fa-database reads as "disk storage" and would
 * collide semantically with #PnDiskIo. */
#define PN_MEMORY_ICON           "\xef\x80\x89"

#define PN_MEMORY_DEFAULT_PERIOD 2u
/* See PN_LOAD_DEFAULT_HOST -- empty is the canonical "this machine"
 * marker; the dialog shows g_get_host_name() as a gray placeholder
 * hint and the trigger emits that same name on data.host. */
#define PN_MEMORY_DEFAULT_HOST   ""
#define PN_MEMORY_DEFAULT_UNIT   PN_MEMORY_UNIT_PERCENT

struct _PnMemory
{
    PnAutoTrigger parent_instance;

    /* @hostname and @unit are touched from both the worker thread
     * (in the trigger) and the main thread (via property setters),
     * so the mutex serialises access.  No previous-sample state is
     * needed -- the figure is an instantaneous snapshot from
     * /proc/meminfo, not a per-second delta, so each tick stands
     * alone and a hostname change does not warm-up the next tick. */
    GMutex        mutex;
    gchar        *hostname;
    PnMemoryUnit  unit;
};

G_DEFINE_TYPE (PnMemory, pn_memory, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_UNIT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Unit enum GType + per-value divisor/label                          */
/* ------------------------------------------------------------------ */

GType
pn_memory_unit_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_MEMORY_UNIT_PERCENT,
              "PN_MEMORY_UNIT_PERCENT", "%"     },
            { PN_MEMORY_UNIT_BYTE,
              "PN_MEMORY_UNIT_BYTE",    "B"     },
            { PN_MEMORY_UNIT_KBYTE,
              "PN_MEMORY_UNIT_KBYTE",   "KByte" },
            { PN_MEMORY_UNIT_MBYTE,
              "PN_MEMORY_UNIT_MBYTE",   "MByte" },
            { PN_MEMORY_UNIT_GBYTE,
              "PN_MEMORY_UNIT_GBYTE",   "GByte" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnMemoryUnit", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* Binary multiples: a "32 GByte" RAM stick is universally understood
 * as 32 GiB (2^35 bytes), so a node labelled `GByte` reporting the
 * decimal-SI form (~30 GiB) would surprise every user.  Documented in
 * data/help/PnMemory.html so the divergence from the I/O nodes'
 * decimal convention is explicit. */
static gdouble
unit_divisor (PnMemoryUnit unit)
{
    switch (unit)
    {
    case PN_MEMORY_UNIT_PERCENT: return 1.0;  /* not used in % mode */
    case PN_MEMORY_UNIT_BYTE:    return 1.0;
    case PN_MEMORY_UNIT_KBYTE:   return 1024.0;
    case PN_MEMORY_UNIT_MBYTE:   return 1024.0 * 1024.0;
    case PN_MEMORY_UNIT_GBYTE:   return 1024.0 * 1024.0 * 1024.0;
    }
    return 1.0;
}

static const gchar *
unit_label (PnMemoryUnit unit)
{
    switch (unit)
    {
    case PN_MEMORY_UNIT_PERCENT: return "%";
    case PN_MEMORY_UNIT_BYTE:    return "B";
    case PN_MEMORY_UNIT_KBYTE:   return "KByte";
    case PN_MEMORY_UNIT_MBYTE:   return "MByte";
    case PN_MEMORY_UNIT_GBYTE:   return "GByte";
    }
    return "%";
}

/* ------------------------------------------------------------------ */
/*  Property accessors (thread-safe)                                   */
/* ------------------------------------------------------------------ */

static gchar *
memory_dup_hostname_locked (PnMemory *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static PnMemoryUnit
memory_get_unit_locked (PnMemory *self)
{
    PnMemoryUnit value;

    g_mutex_lock (&self->mutex);
    value = self->unit;
    g_mutex_unlock (&self->mutex);

    return value;
}

static void
memory_set_hostname (
        PnMemory    *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_MEMORY_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static void
memory_set_unit (
        PnMemory     *self,
        PnMemoryUnit  unit)
{
    g_mutex_lock (&self->mutex);
    self->unit = unit;
    g_mutex_unlock (&self->mutex);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_UNIT]);
}

static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/meminfo parsing                                              */
/*                                                                     */
/*  Line format is "<Field>:<whitespace><number> kB" -- the trailing  */
/*  "kB" is documented as kibibytes (1024 bytes) despite the lower-   */
/*  case `kB` suffix the kernel uses, so we multiply each value by    */
/*  1024 to land on bytes.                                             */
/* ------------------------------------------------------------------ */

/* Returns TRUE iff the line begins with @key followed by ':' and an
 * integer.  Captures the integer (in kibibytes) into @out_kib. */
static gboolean
line_pull_kib (
        const gchar  *line,
        const gchar  *key,
        guint64      *out_kib)
{
    gsize        key_len;
    const gchar *p;
    gchar       *end;

    key_len = strlen (key);
    if (strncmp (line, key, key_len) != 0)
        return FALSE;
    p = line + key_len;
    if (*p != ':')
        return FALSE;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    *out_kib = g_ascii_strtoull (p, &end, 10);
    if (end == p)
        return FALSE;
    return TRUE;
}

/* Exposed (non-static) for unit testing — see tests/unit/test-pn-memory.c.
 * Kept out of the public header to keep the library surface clean; the
 * test declares a matching extern prototype. */
gboolean
pn_memory_parse_meminfo (
        const gchar  *text,
        guint64      *out_total_bytes,
        guint64      *out_available_bytes,
        guint64      *out_free_bytes)
{
    const gchar *cursor = text;
    guint64      total = 0, available = 0, free_bytes = 0;
    gboolean     have_total = FALSE;
    gboolean     have_available = FALSE;
    gboolean     have_free = FALSE;

    while (cursor != NULL && *cursor != '\0')
    {
        const gchar *line_end;
        guint64      v;

        line_end = strchr (cursor, '\n');

        if (!have_total && line_pull_kib (cursor, "MemTotal", &v))
        {
            total = v;
            have_total = TRUE;
        }
        else if (!have_available && line_pull_kib (cursor, "MemAvailable", &v))
        {
            available = v;
            have_available = TRUE;
        }
        else if (!have_free && line_pull_kib (cursor, "MemFree", &v))
        {
            free_bytes = v;
            have_free = TRUE;
        }

        cursor = (line_end != NULL) ? line_end + 1 : NULL;

        if (have_total && have_available && have_free)
            break;
    }

    if (!have_total || !have_available)
        return FALSE;

    /* /proc/meminfo reports kibibytes (1024 bytes per unit) despite
     * the "kB" suffix; convert once on the way out so the rest of
     * the code can stay in bytes. */
    *out_total_bytes     = total      * 1024;
    *out_available_bytes = available  * 1024;
    *out_free_bytes      = free_bytes * 1024;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Sample acquisition                                                 */
/* ------------------------------------------------------------------ */

static gboolean
sample_local (
        gchar  **out_text,
        GError **error)
{
    return g_file_get_contents ("/proc/meminfo", out_text, NULL, error);
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
    cmd = g_strdup_printf (
            "ssh -o BatchMode=yes "
            "-o ConnectTimeout=%u "
            "-o StrictHostKeyChecking=accept-new "
            "%s cat /proc/meminfo",
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
pn_memory_trigger (PnAutoTrigger *trigger)
{
    PnMemory    *self      = PN_MEMORY (trigger);
    PnNode      *node      = PN_NODE (self);
    gchar       *hostname  = memory_dup_hostname_locked (self);
    PnMemoryUnit unit      = memory_get_unit_locked (self);
    gchar       *raw       = NULL;
    gchar       *errbuf    = NULL;
    GError      *error     = NULL;
    PnMessage   *msg;
    gboolean     success   = FALSE;
    gboolean     local     = hostname_is_local (hostname);
    /* Empty / "localhost" both resolve to the actual machine name on
     * the wire and in the summary -- same convention as #PnLoad. */
    const gchar *display   = local ? g_get_host_name () : hostname;
    guint        period;
    guint        timeout;
    guint64      total_b = 0, available_b = 0, free_b = 0;
    guint64      used_b  = 0;
    gdouble      percent = 0.0;
    gdouble      value   = 0.0;
    gdouble      divisor = unit_divisor (unit);
    const gchar *unit_str = unit_label (unit);

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
        success = pn_memory_parse_meminfo (raw, &total_b, &available_b, &free_b);

    if (success && total_b > 0)
    {
        used_b  = (total_b >= available_b) ? (total_b - available_b) : 0;
        percent = 100.0 * (gdouble) used_b / (gdouble) total_b;
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_boolean (msg, "success", success);

    if (success && total_b > 0)
    {
        if (unit == PN_MEMORY_UNIT_PERCENT)
        {
            value = percent;
            pn_message_set_double (msg, "value", value);
            pn_message_set_double (msg, "used",      percent);
            /* Keep the same field shape across modes: in % mode the
             * absolute-byte fields are still meaningful but reported
             * as percentages so a downstream consumer reading
             * data.total / data.used / data.free in the configured
             * unit gets sensible numbers without having to look at
             * data.unit first. */
            pn_message_set_double (msg, "total",     100.0);
            pn_message_set_double (msg, "available",
                                   100.0 - percent);
            pn_message_set_double (msg, "free",
                                   total_b > 0
                                   ? 100.0 * (gdouble) free_b /
                                             (gdouble) total_b
                                   : 0.0);
        }
        else
        {
            value = (gdouble) used_b / divisor;
            pn_message_set_double (msg, "value",     value);
            pn_message_set_double (msg, "used",      value);
            pn_message_set_double (msg, "total",
                                  (gdouble) total_b     / divisor);
            pn_message_set_double (msg, "available",
                                  (gdouble) available_b / divisor);
            pn_message_set_double (msg, "free",
                                  (gdouble) free_b      / divisor);
        }

        /* data.percent is always the percentage regardless of the
         * configured unit, so a downstream filter or graph that
         * wants the saturation curve does not have to special-case
         * the % unit. */
        pn_message_set_double (msg, "percent", percent);
        pn_message_set_string (msg, "unit",    unit_str);

        {
            const gchar *fmt;
            gchar       *summary;

            if (unit == PN_MEMORY_UNIT_PERCENT)
            {
                /* In % mode show the percentage breakdown; the
                 * absolute total is appended in GiByte for context
                 * so a 70% reading is intelligible without reading
                 * data.total. */
                summary = g_strdup_printf (
                        "Memory on %s: %.1f%% used "
                        "(%.2f GByte total).",
                        display,
                        percent,
                        (gdouble) total_b /
                        (1024.0 * 1024.0 * 1024.0));
            }
            else
            {
                fmt = (unit == PN_MEMORY_UNIT_BYTE)
                    ? "Memory on %s: %.0f %s used / %.0f %s total (%.1f%%)."
                    : "Memory on %s: %.2f %s used / %.2f %s total (%.1f%%).";
                summary = g_strdup_printf (
                        fmt,
                        display,
                        value, unit_str,
                        (gdouble) total_b / divisor, unit_str,
                        percent);
            }

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
        else if (raw != NULL && !success)
            reason = "MemTotal / MemAvailable missing from /proc/meminfo";

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read memory from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read memory from %s.",
                    display);

        pn_message_set_boolean (msg, "success", FALSE);
        pn_message_set_string  (msg, "output",  summary);
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
pn_memory_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnMemory *self = PN_MEMORY (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, memory_dup_hostname_locked (self));
        break;
    case PROP_UNIT:
        g_value_set_enum (value, memory_get_unit_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_memory_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnMemory *self = PN_MEMORY (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        memory_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_UNIT:
        memory_set_unit (self, g_value_get_enum (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_memory_finalize (GObject *object)
{
    PnMemory *self = PN_MEMORY (object);

    g_clear_pointer (&self->hostname, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_memory_parent_class)->finalize (object);
}

static void
pn_memory_class_init (PnMemoryClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_memory_get_property;
    object_class->set_property = pn_memory_set_property;
    object_class->finalize     = pn_memory_finalize;
    trigger_class->trigger     = pn_memory_trigger;

    node_class->palette_icon   = PN_MEMORY_ICON;
    node_class->class_name     = "Memory";
    node_class->icon           = PN_MEMORY_ICON;
    node_class->color          = (PnColor){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose /proc/meminfo should be sampled.  An empty "
            "string or \"localhost\" reads the local file directly; "
            "any other value is treated as an SSH target and the "
            "sample is fetched via passwordless ssh.",
            PN_MEMORY_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_UNIT] = g_param_spec_enum (
            "unit", "Unit",
            "Output unit for data.value: % (percentage of MemTotal "
            "that is currently in use, the default), or one of B / "
            "KByte / MByte / GByte for the absolute used-byte count "
            "in *binary* multiples (1 KByte = 1024 bytes -- the "
            "convention RAM vendors and tools like `free` / `htop` "
            "use, deliberately divergent from PnDiskIo / PnNetIo "
            "which use decimal-SI multiples for throughput).  The "
            "chosen label is emitted on data.unit; data.percent is "
            "always the percentage regardless of the configured unit.",
            PN_TYPE_MEMORY_UNIT,
            PN_MEMORY_DEFAULT_UNIT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_memory_init (PnMemory *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname = g_strdup (PN_MEMORY_DEFAULT_HOST);
    self->unit     = PN_MEMORY_DEFAULT_UNIT;

    pn_node_set_class_name (node, "Memory");
    pn_node_set_icon       (node, PN_MEMORY_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_MEMORY_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMemory *
pn_memory_new (void)
{
    return g_object_new (PN_TYPE_MEMORY, NULL);
}
