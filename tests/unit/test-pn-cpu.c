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

/* Unit tests for PnCpu's /proc/stat parser.  Canned kernel text stands
 * in for the live file (the "stub" for /proc).  The parser selects a row
 * ("" -> the aggregate "cpu" row, "N" -> "cpuN") and splits the eight
 * jiffy columns into busy / idle / iowait / total:
 *   busy  = user + nice + system + irq + softirq + steal
 *   idle  = idle + iowait
 *   total = busy + idle
 * guest / guest_nice are intentionally ignored (already folded into
 * user / nice by the kernel). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"

/* Defined (non-static) in lib/pn-cpu.c — see the note there. */
gboolean pn_cpu_parse_proc_stat (const gchar *text,
                                 const gchar *want_core,
                                 guint64 *out_busy,
                                 guint64 *out_idle,
                                 guint64 *out_iowait,
                                 guint64 *out_total,
                                 gchar **out_label);

/* Columns: user nice system idle iowait irq softirq steal.  "cpu12" is
 * placed before "cpu1" so the per-core test also proves the trailing
 * space anchors the match (cpu1 must not pick up cpu12). */
static const gchar STAT[] =
        "cpu  100 20 30 1000 50 5 5 0\n"
        "cpu12 1 1 1 1 1 1 1 1\n"
        "cpu1 90 20 25 800 45 5 5 0\n"
        "intr 123456\n";

static void
test_aggregate_row (void)
{
    guint64  busy = 0, idle = 0, iowait = 0, total = 0;
    gchar   *label = NULL;

    PN_CHECK (pn_cpu_parse_proc_stat (STAT, "",
                                      &busy, &idle, &iowait, &total, &label));
    PN_CHECK_CMPINT (busy,   ==, 100 + 20 + 30 + 5 + 5 + 0);   /* 160  */
    PN_CHECK_CMPINT (idle,   ==, 1000 + 50);                   /* 1050 */
    PN_CHECK_CMPINT (iowait, ==, 50);
    PN_CHECK_CMPINT (total,  ==, 160 + 1050);                  /* 1210 */
    PN_CHECK_CMPSTR (label,  ==, "cpu");

    g_free (label);
}

static void
test_per_core_anchored (void)
{
    guint64  busy = 0, idle = 0, iowait = 0, total = 0;
    gchar   *label = NULL;

    /* "1" -> "cpu1 ", which must skip the earlier "cpu12" row. */
    PN_CHECK (pn_cpu_parse_proc_stat (STAT, "1",
                                      &busy, &idle, &iowait, &total, &label));
    PN_CHECK_CMPINT (busy,  ==, 90 + 20 + 25 + 5 + 5 + 0);     /* 145 */
    PN_CHECK_CMPINT (total, ==, 145 + (800 + 45));             /* 990 */
    PN_CHECK_CMPSTR (label, ==, "cpu1");

    g_free (label);
}

static void
test_missing_core_fails (void)
{
    guint64  busy, idle, iowait, total;
    gchar   *label = (gchar *) 0x1;   /* must be cleared to NULL on miss */

    PN_CHECK_FALSE (pn_cpu_parse_proc_stat (STAT, "9",
                                            &busy, &idle, &iowait, &total,
                                            &label));
    PN_CHECK (label == NULL);
}

static void
test_garbage_fails (void)
{
    guint64  busy, idle, iowait, total;
    gchar   *label = NULL;

    PN_CHECK_FALSE (pn_cpu_parse_proc_stat ("intr 1\nctxt 2\n", "",
                                            &busy, &idle, &iowait, &total,
                                            &label));
    g_free (label);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-cpu");
    pn_test_add ("aggregate_row",          test_aggregate_row);
    pn_test_add ("per_core_anchored",      test_per_core_anchored);
    pn_test_add ("missing_core_fails",     test_missing_core_fails);
    pn_test_add ("garbage_fails",          test_garbage_fails);
    return pn_test_run ();
}
