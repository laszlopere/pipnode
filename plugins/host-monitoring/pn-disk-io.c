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

#include "pn-disk-io.h"
#include "pn-message.h"
#include "pn-ssh-profile.h"

#include <string.h>

/* fa-hdd-o U+F0A0 — a hard-drive glyph reads as "disk I/O".  In the
 * FA-4 range the bundled icon font supports (the same FA-5 ceiling
 * the shell-plugin icons document applies here). */
#define PN_DISK_IO_ICON           "\xef\x82\xa0"

#define PN_DISK_IO_DEFAULT_PERIOD 2u
/* See PN_LOAD_DEFAULT_HOST -- empty is the canonical "this machine"
 * marker; the dialog shows g_get_host_name() as a gray placeholder
 * hint and the trigger emits that same name on data.host. */
#define PN_DISK_IO_DEFAULT_HOST   ""
#define PN_DISK_IO_DEFAULT_DEVICE ""
#define PN_DISK_IO_DEFAULT_UNIT   PN_DISK_IO_UNIT_GBYTE

/* The kernel reports /proc/diskstats sector counts in 512-byte units
 * regardless of the device's physical block size — see Documentation/
 * iostats.txt: "the number of sectors read/written, in 512-byte units".
 * This is the conversion factor the throughput math relies on. */
#define PN_DISK_IO_SECTOR_BYTES   512u

struct _PnDiskIo
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @hostname, @device and @unit while the
     * main thread may overwrite them via the property setters, so
     * accesses are serialised through @mutex.  The same mutex also
     * guards the cumulative-sector state below: a hostname or device
     * change invalidates the previous sample (otherwise we'd compute
     * a delta against the wrong machine or the wrong row), so the
     * setter resets @have_prev under the lock and the trigger reads
     * it under the lock too.  A unit change does *not* invalidate
     * the baseline -- the bytes/sec computation is the same, only
     * the divisor differs.  */
    GMutex        mutex;
    gchar        *hostname;
    gchar        *device;
    PnDiskIoUnit  unit;

    /* Cumulative sector counters from the previous tick, in 512-byte
     * units exactly as /proc/diskstats reports them.  @have_prev is
     * %FALSE on the very first tick and after any hostname/device
     * change, so the trigger emits a zero-throughput "warming up"
     * sample rather than fabricating a delta against bogus state. */
    guint64  prev_sectors_read;
    guint64  prev_sectors_written;
    gint64   prev_time_us;
    gboolean have_prev;
};

