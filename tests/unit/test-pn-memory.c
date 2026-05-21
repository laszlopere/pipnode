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

/* Unit tests for PnMemory's /proc/meminfo parser.  Canned kernel text
 * stands in for the live file (the "stub" for /proc) so the test stays
 * headless and deterministic.  The parser pulls MemTotal / MemAvailable
 * / MemFree, reported by the kernel in kibibytes, and hands them back in
 * bytes.  MemTotal and MemAvailable are required; MemFree is optional. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"

/* Defined (non-static) in lib/pn-memory.c — see the note there. */
gboolean pn_memory_parse_meminfo (const gchar *text,
                                  guint64 *out_total_bytes,
                                  guint64 *out_available_bytes,
                                  guint64 *out_free_bytes);

/* A trimmed but realistically-shaped /proc/meminfo.  The "kB" suffix is
 * a kernel misnomer for kibibytes, so every value is multiplied by 1024
 * on the way out. */
static const gchar MEMINFO[] =
        "MemTotal:       16000 kB\n"
        "MemFree:         4000 kB\n"
        "MemAvailable:    8000 kB\n"
        "Buffers:          512 kB\n"
        "Cached:          2048 kB\n";

static void
test_parses_and_converts_to_bytes (void)
{
    guint64  total = 0, avail = 0, freeb = 0;

    PN_CHECK (pn_memory_parse_meminfo (MEMINFO, &total, &avail, &freeb));
    PN_CHECK_CMPINT (total, ==, (guint64) 16000 * 1024);
    PN_CHECK_CMPINT (avail, ==, (guint64)  8000 * 1024);
    PN_CHECK_CMPINT (freeb, ==, (guint64)  4000 * 1024);
}

static void
test_memfree_is_optional (void)
{
    guint64 total = 0, avail = 0, freeb = 123;

    /* Without a MemFree line the parse still succeeds on the two
     * required fields; the free output is simply left at zero. */
    PN_CHECK (pn_memory_parse_meminfo (
                  "MemTotal: 1000 kB\nMemAvailable: 500 kB\n",
                  &total, &avail, &freeb));
    PN_CHECK_CMPINT (total, ==, (guint64) 1000 * 1024);
    PN_CHECK_CMPINT (avail, ==, (guint64)  500 * 1024);
    PN_CHECK_CMPINT (freeb, ==, 0);
}

static void
test_requires_total_and_available (void)
{
    guint64 total, avail, freeb;

    /* MemTotal present but MemAvailable missing -> fail. */
    PN_CHECK_FALSE (pn_memory_parse_meminfo (
                        "MemTotal: 1000 kB\nMemFree: 100 kB\n",
                        &total, &avail, &freeb));
    PN_CHECK_FALSE (pn_memory_parse_meminfo ("", &total, &avail, &freeb));
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-memory");
    pn_test_add ("parses_to_bytes",        test_parses_and_converts_to_bytes);
    pn_test_add ("memfree_optional",       test_memfree_is_optional);
    pn_test_add ("requires_total_avail",   test_requires_total_and_available);
    return pn_test_run ();
}
