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

#include "pntest.h"

#define PN_TEST_MAX 128

typedef struct
{
    const gchar *name;
    PnTestFunc   func;
} PnTestEntry;

static PnTestEntry  entries[PN_TEST_MAX];
static guint        n_entries;
static const gchar *suite_name = "test";
static gboolean     verbose;

/* Whole-binary tallies (checks, not test cases). */
static guint total_checks;
static guint total_failed;

/* Per-test-case tallies, reset before each case runs. */
static guint cur_checks;
static guint cur_failed;

void
pn_test_init (int *argc, char ***argv, const gchar *suite)
{
    /* We parse --verbose ourselves rather than going through
     * g_test_init(): GLib's GTest reporter wraps everything in the TAP
     * protocol (a "TAP version 13" banner, a random-seed line, and a
     * "# " prefix on every g_print), which cannot be reduced to the
     * single summary line this harness is meant to print by default. */
    if (argc != NULL && argv != NULL)
    {
        int i;
        for (i = 1; i < *argc; i++)
            if (g_strcmp0 ((*argv)[i], "--verbose") == 0 ||
                g_strcmp0 ((*argv)[i], "-v") == 0)
                verbose = TRUE;
    }

    if (suite != NULL)
        suite_name = suite;
}

void
pn_test_add (const gchar *name, PnTestFunc func)
{
    g_assert (n_entries < PN_TEST_MAX);
    entries[n_entries].name = name;
    entries[n_entries].func = func;
    n_entries++;
}

void
pn_test_check_ (gboolean ok, const gchar *expr, const gchar *file, int line)
{
    total_checks++;
    cur_checks++;

    if (!ok)
    {
        cur_failed++;
        total_failed++;
        if (verbose)
            g_print ("      FAIL  %s  (%s:%d)\n", expr, file, line);
    }
    else if (verbose)
    {
        g_print ("      PASS  %s\n", expr);
    }
}

int
pn_test_run (void)
{
    guint     i;
    const int namew = PN_TEST_NAME_WIDTH;

    /* The test unit, then the list of its test cases. */
    g_print ("%s\n", suite_name);

    for (i = 0; i < n_entries; i++)
    {
        cur_checks = 0;
        cur_failed = 0;

        /* In --verbose mode the case name heads its check list; the
         * count line below repeats it so the tally stays attached. */
        if (verbose)
            g_print ("  %s\n", entries[i].name);

        entries[i].func ();

        g_print ("  %-*s  %u checks, %u failed\n",
                 namew, entries[i].name, cur_checks, cur_failed);
    }

    g_print ("  total: %u checks, %u failed\n", total_checks, total_failed);

    return total_failed > 0 ? 1 : 0;
}
