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

#include "pn-net-io.h"
#include "pn-message.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

#include <string.h>

/* fa-exchange U+F0EC — two horizontal arrows pointing in opposite
 * directions read as "bidirectional network traffic".  In the FA-4
 * range the bundled icon font supports (same FA-5 ceiling caveat as
 * the shell-plugin icons). */
#define PN_NET_IO_ICON              "\xef\x83\xac"

#define PN_NET_IO_DEFAULT_PERIOD    2u
/* See PN_LOAD_DEFAULT_HOST -- empty is the canonical "this machine"
 * marker; the dialog shows g_get_host_name() as a gray placeholder
 * hint and the trigger emits that same name on data.host. */
#define PN_NET_IO_DEFAULT_HOST      ""
#define PN_NET_IO_DEFAULT_INTERFACE ""
/* MByte/sec is the right default for a typical wired/wireless host
 * link in 2026: a gigabit ethernet maxes out around 125 MByte/sec
 * and a saturated Wi-Fi 6 link hits the same order, so the figure
 * lands in a readable 0.x -- 100.x range; GByte/sec would surface as
 * all-zeros for everyone except 10 GbE+ users, and KByte/sec would
 * overflow the dialog field on a real workload. */
#define PN_NET_IO_DEFAULT_UNIT      PN_NET_IO_UNIT_MBYTE

struct _PnNetIo
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @hostname, @interface and @unit while
     * the main thread may overwrite them via the property setters,
     * so accesses are serialised through @mutex.  The same mutex
     * also guards the cumulative-byte state below: a hostname or
     * interface change invalidates the previous sample (otherwise
     * we'd compute a delta against the wrong machine or the wrong
     * row), so the setter resets @have_prev under the lock and the
     * trigger reads it under the lock too.  A unit change does
     * *not* invalidate the baseline -- the bytes/sec computation is
     * the same, only the divisor differs. */
    GMutex       mutex;
    gchar       *hostname;
    gchar       *interface;
    PnNetIoUnit  unit;

    /* Cumulative byte counters from the previous tick, in bytes
     * exactly as /proc/net/dev reports them.  @have_prev is %FALSE
     * on the very first tick and after any hostname / interface
     * change, so the trigger emits a zero-throughput "warming up"
     * sample rather than fabricating a delta against bogus state. */
    guint64      prev_bytes_rx;
    guint64      prev_bytes_tx;
    gint64       prev_time_us;
    gboolean     have_prev;

    /* Optional SSH Login credential (item 47.4): @auth_profile is the
     * picked profile id (main thread only); @ssh_login is the value
     * snapshot the worker reads under @mutex, refreshed on the main
     * thread when the property or the vault changes. */
    gchar       *auth_profile;
    PnSshLogin   ssh_login;
};

G_DEFINE_TYPE (PnNetIo, pn_net_io, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_AUTH_PROFILE,
    PROP_HOSTNAME,
    PROP_INTERFACE,
    PROP_UNIT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Unit enum GType + per-value divisor/label                          */
/* ------------------------------------------------------------------ */

GType
pn_net_io_unit_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_NET_IO_UNIT_BYTE,
              "PN_NET_IO_UNIT_BYTE",  "B/sec"     },
            { PN_NET_IO_UNIT_KBYTE,
              "PN_NET_IO_UNIT_KBYTE", "KByte/sec" },
            { PN_NET_IO_UNIT_MBYTE,
              "PN_NET_IO_UNIT_MBYTE", "MByte/sec" },
            { PN_NET_IO_UNIT_GBYTE,
              "PN_NET_IO_UNIT_GBYTE", "GByte/sec" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnNetIoUnit", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

static gdouble
unit_divisor (PnNetIoUnit unit)
{
    switch (unit)
    {
    case PN_NET_IO_UNIT_BYTE:  return 1.0;
    case PN_NET_IO_UNIT_KBYTE: return 1.0e3;
    case PN_NET_IO_UNIT_MBYTE: return 1.0e6;
    case PN_NET_IO_UNIT_GBYTE: return 1.0e9;
    }
    return 1.0e6;
}

static const gchar *
unit_label (PnNetIoUnit unit)
{
    switch (unit)
    {
    case PN_NET_IO_UNIT_BYTE:  return "B/sec";
    case PN_NET_IO_UNIT_KBYTE: return "KByte/sec";
    case PN_NET_IO_UNIT_MBYTE: return "MByte/sec";
    case PN_NET_IO_UNIT_GBYTE: return "GByte/sec";
    }
    return "MByte/sec";
}

/* ------------------------------------------------------------------ */
/*  Property accessors (thread-safe)                                   */
/* ------------------------------------------------------------------ */

static gchar *
net_io_dup_hostname_locked (PnNetIo *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static gchar *
net_io_dup_interface_locked (PnNetIo *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->interface);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static PnNetIoUnit
net_io_get_unit_locked (PnNetIo *self)
{
    PnNetIoUnit value;

    g_mutex_lock (&self->mutex);
    value = self->unit;
    g_mutex_unlock (&self->mutex);

    return value;
}

static void
net_io_set_hostname (
        PnNetIo     *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_NET_IO_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static void
net_io_set_interface (
        PnNetIo     *self,
        const gchar *interface)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((interface != NULL) ? interface
                                                : PN_NET_IO_DEFAULT_INTERFACE);

    g_mutex_lock (&self->mutex);
    old = self->interface;
    self->interface = replacement;
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_INTERFACE]);
}