G_DEFINE_TYPE (PnDiskIo, pn_disk_io, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_DEVICE,
    PROP_UNIT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Unit enum GType + per-value divisor/label                          */
/* ------------------------------------------------------------------ */

GType
pn_disk_io_unit_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The enum nicks ("B/sec", "KByte/sec", ...) are what the
         * JSON serialiser writes to disk via gvalue_to_json's enum
         * branch (lib/pn-flow.c) and what the auto-generated combo
         * box surfaces in the dialog, so they double as the on-wire
         * data.unit strings the trigger pulls back out of the
         * lookup tables below. */
        static const GEnumValue values[] = {
            { PN_DISK_IO_UNIT_BYTE,
              "PN_DISK_IO_UNIT_BYTE",  "B/sec"     },
            { PN_DISK_IO_UNIT_KBYTE,
              "PN_DISK_IO_UNIT_KBYTE", "KByte/sec" },
            { PN_DISK_IO_UNIT_MBYTE,
              "PN_DISK_IO_UNIT_MBYTE", "MByte/sec" },
            { PN_DISK_IO_UNIT_GBYTE,
              "PN_DISK_IO_UNIT_GBYTE", "GByte/sec" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnDiskIoUnit", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* Divisor in bytes that converts a raw bytes/sec figure into the
 * configured unit, and the matching short label.  Both tables are
 * indexed by #PnDiskIoUnit; out-of-range values fall back to the
 * GByte row, which matches the property default. */
static gdouble
unit_divisor (PnDiskIoUnit unit)
{
    switch (unit)
    {
    case PN_DISK_IO_UNIT_BYTE:  return 1.0;
    case PN_DISK_IO_UNIT_KBYTE: return 1.0e3;
    case PN_DISK_IO_UNIT_MBYTE: return 1.0e6;
    case PN_DISK_IO_UNIT_GBYTE: return 1.0e9;
    }
    return 1.0e9;
}

static const gchar *
unit_label (PnDiskIoUnit unit)
{
    switch (unit)
    {
    case PN_DISK_IO_UNIT_BYTE:  return "B/sec";
    case PN_DISK_IO_UNIT_KBYTE: return "KByte/sec";
    case PN_DISK_IO_UNIT_MBYTE: return "MByte/sec";
    case PN_DISK_IO_UNIT_GBYTE: return "GByte/sec";
    }
    return "GByte/sec";
}

/* ------------------------------------------------------------------ */
/*  Property accessors (thread-safe)                                   */
/* ------------------------------------------------------------------ */

static gchar *
disk_io_dup_hostname_locked (PnDiskIo *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static gchar *
disk_io_dup_device_locked (PnDiskIo *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->device);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static PnDiskIoUnit
disk_io_get_unit_locked (PnDiskIo *self)
{
    PnDiskIoUnit value;

    g_mutex_lock (&self->mutex);
    value = self->unit;
    g_mutex_unlock (&self->mutex);

    return value;
}

static void
disk_io_set_unit (
        PnDiskIo     *self,
        PnDiskIoUnit  unit)
{
    g_mutex_lock (&self->mutex);
    self->unit = unit;
    g_mutex_unlock (&self->mutex);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_UNIT]);
}

static void
disk_io_set_hostname (
        PnDiskIo    *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_DISK_IO_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    /* The previous sample was taken from a different host; the next
     * delta would be meaningless if we kept it. */
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static void
disk_io_set_device (
        PnDiskIo    *self,
        const gchar *device)
{
    gchar *old;
    gchar *replacement;

    replacement = g_strdup ((device != NULL) ? device
                                             : PN_DISK_IO_DEFAULT_DEVICE);

    g_mutex_lock (&self->mutex);
    old = self->device;
    self->device = replacement;
    /* Same reason as in disk_io_set_hostname: the next delta would
     * subtract counters for a different row of /proc/diskstats. */
    self->have_prev = FALSE;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DEVICE]);
}

static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  /proc/diskstats parsing                                            */
/*                                                                     */
/*  Per-line format (man proc, "iostats"):                             */
/*    major minor name reads merged sectors_read time_reading          */
/*    writes merged sectors_written time_writing iops_in_flight        */
/*    time_io weighted_time_io [discards ...]                          */
/*  Only the device name (column 3) and the two cumulative sector      */
/*  counters (columns 6 and 10, 1-indexed) are consumed here.          */
/* ------------------------------------------------------------------ */

/* TRUE when @name is a pseudo-device whose I/O figures would skew the
 * "all disks" sum into noise: loop/ram/zram/sr/fd contribute boot-
 * time activity that has nothing to do with the host's real disks. */
static gboolean
is_pseudo_device (const gchar *name)
{
    return g_str_has_prefix (name, "loop")
        || g_str_has_prefix (name, "ram")
        || g_str_has_prefix (name, "zram")
        || g_str_has_prefix (name, "sr")
        || g_str_has_prefix (name, "fd");
}

/* TRUE when @name is a partition of another device that also appears
 * in /proc/diskstats.  Summing partitions on top of their parent disk
 * would double-count every byte, so the "all disks" path skips these.
 *
 * The kernel exposes the same I/O for the whole disk and its
 * partitions in separate rows; the parent row already aggregates the
 * partition rows.  Heuristic by name pattern (the conventional kernel
 * naming, stable across the last two decades of block-driver
 * additions):
 *   sd[a-z]+[0-9]+        sda1, sdb12
 *   hd[a-z]+[0-9]+        hda1
 *   vd[a-z]+[0-9]+        vda1   (virtio)
 *   xvd[a-z]+[0-9]+       xvda1  (xen)
 *   nvme[0-9]+n[0-9]+p[0-9]+   nvme0n1p1
 *   mmcblk[0-9]+p[0-9]+        mmcblk0p1
 */
static gboolean
is_partition (const gchar *name)
{
    const gchar *p;
    gsize        len;

    len = strlen (name);
    if (len == 0)
        return FALSE;

    /* nvme0n1p1 / mmcblk0p1: needs a 'p' followed by digits at the
     * end. Both prefixes share that shape. */
    if (g_str_has_prefix (name, "nvme") || g_str_has_prefix (name, "mmcblk"))
    {
        p = strrchr (name, 'p');
        if (p == NULL || p == name)
            return FALSE;
        ++p;
        if (*p == '\0')
            return FALSE;
        for (; *p != '\0'; ++p)
            if (*p < '0' || *p > '9')
                return FALSE;
        return TRUE;
    }

    /* sda1 / hda1 / vda1 / xvda1: a letter-only stem followed by
     * trailing digits.  The classic SCSI-style "name ends in a
     * number" rule, scoped to the known prefixes so we do not
     * accidentally classify e.g. md0 (whole RAID device) as a
     * partition. */
    if (g_str_has_prefix (name, "sd")  ||
        g_str_has_prefix (name, "hd")  ||
        g_str_has_prefix (name, "vd")  ||
        g_str_has_prefix (name, "xvd"))
    {
        if (name[len - 1] < '0' || name[len - 1] > '9')
            return FALSE;
        return TRUE;
    }

    return FALSE;
}

/* Walk @text line-by-line and sum the cumulative sectors_read /
 * sectors_written across either the single device @want (when
 * non-empty) or every whole-disk row (otherwise).  Returns %TRUE iff
 * at least one matching row was found.
 *
 * The per-line parse uses g_ascii_strtoull rather than sscanf to stay
 * locale-independent and to avoid the well-known sscanf %llu portability
 * pitfalls on 32-bit hosts.
 */
/* Exposed (non-static) for unit testing — see tests/unit/test-pn-disk-io.c.
 * Kept out of the public header to keep the library surface clean; the
 * test declares a matching extern prototype. */
gboolean
pn_disk_io_parse_diskstats (
        const gchar  *text,
        const gchar  *want,
        guint64      *out_sectors_read,
        guint64      *out_sectors_written,
        gchar       **out_matched_label)
{
    const gchar *cursor = text;
    guint64      total_r = 0;
    guint64      total_w = 0;
    gboolean     found = FALSE;
    gboolean     want_all = (want == NULL) || (*want == '\0');
    GString     *matched;

    matched = g_string_new (NULL);

    while (cursor != NULL && *cursor != '\0')
    {
        const gchar *line_end;
        const gchar *p;
        gchar       *end;
        gchar        name[64];
        gsize        name_len;
        guint64      sectors_r;
        guint64      sectors_w;
        gint         i;

        line_end = strchr (cursor, '\n');

        p = cursor;
        cursor = (line_end != NULL) ? line_end + 1 : NULL;

        /* major */
        while (*p == ' ' || *p == '\t') ++p;
        (void) g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;
        p = end;

        /* minor */
        while (*p == ' ' || *p == '\t') ++p;
        (void) g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;
        p = end;

        /* name */
        while (*p == ' ' || *p == '\t') ++p;
        name_len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r' &&
               name_len + 1 < sizeof (name))
        {
            name[name_len++] = *p++;
        }
        if (name_len == 0) continue;
        name[name_len] = '\0';

        /* Skip the three already-parsed fields' worth of remaining
         * whitespace plus the next two integer columns (reads_completed
         * and reads_merged) to land on sectors_read. */
        for (i = 0; i < 2; ++i)
        {
            while (*p == ' ' || *p == '\t') ++p;
            (void) g_ascii_strtoull (p, &end, 10);
            if (end == p) { p = NULL; break; }
            p = end;
        }
        if (p == NULL) continue;

        /* sectors_read */
        while (*p == ' ' || *p == '\t') ++p;
        sectors_r = g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;
        p = end;

        /* Skip time_reading, writes_completed, writes_merged to reach
         * sectors_written. */
        for (i = 0; i < 3; ++i)
        {
            while (*p == ' ' || *p == '\t') ++p;
            (void) g_ascii_strtoull (p, &end, 10);
            if (end == p) { p = NULL; break; }
            p = end;
        }
        if (p == NULL) continue;

        /* sectors_written */
        while (*p == ' ' || *p == '\t') ++p;
        sectors_w = g_ascii_strtoull (p, &end, 10);
        if (end == p) continue;

        if (want_all)
        {
            if (is_pseudo_device (name)) continue;
            if (is_partition (name))     continue;
        }
        else
        {
            if (strcmp (name, want) != 0) continue;
        }

        total_r += sectors_r;
        total_w += sectors_w;
        found = TRUE;

        if (matched->len > 0)
            g_string_append_c (matched, '+');
        g_string_append (matched, name);
    }

    if (found)
    {
        *out_sectors_read    = total_r;
        *out_sectors_written = total_w;
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
    return g_file_get_contents ("/proc/diskstats", out_text, NULL, error);
}

static gboolean
sample_ssh (
        const gchar  *hostname,
        guint         timeout,
        gchar       **out_text,
        gchar       **out_stderr,
        GError      **error)
{
    const gchar *base_argv[] = { "cat", "/proc/diskstats", NULL };
    gchar      **argv;
    gint         exit_status = 0;
    gboolean     spawned;
    gboolean     ok = FALSE;

    /* Route through the one shared SSH argv builder (TODO #47.3) instead
     * of hand-rolling the ssh flag string.  No login profile is threaded
     * through yet (item 47.4), so pass NULL — the builder then emits the
     * same BatchMode / accept-new / ConnectTimeout argv this node used to
     * format inline, and g_spawn_sync runs it with no shell re-parse. */
    argv = pn_ssh_build_argv (hostname, NULL, timeout, base_argv);

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
pn_disk_io_trigger (PnAutoTrigger *trigger)
{
    PnDiskIo    *self     = PN_DISK_IO (trigger);
    PnNode      *node     = PN_NODE (self);
    gchar       *hostname = disk_io_dup_hostname_locked (self);
    gchar       *device   = disk_io_dup_device_locked (self);
    PnDiskIoUnit unit     = disk_io_get_unit_locked (self);
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
    guint64      sectors_r = 0;
    guint64      sectors_w = 0;
    gint64       now_us;
    gdouble      divisor   = unit_divisor (unit);
    const gchar *unit_str  = unit_label   (unit);
    gdouble      rate_read  = 0.0;
    gdouble      rate_write = 0.0;
    gdouble      rate_total = 0.0;
    gboolean     have_prev = FALSE;
    guint64      prev_r = 0;
    guint64      prev_w = 0;
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
        success = pn_disk_io_parse_diskstats (raw, device,
                                              &sectors_r, &sectors_w, &matched);

    now_us = g_get_monotonic_time ();

    /* Stage the snapshot under the lock: if a property setter races
     * us and resets @have_prev concurrently we want to honour that
     * reset rather than overwrite it.  The snapshot we read here is
     * the one we will use for the delta this tick. */
    g_mutex_lock (&self->mutex);
    have_prev = self->have_prev;
    if (have_prev)
    {
        prev_r  = self->prev_sectors_read;
        prev_w  = self->prev_sectors_written;
        prev_us = self->prev_time_us;
    }
    if (success)
    {
        self->prev_sectors_read    = sectors_r;
        self->prev_sectors_written = sectors_w;
        self->prev_time_us         = now_us;
        self->have_prev            = TRUE;
    }
    g_mutex_unlock (&self->mutex);

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_string  (msg, "device",
                            (matched != NULL) ? matched
                                              : (device != NULL ? device : ""));
    pn_message_set_boolean (msg, "success", success);

    if (success && have_prev && now_us > prev_us)
    {
        gdouble elapsed_s;
        guint64 delta_r;
        guint64 delta_w;

        elapsed_s = (gdouble) (now_us - prev_us) / 1.0e6;

        /* Counter wraparound on a 64-bit kernel counter is effectively
         * impossible in human-relevant timescales, but a hostname /
         * device change between samples would have cleared have_prev,
         * so the only way to reach a negative delta here is a counter
         * reset (e.g. the remote machine rebooted between ticks).
         * Clamp to zero rather than emit a garbage spike. */
        delta_r = (sectors_r >= prev_r) ? (sectors_r - prev_r) : 0;
        delta_w = (sectors_w >= prev_w) ? (sectors_w - prev_w) : 0;

        rate_read  = ((gdouble) delta_r * PN_DISK_IO_SECTOR_BYTES)
                     / elapsed_s / divisor;
        rate_write = ((gdouble) delta_w * PN_DISK_IO_SECTOR_BYTES)
                     / elapsed_s / divisor;
        rate_total = rate_read + rate_write;

        pn_message_set_double (msg, "value", rate_total);
        pn_message_set_double (msg, "read",  rate_read);
        pn_message_set_double (msg, "write", rate_write);
        pn_message_set_string (msg, "unit",  unit_str);

        {
            /* %.0f for raw bytes (sub-1 fractions are noise on a
             * counter that ticks in 512 B sectors), %.3f for the
             * three multiples (a GByte/sec figure typically lands in
             * the 0.00x – 1.xxx range; three fractional digits keeps
             * a 50 MB/s spike readable). */
            const gchar *fmt = (unit == PN_DISK_IO_UNIT_BYTE)
                ? "Disk I/O on %s [%s]: %.0f %s "
                  "(read %.0f, write %.0f)."
                : "Disk I/O on %s [%s]: %.3f %s "
                  "(read %.3f, write %.3f).";
            gchar *summary = g_strdup_printf (
                    fmt,
                    display,
                    (matched != NULL) ? matched : "all",
                    rate_total, unit_str, rate_read, rate_write);
            pn_message_set_string (msg, "output", summary);
            g_free (summary);
        }
    }
    else if (success && !have_prev)
    {
        /* First sample after construction / hostname change / device
         * change: we have no baseline to subtract against.  Emit a
         * zero reading so downstream graphs do not gap, with a hint
         * in data.output that the next tick will carry a real value. */
        pn_message_set_double (msg, "value", 0.0);
        pn_message_set_double (msg, "read",  0.0);
        pn_message_set_double (msg, "write", 0.0);
        pn_message_set_string (msg, "unit",  unit_str);

        {
            gchar *summary = g_strdup_printf (
                    "Disk I/O on %s [%s]: warming up "
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
        else if (raw != NULL && matched == NULL && device != NULL && *device != '\0')
            reason = "device not present in /proc/diskstats";
        else if (raw != NULL && matched == NULL)
            reason = "no whole-disk rows in /proc/diskstats";

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read disk I/O from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read disk I/O from %s.",
                    display);

        /* Surface success=false explicitly when parse_diskstats found
         * no matching row even though the file was read successfully
         * (e.g. user typed "sdx" but the host only has nvme0n1). */
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
    g_free (device);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_disk_io_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDiskIo *self = PN_DISK_IO (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, disk_io_dup_hostname_locked (self));
        break;
    case PROP_DEVICE:
        g_value_take_string (value, disk_io_dup_device_locked (self));
        break;
    case PROP_UNIT:
        g_value_set_enum (value, disk_io_get_unit_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_disk_io_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDiskIo *self = PN_DISK_IO (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        disk_io_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_DEVICE:
        disk_io_set_device (self, g_value_get_string (value));
        break;
    case PROP_UNIT:
        disk_io_set_unit (self, g_value_get_enum (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_disk_io_finalize (GObject *object)
{
    PnDiskIo *self = PN_DISK_IO (object);

    g_clear_pointer (&self->hostname, g_free);
    g_clear_pointer (&self->device,   g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_disk_io_parent_class)->finalize (object);
}

static void
pn_disk_io_class_init (PnDiskIoClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_disk_io_get_property;
    object_class->set_property = pn_disk_io_set_property;
    object_class->finalize     = pn_disk_io_finalize;
    trigger_class->trigger     = pn_disk_io_trigger;

    node_class->palette_icon   = PN_DISK_IO_ICON;
    node_class->class_name     = "Disk I/O";
    node_class->icon           = PN_DISK_IO_ICON;
    node_class->color          = (PnColor){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose /proc/diskstats should be sampled.  An empty "
            "string or \"localhost\" reads the local file directly; "
            "any other value is treated as an SSH target and the "
            "sample is fetched via passwordless ssh.",
            PN_DISK_IO_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Block-device name to measure (e.g. \"sda\", \"nvme0n1\", "
            "\"dm-0\").  An empty string sums across every whole-disk "
            "row of /proc/diskstats (partitions, loop, ram, sr and fd "
            "rows are skipped so a parent disk is not double-counted "
            "through its partitions).",
            PN_DISK_IO_DEFAULT_DEVICE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNIT] = g_param_spec_enum (
            "unit", "Unit",
            "Output unit for data.value: B/sec, KByte/sec, MByte/sec, "
            "or GByte/sec (decimal multiples, 1 KByte = 1000 bytes).  "
            "The matching short string is also emitted on data.unit "
            "so a downstream graph axis label can pick it up verbatim.",
            PN_TYPE_DISK_IO_UNIT,
            PN_DISK_IO_DEFAULT_UNIT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_disk_io_init (PnDiskIo *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname  = g_strdup (PN_DISK_IO_DEFAULT_HOST);
    self->device    = g_strdup (PN_DISK_IO_DEFAULT_DEVICE);
    self->unit      = PN_DISK_IO_DEFAULT_UNIT;
    self->have_prev = FALSE;

    pn_node_set_class_name (node, "Disk I/O");
    pn_node_set_icon       (node, PN_DISK_IO_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* 2 s is short enough to feel responsive in a graph and long
     * enough that the per-tick delta is well above the noise floor
     * the kernel rounds sector counts to. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_DISK_IO_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDiskIo *
pn_disk_io_new (void)
{
    return g_object_new (PN_TYPE_DISK_IO, NULL);
}
