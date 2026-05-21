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

/* Unit tests for PnDiskIo's /proc/diskstats parser.  Canned kernel text
 * stands in for the live file (the "stub" for /proc).  Each row is
 * "major minor name ...", with sectors_read at field 3 after the name
 * and sectors_written at field 7.  An empty selector sums every
 * whole-disk row while skipping pseudo-devices (loop/ram/...) and
 * partitions (sda1) so their I/O is not double-counted; a named selector
 * matches exactly, partitions included. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"

/* Defined (non-static) in lib/pn-disk-io.c — see the note there. */
gboolean pn_disk_io_parse_diskstats (const gchar *text,
                                     const gchar *want,
                                     guint64 *out_sectors_read,
                                     guint64 *out_sectors_written,
                                     gchar **out_matched_label);

/* Fields after the name: reads_completed reads_merged SECTORS_READ
 * time_reading writes_completed writes_merged SECTORS_WRITTEN ... */
static const gchar DISKSTATS[] =
        "   8       0 sda 100 10 5000 200 50 5 3000 100 0 0 0\n"
        "   8       1 sda1 50 5 2500 100 25 2 1500 50 0 0 0\n"
        " 259       0 nvme0n1 200 20 8000 300 100 10 4000 150 0 0 0\n"
        "   7       0 loop0 1 0 8 0 0 0 0 0 0 0 0\n";

static void
test_sum_skips_pseudo_and_partitions (void)
{
    guint64  rd = 0, wr = 0;
    gchar   *label = NULL;

    /* Empty selector -> whole disks only: sda + nvme0n1, never sda1
     * (partition) or loop0 (pseudo). */
    PN_CHECK (pn_disk_io_parse_diskstats (DISKSTATS, "", &rd, &wr, &label));
    PN_CHECK_CMPINT (rd, ==, 5000 + 8000);
    PN_CHECK_CMPINT (wr, ==, 3000 + 4000);
    PN_CHECK_CMPSTR (label, ==, "sda+nvme0n1");

    g_free (label);
}

static void
test_named_whole_disk (void)
{
    guint64  rd = 0, wr = 0;
    gchar   *label = NULL;

    PN_CHECK (pn_disk_io_parse_diskstats (DISKSTATS, "sda", &rd, &wr, &label));
    PN_CHECK_CMPINT (rd, ==, 5000);
    PN_CHECK_CMPINT (wr, ==, 3000);
    PN_CHECK_CMPSTR (label, ==, "sda");

    g_free (label);
}

static void
test_named_partition_allowed (void)
{
    guint64  rd = 0, wr = 0;
    gchar   *label = NULL;

    /* The partition skip only applies to the "all disks" sum; an
     * explicit request for a partition still matches. */
    PN_CHECK (pn_disk_io_parse_diskstats (DISKSTATS, "sda1", &rd, &wr, &label));
    PN_CHECK_CMPINT (rd, ==, 2500);
    PN_CHECK_CMPINT (wr, ==, 1500);
    PN_CHECK_CMPSTR (label, ==, "sda1");

    g_free (label);
}

static void
test_unknown_device_fails (void)
{
    guint64  rd, wr;
    gchar   *label = (gchar *) 0x1;

    PN_CHECK_FALSE (pn_disk_io_parse_diskstats (DISKSTATS, "sdz",
                                                &rd, &wr, &label));
    PN_CHECK (label == NULL);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-disk-io");
    pn_test_add ("sum_skips_pseudo_part",  test_sum_skips_pseudo_and_partitions);
    pn_test_add ("named_whole_disk",       test_named_whole_disk);
    pn_test_add ("named_partition_ok",     test_named_partition_allowed);
    pn_test_add ("unknown_fails",          test_unknown_device_fails);
    return pn_test_run ();
}
