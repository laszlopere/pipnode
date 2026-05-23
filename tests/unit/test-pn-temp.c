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

/* Unit tests for PnTemp's pure parse/aggregate seam.  The node samples
 * real hardware (local hwmon/thermal, or a remote host over SSH), which
 * is not reproducible headless, so the testable contract is the two
 * helpers the trigger feeds its sample through: pn_temp_parse_remote_lines
 * (one "name|label|millidegrees" line each -> per-sensor readings) and
 * pn_temp_aggregate (collapse readings to a value per the AVERAGE /
 * MAXIMUM mode, plus min/max/avg and the hottest sensor's label).
 * Canned blobs stand in for the SSH sample.  These pin the logic half
 * that the headless/core split (TODO #23) keeps loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-temp.h"

/* Free the strdup'd label of each parsed reading. */
static void
clear_reading (gpointer data)
{
    PnTempReading *r = data;
    g_free (r->label);
}

static GArray *
new_readings (void)
{
    GArray *a = g_array_new (FALSE, FALSE, sizeof (PnTempReading));
    g_array_set_clear_func (a, clear_reading);
    return a;
}

static void
test_parse_basic (void)
{
    GArray *r = new_readings ();

    pn_temp_parse_remote_lines (
        "coretemp|Core 0|45000\n"
        "k10temp|Tctl|60000\n", r);

    PN_CHECK_CMPINT (r->len, ==, 2u);
    PN_CHECK_CMPSTR (g_array_index (r, PnTempReading, 0).label,
                     ==, "coretemp/Core 0");
    PN_CHECK_NEAR  (g_array_index (r, PnTempReading, 0).celsius, 45.0, 1e-9);
    PN_CHECK_CMPSTR (g_array_index (r, PnTempReading, 1).label,
                     ==, "k10temp/Tctl");
    PN_CHECK_NEAR  (g_array_index (r, PnTempReading, 1).celsius, 60.0, 1e-9);

    g_array_free (r, TRUE);
}

static void
test_parse_skips_malformed (void)
{
    GArray *r = new_readings ();

    /* A line with no bars and one with a single bar are both dropped;
     * only the well-formed three-field line survives.  A trailing line
     * without a newline is still parsed. */
    pn_temp_parse_remote_lines (
        "garbage_no_bars\n"
        "onebar|only\n"
        "\n"
        "ok|sensor|12500", r);

    PN_CHECK_CMPINT (r->len, ==, 1u);
    PN_CHECK_CMPSTR (g_array_index (r, PnTempReading, 0).label, ==, "ok/sensor");
    PN_CHECK_NEAR  (g_array_index (r, PnTempReading, 0).celsius, 12.5, 1e-9);

    g_array_free (r, TRUE);
}

static void
test_parse_empty_and_null (void)
{
    GArray *r = new_readings ();

    pn_temp_parse_remote_lines ("", r);
    PN_CHECK_CMPINT (r->len, ==, 0u);
    pn_temp_parse_remote_lines (NULL, r);
    PN_CHECK_CMPINT (r->len, ==, 0u);

    g_array_free (r, TRUE);
}

static void
test_aggregate_average_and_extents (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo = -1, hi = -1, avg = -1, value;
    const gchar *hot = NULL;

    pn_temp_parse_remote_lines (
        "a|x|40000\n"   /* 40 C */
        "b|y|60000\n"   /* 60 C */
        "c|z|50000\n",  /* 50 C */
        r);

    value = pn_temp_aggregate (r, PN_TEMP_AVERAGE, &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR  (value, 50.0, 1e-9);   /* average mode returns the mean */
    PN_CHECK_NEAR  (lo,    40.0, 1e-9);
    PN_CHECK_NEAR  (hi,    60.0, 1e-9);
    PN_CHECK_NEAR  (avg,   50.0, 1e-9);
    PN_CHECK_CMPSTR (hot,  ==, "b/y");    /* hottest sensor's label */

    g_array_free (r, TRUE);
}

static void
test_aggregate_maximum (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo, hi, avg, value;
    const gchar *hot = NULL;

    pn_temp_parse_remote_lines ("a|x|40000\nb|y|60000\n", r);
    value = pn_temp_aggregate (r, PN_TEMP_MAXIMUM, &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR  (value, 60.0, 1e-9);   /* maximum mode returns the peak */
    PN_CHECK_NEAR  (avg,   50.0, 1e-9);
    PN_CHECK_CMPSTR (hot,  ==, "b/y");

    g_array_free (r, TRUE);
}

static void
test_aggregate_empty (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo = 9, hi = 9, avg = 9, value;
    const gchar *hot = (const gchar *) "sentinel";

    value = pn_temp_aggregate (r, PN_TEMP_AVERAGE, &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR (value, 0.0, 1e-9);
    PN_CHECK_NEAR (lo,    0.0, 1e-9);
    PN_CHECK_NEAR (hi,    0.0, 1e-9);
    PN_CHECK_NEAR (avg,   0.0, 1e-9);
    PN_CHECK (hot == NULL);

    g_array_free (r, TRUE);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-temp");
    pn_test_add ("parse_basic",                test_parse_basic);
    pn_test_add ("parse_skips_malformed",      test_parse_skips_malformed);
    pn_test_add ("parse_empty_and_null",       test_parse_empty_and_null);
    pn_test_add ("aggregate_average_extents",  test_aggregate_average_and_extents);
    pn_test_add ("aggregate_maximum",          test_aggregate_maximum);
    pn_test_add ("aggregate_empty",            test_aggregate_empty);
    return pn_test_run ();
}