static void
net_io_set_unit (
        PnNetIo     *self,
        PnNetIoUnit  unit)
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
/*  /proc/net/dev parsing                                              */
/*                                                                     */
/*  Per-line format (after the two-line header):                       */
/*    <iface>: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame    */
/*             rx_compressed rx_multicast                              */
/*             tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls    */
/*             tx_carrier tx_compressed                                */
/*  The interface name is followed by a literal ':' (sometimes with    */
/*  surrounding whitespace) and 16 integer columns; columns 1 and 9   */
/*  (1-indexed past the colon) are the cumulative RX/TX byte counters  */
/*  this node consumes.                                                */
/* ------------------------------------------------------------------ */

/* TRUE when @name is the loopback interface, which the "all                 *
 * interfaces" sum skips by default to keep localhost-loop traffic from      *
 * inflating the figure.  The kernel only ever exposes one loopback so       *
 * an exact match is sufficient.                                              */
static gboolean
is_loopback (const gchar *name)
{
    return strcmp (name, "lo") == 0;
}

/* Exposed (non-static) for unit testing — see tests/unit/test-pn-net-io.c.
 * Kept out of the public header to keep the library surface clean; the
 * test declares a matching extern prototype. */
gboolean
pn_net_io_parse_net_dev (
        const gchar  *text,
        const gchar  *want,
        guint64      *out_rx,
        guint64      *out_tx,
        gchar       **out_matched_label)
{
    const gchar *cursor = text;
    guint64      total_rx = 0;
    guint64      total_tx = 0;
    gboolean     found = FALSE;
    gboolean     want_all = (want == NULL) || (*want == '\0');
    GString     *matched;
    gint         header_lines_skipped = 0;

    matched = g_string_new (NULL);

    while (cursor != NULL && *cursor != '\0')
    {
        const gchar *line_end;
        const gchar *p;
        const gchar *colon;
        gchar       *end;
        gchar        name[64];
        gsize        name_len;
        guint64      rx_bytes;
        guint64      tx_bytes;
        gint         i;

        line_end = strchr (cursor, '\n');

        p = cursor;
        cursor = (line_end != NULL) ? line_end + 1 : NULL;

        /* The first two lines are the human-readable header
         * ("Inter-|..." and "face |..."): neither contains a colon
         * followed by digits, but they do contain '|' which the
         * data lines do not.  Skipping by '|' presence is the
         * standard approach iftop / nload use. */
        if (header_lines_skipped < 2)
        {
            ++header_lines_skipped;
            continue;
        }

        /* Locate the interface-name terminator ':'.  A line without
         * a colon is malformed and skipped. */
        colon = NULL;
        for (const gchar *q = p; q != NULL && *q != '\0' && *q != '\n'; ++q)
        {
            if (*q == ':') { colon = q; break; }
        }
        if (colon == NULL) continue;

        /* Strip leading whitespace from the interface name. */
        while (p < colon && (*p == ' ' || *p == '\t')) ++p;
        name_len = 0;
        while (p < colon && name_len + 1 < sizeof (name))
            name[name_len++] = *p++;
        if (name_len == 0) continue;
        name[name_len] = '\0';

        /* Advance past the colon and any whitespace before the
         * first integer column. */
        p = colon + 1;

        /* rx_bytes (column 1 past the colon) */
        while (*p == ' ' || *p == '\t') ++p;
        rx_bytes = g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;
        p = end;

        /* Skip columns 2..8 (rx_packets, rx_errs, rx_drop, rx_fifo,
         * rx_frame, rx_compressed, rx_multicast) to land on column 9
         * which is tx_bytes. */
        for (i = 0; i < 7; ++i)
        {
            while (*p == ' ' || *p == '\t') ++p;
            (void) g_ascii_strtoull (p, &end, 10);
            if (end == p) { p = NULL; break; }
            p = end;
        }
        if (p == NULL) continue;

        /* tx_bytes (column 9 past the colon) */
        while (*p == ' ' || *p == '\t') ++p;
        tx_bytes = g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;

        if (want_all)
        {
            if (is_loopback (name)) continue;
        }
        else
        {
            if (strcmp (name, want) != 0) continue;
        }

        total_rx += rx_bytes;
        total_tx += tx_bytes;
        found = TRUE;

        if (matched->len > 0)
            g_string_append_c (matched, '+');
        g_string_append (matched, name);
    }

    if (found)
    {
        *out_rx = total_rx;
        *out_tx = total_tx;
        if (out_matched_label != NULL)
            *out_matched_label = g_string_free (matched, FALSE);
        else
            g_string_free (matched, TRUE);
    }
    else
    {
        g_string_free (matched, TRUE);
        if (out_matched_label != NULL)
            *out_matched_label = NULL;
    }

    return found;
}

