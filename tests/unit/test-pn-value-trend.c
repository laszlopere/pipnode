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

/* Unit tests for PnValueTrend: one input, a "rising" and a "falling"
 * output, an optional third "unchanged" one, and a deadband measured
 * from the last forwarded value.  Emission is synchronous in receive(),
 * so no main loop is needed.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-value-trend.h"

#include <string.h>

typedef struct
{
    guint   count;
    gint    output[32];
    gdouble value[32];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->output))
    {
        r->output[r->count] = pn_node_current_output ();
        r->value[r->count]  = pn_test_num (message, "value");
    }
    r->count++;
}

static PnNode *
make_node (Recorder *rec)
{
    PnNode *node = PN_NODE (pn_value_trend_new ());

    memset (rec, 0, sizeof *rec);
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Deliver a message whose data.value is @value, and return the output
 * it left by, or -1 when it was dropped. */
static gint
feed (PnNode *node, Recorder *rec, gdouble value)
{
    PnMessage *m      = pn_message_new (NULL, NULL);
    guint      before = rec->count;

    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK (rec->count <= before + 1);
    return rec->count > before ? rec->output[rec->count - 1] : -1;
}

/* Deliver a message with no numeric value at all. */
static gint
feed_text (PnNode *node, Recorder *rec, const gchar *text)
{
    PnMessage *m      = pn_message_new (NULL, NULL);
    guint      before = rec->count;

    pn_message_set_string (m, "value", text);
    pn_node_receive_message (node, m);
    g_object_unref (m);

    return rec->count > before ? rec->output[rec->count - 1] : -1;
}

/* ------------------------------------------------------------------ */

static void
test_defaults (void)
{
    PnNode   *node = PN_NODE (pn_value_trend_new ());
    gdouble   band = -1.0;
    gboolean  third = TRUE;

    g_object_get (node, "min-change", &band,
                        "unchanged-output", &third, NULL);
    PN_CHECK_NEAR (band, 0.0, 1e-12);
    PN_CHECK_FALSE (third);

    PN_CHECK_CMPSTR (pn_node_get_class_name (node), ==, "Value Trend");
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 2);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "rising");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 1), ==, "falling");

    g_object_unref (node);
}

/* The pure seam, with no node and no state. */
static void
test_classify (void)
{
    PN_CHECK_CMPINT (pn_value_trend_classify (1.0, 2.0, 0.0), ==,
                     PN_VALUE_TREND_RISING);
    PN_CHECK_CMPINT (pn_value_trend_classify (2.0, 1.0, 0.0), ==,
                     PN_VALUE_TREND_FALLING);
    PN_CHECK_CMPINT (pn_value_trend_classify (2.0, 2.0, 0.0), ==,
                     PN_VALUE_TREND_UNCHANGED);

    /* A move of exactly the band width trips; anything under it does not. */
    PN_CHECK_CMPINT (pn_value_trend_classify (10.0, 15.0, 5.0), ==,
                     PN_VALUE_TREND_RISING);
    PN_CHECK_CMPINT (pn_value_trend_classify (10.0, 14.9, 5.0), ==,
                     PN_VALUE_TREND_UNCHANGED);
    PN_CHECK_CMPINT (pn_value_trend_classify (10.0,  5.0, 5.0), ==,
                     PN_VALUE_TREND_FALLING);
    PN_CHECK_CMPINT (pn_value_trend_classify (10.0,  5.1, 5.0), ==,
                     PN_VALUE_TREND_UNCHANGED);

    /* A negative band is read as none. */
    PN_CHECK_CMPINT (pn_value_trend_classify (1.0, 1.5, -3.0), ==,
                     PN_VALUE_TREND_RISING);
    PN_CHECK_CMPINT (pn_value_trend_classify (1.0, 1.0, -3.0), ==,
                     PN_VALUE_TREND_UNCHANGED);
}

