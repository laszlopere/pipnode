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

/* Unit tests for PnLoad's /proc/loadavg parser.  The node samples the
 * file on a worker thread, which would mean real filesystem I/O; the
 * parser is the pure, deterministic core, so the test feeds it canned
 * kernel text in place of the live file (the "stub" for /proc).  It is
 * exposed non-static from pn-load.c purely for this test. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"

/* Defined (non-static) in lib/pn-load.c — see the note there. */
gboolean pn_load_parse_loadavg (const gchar *text,
                                gdouble *out_l1,
                                gdouble *out_l5,
                                gdouble *out_l15);

static void
test_parses_three_averages (void)
{
    gdouble  l1 = -1.0, l5 = -1.0, l15 = -1.0;
    gboolean ok;

    /* Real kernel shape: "L1 L5 L15 RUNNING/TOTAL LASTPID". */
    ok = pn_load_parse_loadavg ("0.50 1.25 2.00 3/512 99999\n",
                                &l1, &l5, &l15);

    PN_CHECK      (ok);
    PN_CHECK_NEAR (l1,  0.50, 1e-9);
    PN_CHECK_NEAR (l5,  1.25, 1e-9);
    PN_CHECK_NEAR (l15, 2.00, 1e-9);
}

static void
test_parses_locale_independent (void)
{
    gdouble  l1 = 0.0, l5 = 0.0, l15 = 0.0;

    /* The parser uses g_ascii_strtod, so a '.' decimal must parse no
     * matter what LC_NUMERIC the host runs under. */
    PN_CHECK      (pn_load_parse_loadavg ("12.34 0.00 0.01 1/2 3",
                                          &l1, &l5, &l15));
    PN_CHECK_NEAR (l1, 12.34, 1e-9);
}

static void
test_rejects_null_and_garbage (void)
{
    gdouble l1, l5, l15;

    PN_CHECK_FALSE (pn_load_parse_loadavg (NULL, &l1, &l5, &l15));
    PN_CHECK_FALSE (pn_load_parse_loadavg ("", &l1, &l5, &l15));
    PN_CHECK_FALSE (pn_load_parse_loadavg ("not a number", &l1, &l5, &l15));
}

static void
test_rejects_truncated (void)
{
    gdouble l1, l5, l15;

    /* Only two of the three averages present -> the third strtod makes
     * no progress and the parse fails rather than reporting garbage. */
    PN_CHECK_FALSE (pn_load_parse_loadavg ("0.10 0.20", &l1, &l5, &l15));
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-load");
    pn_test_add ("parses_averages",        test_parses_three_averages);
    pn_test_add ("locale_independent",     test_parses_locale_independent);
    pn_test_add ("rejects_garbage",        test_rejects_null_and_garbage);
    pn_test_add ("rejects_truncated",      test_rejects_truncated);
    return pn_test_run ();
}