/* ------------------------------------------------------------------ */
/*  Sample acquisition                                                 */
/* ------------------------------------------------------------------ */

static gboolean
sample_local (
        gchar  **out_text,
        GError **error)
{
    return g_file_get_contents ("/proc/net/dev", out_text, NULL, error);
}

static gboolean
sample_ssh (
        const gchar      *hostname,
        const PnSshLogin *login,
        guint             timeout,
        gchar           **out_text,
        gchar           **out_stderr,
        GError          **error)
{
    const gchar *base_argv[] = { "cat", "/proc/net/dev", NULL };
    gchar      **argv;
    gint         exit_status = 0;
    gboolean     spawned;
    gboolean     ok = FALSE;

    /* Route through the one shared SSH argv builder (TODO #47.3) instead
     * of hand-rolling the ssh flag string.  No login profile is threaded
     * through yet (item 47.4), so pass NULL — the builder then emits the
     * same BatchMode / accept-new / ConnectTimeout argv this node used to
     * format inline, and g_spawn_sync runs it with no shell re-parse. */
    argv = pn_ssh_build_argv (hostname, login, timeout, base_argv);

    spawned = g_spawn_sync (NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            out_text, out_stderr,
                            &exit_status, error);

    if (spawned)
        ok = g_spawn_check_wait_status (exit_status, error);

    g_strfreev (argv);

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

static void
pn_net_io_trigger (PnAutoTrigger *trigger)
{
    PnNetIo     *self      = PN_NET_IO (trigger);
    PnNode      *node      = PN_NODE (self);
    gchar       *hostname  = net_io_dup_hostname_locked (self);
    gchar       *interface = net_io_dup_interface_locked (self);
    PnNetIoUnit  unit      = net_io_get_unit_locked (self);
    gchar       *raw       = NULL;
    gchar       *errbuf    = NULL;
    gchar       *matched   = NULL;
    GError      *error     = NULL;
    PnMessage   *msg;
    gboolean     success   = FALSE;
    const gchar *effective;
    gboolean     local;
    /* Empty / "localhost" both resolve to the actual machine name on
     * the wire and in the summary -- same convention as #PnLoad. */
    const gchar *display;
    guint        period;
    guint        timeout;
    guint64      bytes_rx  = 0;
    guint64      bytes_tx  = 0;
    gint64       now_us;
    gdouble      divisor   = unit_divisor (unit);
    const gchar *unit_str  = unit_label   (unit);
    gdouble      rate_rx   = 0.0;
    gdouble      rate_tx   = 0.0;
    gdouble      rate_total = 0.0;
    gboolean     have_prev = FALSE;
    guint64      prev_rx   = 0;
    guint64      prev_tx   = 0;
    gint64       prev_us   = 0;
    PnSshLogin   login     = { 0 };

    /* Resolve the SSH-Login snapshot up front: a profile carrying its own
     * host pins WHERE we sample (mirroring pn_ssh_build_argv), so a node
     * left blank but pointing at such a credential reaches that host rather
     * than falling through to the local file. */
    pn_ssh_login_dup_locked (&self->mutex, &self->ssh_login, &login);
    effective = pn_ssh_login_effective_host (&login, hostname);
    local     = hostname_is_local (effective);
    display   = local ? g_get_host_name () : effective;

    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    if (local)
        success = sample_local (&raw, &error);
    else
        success = sample_ssh (effective, &login, timeout, &raw, &errbuf, &error);

    if (success)
        success = pn_net_io_parse_net_dev (raw, interface,
                                           &bytes_rx, &bytes_tx, &matched);

    now_us = g_get_monotonic_time ();

    g_mutex_lock (&self->mutex);
    have_prev = self->have_prev;
    if (have_prev)
    {
        prev_rx = self->prev_bytes_rx;
        prev_tx = self->prev_bytes_tx;
        prev_us = self->prev_time_us;
    }
    if (success)
    {
        self->prev_bytes_rx = bytes_rx;
        self->prev_bytes_tx = bytes_tx;
        self->prev_time_us  = now_us;
        self->have_prev     = TRUE;
    }
    g_mutex_unlock (&self->mutex);

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_string  (msg, "interface",
                            (matched != NULL) ? matched
                                              : (interface != NULL ? interface : ""));
    pn_message_set_boolean (msg, "success", success);

    if (success && have_prev && now_us > prev_us)
    {
        gdouble elapsed_s;
        guint64 delta_rx;
        guint64 delta_tx;

        elapsed_s = (gdouble) (now_us - prev_us) / 1.0e6;

        /* /proc/net/dev's byte counters are 64-bit on every kernel
         * the host is plausibly running, so wraparound is effectively
         * impossible.  A negative delta therefore implies the remote
         * machine rebooted or the interface was torn down and
         * recreated between samples; clamp to zero rather than emit
         * a garbage spike. */
        delta_rx = (bytes_rx >= prev_rx) ? (bytes_rx - prev_rx) : 0;
        delta_tx = (bytes_tx >= prev_tx) ? (bytes_tx - prev_tx) : 0;

        rate_rx = (gdouble) delta_rx / elapsed_s / divisor;
        rate_tx = (gdouble) delta_tx / elapsed_s / divisor;
        rate_total = rate_rx + rate_tx;

        pn_message_set_double (msg, "value", rate_total);
        pn_message_set_double (msg, "rx",    rate_rx);
        pn_message_set_double (msg, "tx",    rate_tx);
        pn_message_set_string (msg, "unit",  unit_str);

        {
            const gchar *fmt = (unit == PN_NET_IO_UNIT_BYTE)
                ? "Network I/O on %s [%s]: %.0f %s "
                  "(rx %.0f, tx %.0f)."
                : "Network I/O on %s [%s]: %.3f %s "
                  "(rx %.3f, tx %.3f).";
            gchar *summary = g_strdup_printf (
                    fmt,
                    display,
                    (matched != NULL) ? matched : "all",
                    rate_total, unit_str, rate_rx, rate_tx);
            pn_message_set_string (msg, "output", summary);
            g_free (summary);
        }
    }
    else if (success && !have_prev)
    {
        pn_message_set_double (msg, "value", 0.0);
        pn_message_set_double (msg, "rx",    0.0);
        pn_message_set_double (msg, "tx",    0.0);
        pn_message_set_string (msg, "unit",  unit_str);

        {
            gchar *summary = g_strdup_printf (
                    "Network I/O on %s [%s]: warming up "
                    "(next tick has the first delta).",
                    display,
                    (matched != NULL) ? matched : "all");
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
        else if (raw != NULL && matched == NULL && interface != NULL && *interface != '\0')
            reason = "interface not present in /proc/net/dev";
        else if (raw != NULL && matched == NULL)
            reason = "no non-loopback interfaces in /proc/net/dev";

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read network I/O from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read network I/O from %s.",
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
    g_free (interface);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_net_io_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnNetIo *self = PN_NET_IO (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, net_io_dup_hostname_locked (self));
        break;
    case PROP_INTERFACE:
        g_value_take_string (value, net_io_dup_interface_locked (self));
        break;
    case PROP_UNIT:
        g_value_set_enum (value, net_io_get_unit_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_net_io_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnNetIo *self = PN_NET_IO (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        net_io_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_INTERFACE:
        net_io_set_interface (self, g_value_get_string (value));
        break;
    case PROP_UNIT:
        net_io_set_unit (self, g_value_get_enum (value));
        break;
    case PROP_AUTH_PROFILE:
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (value);
        pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                              &self->mutex, &self->ssh_login);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* A profile changed in the vault: re-resolve our login snapshot. */
static void
on_vault_changed (PnNetIo *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_net_io_finalize (GObject *object)
{
    PnNetIo *self = PN_NET_IO (object);

    g_clear_pointer (&self->hostname,     g_free);
    g_clear_pointer (&self->interface,    g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_net_io_parent_class)->finalize (object);
}

static void
pn_net_io_class_init (PnNetIoClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_net_io_get_property;
    object_class->set_property = pn_net_io_set_property;
    object_class->finalize     = pn_net_io_finalize;
    trigger_class->trigger     = pn_net_io_trigger;

    node_class->palette_icon   = PN_NET_IO_ICON;
    node_class->class_name     = "Network I/O";
    node_class->icon           = PN_NET_IO_ICON;
    node_class->color          = (PnColor){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose /proc/net/dev should be sampled.  An empty "
            "string or \"localhost\" reads the local file directly; "
            "any other value is treated as an SSH target and the "
            "sample is fetched via passwordless ssh.",
            PN_NET_IO_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_INTERFACE] = g_param_spec_string (
            "interface", "Interface",
            "Network-interface name to measure (e.g. \"eth0\", "
            "\"wlan0\", \"enp86s0\").  An empty string sums across "
            "every non-loopback row of /proc/net/dev; the loopback "
            "row (\"lo\") is always skipped on the all-interfaces "
            "path so localhost-loop traffic does not inflate the "
            "figure.",
            PN_NET_IO_DEFAULT_INTERFACE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNIT] = g_param_spec_enum (
            "unit", "Unit",
            "Output unit for data.value: B/sec, KByte/sec, MByte/sec, "
            "or GByte/sec (decimal multiples, 1 KByte = 1000 bytes).  "
            "The matching short string is also emitted on data.unit "
            "so a downstream graph axis label can pick it up verbatim. "
            "Network throughput is reported in *bytes* per second "
            "(matching the kernel counter); multiply by 8 downstream "
            "for the bits-per-second (Mbps / Gbps) link-layer convention.",
            PN_TYPE_NET_IO_UNIT,
            PN_NET_IO_DEFAULT_UNIT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Optional SSH Login credential picker (item 47.4). */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_net_io_init (PnNetIo *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname     = g_strdup (PN_NET_IO_DEFAULT_HOST);
    self->interface    = g_strdup (PN_NET_IO_DEFAULT_INTERFACE);
    self->unit         = PN_NET_IO_DEFAULT_UNIT;
    self->have_prev    = FALSE;
    self->auth_profile = g_strdup ("");
    self->ssh_login    = (PnSshLogin){ 0 };

    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "Network I/O");
    pn_node_set_icon       (node, PN_NET_IO_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_NET_IO_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNetIo *
pn_net_io_new (void)
{
    return g_object_new (PN_TYPE_NET_IO, NULL);
}