/* First message seeds, then a plain rise / fall split. */
static void
test_rise_fall_first (void)
{
    Recorder  rec;
    PnNode   *node = make_node (&rec);

    PN_CHECK_CMPINT (feed (node, &rec, 10.0), ==, -1);   /* seeds only */
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    PN_CHECK_CMPINT (feed (node, &rec, 11.0), ==, PN_VALUE_TREND_OUT_RISING);
    PN_CHECK_CMPINT (feed (node, &rec, 11.5), ==, PN_VALUE_TREND_OUT_RISING);
    PN_CHECK_CMPINT (feed (node, &rec,  3.0), ==, PN_VALUE_TREND_OUT_FALLING);
    PN_CHECK_CMPINT (feed (node, &rec, -4.0), ==, PN_VALUE_TREND_OUT_FALLING);
    PN_CHECK_CMPINT (feed (node, &rec,  0.0), ==, PN_VALUE_TREND_OUT_RISING);

    /* Equal to the reference: dropped, no third output configured. */
    PN_CHECK_CMPINT (feed (node, &rec, 0.0), ==, -1);
    PN_CHECK_CMPINT (rec.count, ==, 5u);

    g_object_unref (node);
}

/* The message is forwarded untouched, on the right output. */
static void
test_forwards_unchanged (void)
{
    Recorder   rec;
    PnNode    *node = make_node (&rec);
    PnMessage *m;

    feed (node, &rec, 1.0);

    m = pn_message_new (NULL, "sensor/temp");
    pn_message_set_double (m, "value", 7.5);
    pn_message_set_string (m, "unit", "C");
    pn_node_receive_message (node, m);
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.output[0], ==, PN_VALUE_TREND_OUT_RISING);
    PN_CHECK_NEAR (pn_test_num (m, "value"), 7.5, 1e-12);
    PN_CHECK_CMPSTR (pn_test_str (m, "unit"), ==, "C");
    PN_CHECK_CMPSTR (pn_message_get_topic (m), ==, "sensor/temp");
    g_object_unref (m);

    g_object_unref (node);
}

/* Sub-band steps are swallowed, but they accumulate: the reference is
 * the last value FORWARDED, so a slow drift eventually trips. */
static void
test_deadband_drift (void)
{
    Recorder  rec;
    PnNode   *node = make_node (&rec);

    g_object_set (node, "min-change", 5.0, NULL);

    PN_CHECK_CMPINT (feed (node, &rec, 100.0), ==, -1);  /* seeds */

    PN_CHECK_CMPINT (feed (node, &rec, 102.0), ==, -1);  /* +2  under band */
    PN_CHECK_CMPINT (feed (node, &rec, 104.0), ==, -1);  /* +4  under band */
    /* +6 from the reference of 100, even though each step was small. */
    PN_CHECK_CMPINT (feed (node, &rec, 106.0), ==, PN_VALUE_TREND_OUT_RISING);
    PN_CHECK_NEAR (rec.value[0], 106.0, 1e-12);

    /* Reference is now 106: 103 is -3, still inside the band. */
    PN_CHECK_CMPINT (feed (node, &rec, 103.0), ==, -1);
    PN_CHECK_CMPINT (feed (node, &rec, 101.0), ==, PN_VALUE_TREND_OUT_FALLING);
    PN_CHECK_NEAR (rec.value[1], 101.0, 1e-12);

    /* Exactly the band width trips. */
    PN_CHECK_CMPINT (feed (node, &rec, 106.0), ==, PN_VALUE_TREND_OUT_RISING);

    PN_CHECK_CMPINT (rec.count, ==, 3u);
    g_object_unref (node);
}

/* The optional third output collects what would have been dropped —
 * except the first message, which has nothing to compare against. */
