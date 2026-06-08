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

/* Unit tests for PnAmbient's pure parse/aggregate seam.  The node samples
 * real hardware (local /sys/class/hwmon + /sys/class/thermal, or a remote
 * host over SSH), which is not reproducible headless, so the testable
 * contract is the two helpers the trigger feeds its sample through:
 * pn_ambient_parse_remote_lines (one "name|label|millidegrees" line each
 * -> per-sensor readings) and pn_ambient_aggregate (collapse readings to a
 * value per the AVERAGE / MAXIMUM mode, plus min/max/avg and the hottest
 * sensor's label).  Canned blobs stand in for the SSH sample.  A final
 * case pins the aggregation enum's stable "average"/"maximum" nicks, which
 * a downstream filter switching on data.aggregation relies on. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ambient.h"

/* Free the strdup'd label of each parsed reading. */
static void
clear_reading (gpointer data)
{
    PnAmbientReading *r = data;
    g_free (r->label);
}

static GArray *
new_readings (void)
{
    GArray *a = g_array_new (FALSE, FALSE, sizeof (PnAmbientReading));
    g_array_set_clear_func (a, clear_reading);
    return a;
}

static void
test_parse_basic (void)
{
    GArray *r = new_readings ();

    pn_ambient_parse_remote_lines (
        "acpitz|thermal_zone0|32000\n"
        "pch_skylake|thermal_zone1|41000\n", r);

    PN_CHECK_CMPINT (r->len, ==, 2u);
    PN_CHECK_CMPSTR (g_array_index (r, PnAmbientReading, 0).label,
                     ==, "acpitz/thermal_zone0");
    PN_CHECK_NEAR  (g_array_index (r, PnAmbientReading, 0).celsius, 32.0, 1e-9);
    PN_CHECK_CMPSTR (g_array_index (r, PnAmbientReading, 1).label,
                     ==, "pch_skylake/thermal_zone1");
    PN_CHECK_NEAR  (g_array_index (r, PnAmbientReading, 1).celsius, 41.0, 1e-9);

    g_array_free (r, TRUE);
}

static void
test_parse_skips_malformed (void)
{
    GArray *r = new_readings ();

    /* A line with no bars and one with a single bar are both dropped;
     * only the well-formed three-field line survives.  A trailing line
     * without a newline is still parsed. */
    pn_ambient_parse_remote_lines (
        "garbage_no_bars\n"
        "onebar|only\n"
        "\n"
        "acpitz|zone|27500", r);

    PN_CHECK_CMPINT (r->len, ==, 1u);
    PN_CHECK_CMPSTR (g_array_index (r, PnAmbientReading, 0).label,
                     ==, "acpitz/zone");
    PN_CHECK_NEAR  (g_array_index (r, PnAmbientReading, 0).celsius, 27.5, 1e-9);

    g_array_free (r, TRUE);
}

static void
test_parse_empty_and_null (void)
{
    GArray *r = new_readings ();

    pn_ambient_parse_remote_lines ("", r);
    PN_CHECK_CMPINT (r->len, ==, 0u);
    pn_ambient_parse_remote_lines (NULL, r);
    PN_CHECK_CMPINT (r->len, ==, 0u);

    g_array_free (r, TRUE);
}

static void
test_aggregate_average (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo = -1, hi = -1, avg = -1, value;
    const gchar *hot = NULL;

    pn_ambient_parse_remote_lines (
        "a|x|20000\n"   /* 20 C */
        "b|y|30000\n"   /* 30 C */
        "c|z|25000\n",  /* 25 C */
        r);

    value = pn_ambient_aggregate (r, PN_AMBIENT_AVERAGE,
                                  &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR  (value, 25.0, 1e-9);   /* average mode returns the mean */
    PN_CHECK_NEAR  (lo,    20.0, 1e-9);
    PN_CHECK_NEAR  (hi,    30.0, 1e-9);
    PN_CHECK_NEAR  (avg,   25.0, 1e-9);
    PN_CHECK_CMPSTR (hot,  ==, "b/y");    /* hottest sensor's label */

    g_array_free (r, TRUE);
}

static void
test_aggregate_maximum (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo, hi, avg, value;
    const gchar *hot = NULL;

    pn_ambient_parse_remote_lines ("a|x|20000\nb|y|30000\n", r);
    value = pn_ambient_aggregate (r, PN_AMBIENT_MAXIMUM,
                                  &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR  (value, 30.0, 1e-9);   /* maximum mode returns the peak */
    PN_CHECK_NEAR  (avg,   25.0, 1e-9);
    PN_CHECK_CMPSTR (hot,  ==, "b/y");

    g_array_free (r, TRUE);
}

static void
test_aggregate_empty (void)
{
    GArray      *r   = new_readings ();
    gdouble      lo = 9, hi = 9, avg = 9, value;
    const gchar *hot = (const gchar *) "sentinel";

    value = pn_ambient_aggregate (r, PN_AMBIENT_AVERAGE,
                                  &lo, &hi, &avg, &hot);

    PN_CHECK_NEAR (value, 0.0, 1e-9);
    PN_CHECK_NEAR (lo,    0.0, 1e-9);
    PN_CHECK_NEAR (hi,    0.0, 1e-9);
    PN_CHECK_NEAR (avg,   0.0, 1e-9);
    PN_CHECK (hot == NULL);

    g_array_free (r, TRUE);
}

static void
test_aggregation_nicks (void)
{
    GEnumClass *klass = g_type_class_ref (PN_TYPE_AMBIENT_AGGREGATION);
    GEnumValue *avg   = g_enum_get_value (klass, PN_AMBIENT_AVERAGE);
    GEnumValue *max   = g_enum_get_value (klass, PN_AMBIENT_MAXIMUM);

    /* The nicks are part of the contract -- they appear verbatim on
     * data.aggregation and must stay identical to #PnTemp's so a filter
     * can switch on either node's output. */
    PN_CHECK (avg != NULL && max != NULL);
    PN_CHECK_CMPSTR (avg->value_nick, ==, "average");
    PN_CHECK_CMPSTR (max->value_nick, ==, "maximum");

    g_type_class_unref (klass);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ambient");
    pn_test_add ("parse_basic",            test_parse_basic);
    pn_test_add ("parse_skips_malformed",  test_parse_skips_malformed);
    pn_test_add ("parse_empty_and_null",   test_parse_empty_and_null);
    pn_test_add ("aggregate_average",      test_aggregate_average);
    pn_test_add ("aggregate_maximum",      test_aggregate_maximum);
    pn_test_add ("aggregate_empty",        test_aggregate_empty);
    pn_test_add ("aggregation_nicks",      test_aggregation_nicks);
    return pn_test_run ();
}