static void
test_unchanged_output (void)
{
    Recorder  rec;
    PnNode   *node = make_node (&rec);

    g_object_set (node, "unchanged-output", TRUE, "min-change", 2.0, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 3);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 2), ==, "unchanged");

    PN_CHECK_CMPINT (feed (node, &rec, 50.0), ==, -1);   /* still dropped */
    PN_CHECK_CMPINT (feed (node, &rec, 50.0), ==,
                     PN_VALUE_TREND_OUT_UNCHANGED);
    PN_CHECK_CMPINT (feed (node, &rec, 51.0), ==,
                     PN_VALUE_TREND_OUT_UNCHANGED);
    PN_CHECK_CMPINT (feed (node, &rec, 52.0), ==, PN_VALUE_TREND_OUT_RISING);

    /* Turning it back off drops those messages again. */
    g_object_set (node, "unchanged-output", FALSE, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 2);
    PN_CHECK_CMPINT (feed (node, &rec, 52.0), ==, -1);

    g_object_unref (node);
}

/* Non-numeric and missing values are dropped and leave the reference
 * alone; booleans read as 1 and 0. */
static void
test_non_numeric (void)
{
    Recorder   rec;
    PnNode    *node = make_node (&rec);
    PnMessage *m;

    PN_CHECK_CMPINT (feed_text (node, &rec, "warm"), ==, -1);
    PN_CHECK_CMPINT (feed (node, &rec, 10.0), ==, -1);   /* seeds */

    /* Neither of these may become the new reference. */
    PN_CHECK_CMPINT (feed_text (node, &rec, "99"), ==, -1);
    m = pn_message_new (NULL, NULL);
    pn_node_receive_message (node, m);                   /* no value at all */
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    PN_CHECK_CMPINT (feed (node, &rec, 11.0), ==, PN_VALUE_TREND_OUT_RISING);

    /* Booleans: 1.0 against a reference of 11 is a fall. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_boolean (m, "value", TRUE);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.output[1], ==, PN_VALUE_TREND_OUT_FALLING);

    g_object_unref (node);
}

static void
count_notify (GObject *object, GParamSpec *pspec, gpointer counter)
{
    (void) object;
    (void) pspec;
    (*(guint *) counter)++;
}

/* Properties survive a JSON round trip and notify only on a change. */
static void
test_properties (void)
{
    PnNode *node     = PN_NODE (pn_value_trend_new ());
    guint   notified = 0;

    g_signal_connect (node, "notify::min-change",
                      G_CALLBACK (count_notify), &notified);
    g_signal_connect (node, "notify::unchanged-output",
                      G_CALLBACK (count_notify), &notified);

    g_object_set (node, "min-change", 2.5, NULL);
    g_object_set (node, "min-change", 2.5, NULL);
    PN_CHECK_CMPINT (notified, ==, 1u);

    g_object_set (node, "unchanged-output", TRUE, NULL);
    g_object_set (node, "unchanged-output", TRUE, NULL);
    PN_CHECK_CMPINT (notified, ==, 2u);

    /* The two properties are all the state the document carries, so a
     * fresh node set from them comes up identical, third output and all. */
    {
        PnNode   *copy  = PN_NODE (pn_value_trend_new ());
        gdouble   band  = 0.0;
        gboolean  third = FALSE;

        g_object_get (node, "min-change", &band,
                            "unchanged-output", &third, NULL);
        g_object_set (copy, "min-change", band,
                            "unchanged-output", third, NULL);

        PN_CHECK_NEAR (band, 2.5, 1e-12);
        PN_CHECK (third);
        PN_CHECK_CMPINT (pn_node_get_n_outputs (copy), ==, 3);
        PN_CHECK_CMPSTR (pn_node_get_output_name (copy, 2), ==, "unchanged");

        g_object_unref (copy);
    }

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-value-trend");
    pn_test_add ("defaults",            test_defaults);
    pn_test_add ("classify",            test_classify);
    pn_test_add ("rise_fall_first",     test_rise_fall_first);
    pn_test_add ("forwards_unchanged",  test_forwards_unchanged);
    pn_test_add ("deadband_drift",      test_deadband_drift);
    pn_test_add ("unchanged_output",    test_unchanged_output);
    pn_test_add ("non_numeric",         test_non_numeric);
    pn_test_add ("properties",          test_properties);
    return pn_test_run ();
}
