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

/* Unit tests for PnClockDivider: a tick input and a reset input, N
 * outputs, output k forwarding every tick whose count is a multiple of
 * its divisor, slowest output first.  Emission is synchronous in
 * receive(), so no main loop is needed.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-clock-divider.h"

#include <string.h>

/* One recorded emission: the output it left by and its data.value. */
typedef struct
{
    gint    output;
    gdouble value;
} Emission;

typedef struct
{
    guint    count;
    Emission seen[64];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
    {
        r->seen[r->count].output = pn_node_current_output ();
        r->seen[r->count].value  = pn_test_num (message, "value");

        /* Must not leak into the outputs that fire after this one. */
        pn_message_set_double (message, "value", -1.0);
    }
    r->count++;
}

static PnNode *
make_node (gint outputs, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_clock_divider_new ());

    g_object_set (node, "outputs", outputs, NULL);
    if (rec != NULL)
    {
        memset (rec, 0, sizeof *rec);
        g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    }
    return node;
}

/* Deliver one message on @input, the way a wire would: through
 * pn_node_receive_message_on_input so pn_node_current_input() is set. */
static void
send_on (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);

    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

static void
tick (PnNode *node, gdouble value)
{
    send_on (node, PN_CLOCK_DIVIDER_IN_TICK, value);
}

/* ------------------------------------------------------------------ */

/* Two named inputs, four outputs labelled /2 /4 /8 /16, and defaults
 * that do not show up in the saved "divisors". */
static void
test_defaults (void)
{
    PnNode *node = PN_NODE (pn_clock_divider_new ());
    gchar  *json;

    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 2);
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "tick");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "reset");
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 4);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "/2");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 3), ==, "/16");
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (PN_CLOCK_DIVIDER (node), 15),
                     ==, 65536u);

    g_object_get (node, "divisors", &json, NULL);
    PN_CHECK_CMPSTR (json, ==, "[]");
    g_free (json);

    g_object_unref (node);
}

/* With /2 /4 /8 /16, eight ticks fire: t2 out1; t4 out2,out1;
 * t6 out1; t8 out3,out2,out1 — slowest first — and carry the tick. */
static void
test_powers_of_two_sequence (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, &rec);
    const Emission want[] = {
        { 0, 2.0 },
        { 1, 4.0 }, { 0, 4.0 },
        { 0, 6.0 },
        { 2, 8.0 }, { 1, 8.0 }, { 0, 8.0 },
    };
    guint    i;

    for (i = 1; i <= 8; i++)
        tick (node, (gdouble) i);

    PN_CHECK_CMPINT (rec.count, ==, G_N_ELEMENTS (want));
    for (i = 0; i < G_N_ELEMENTS (want) && i < rec.count; i++)
    {
        PN_CHECK_CMPINT (rec.seen[i].output, ==, want[i].output);
        PN_CHECK_NEAR   (rec.seen[i].value, want[i].value, 1e-9);
    }
    PN_CHECK_CMPINT (pn_clock_divider_get_count (PN_CLOCK_DIVIDER (node)),
                     ==, 8u);

    g_object_unref (node);
}

/* The plan orders by divisor, not by output number: a slow divisor on
 * output 1 fires before a fast one on output 3; equal divisors fire from
 * the highest output down; divisor 1 fires every tick. */
static void
test_plan_orders_by_divisor (void)
{
    PnNode         *node = make_node (3, NULL);
    PnClockDivider *cd   = PN_CLOCK_DIVIDER (node);
    gint            order[PN_CLOCK_DIVIDER_MAX_OUTPUTS];
    gint            n;

    pn_clock_divider_set_divisor (cd, 0, 6);
    pn_clock_divider_set_divisor (cd, 1, 3);
    pn_clock_divider_set_divisor (cd, 2, 1);

    n = pn_clock_divider_plan (cd, 6, order);
    PN_CHECK_CMPINT (n, ==, 3);
    PN_CHECK_CMPINT (order[0], ==, 0);
    PN_CHECK_CMPINT (order[1], ==, 1);
    PN_CHECK_CMPINT (order[2], ==, 2);

    n = pn_clock_divider_plan (cd, 5, order);
    PN_CHECK_CMPINT (n, ==, 1);
    PN_CHECK_CMPINT (order[0], ==, 2);

    pn_clock_divider_set_divisor (cd, 0, 3);
    n = pn_clock_divider_plan (cd, 3, order);
    PN_CHECK_CMPINT (n, ==, 3);
    PN_CHECK_CMPINT (order[0], ==, 1);
    PN_CHECK_CMPINT (order[1], ==, 0);
    PN_CHECK_CMPINT (order[2], ==, 2);

    PN_CHECK_CMPINT (pn_clock_divider_plan (cd, 0, order), ==, 0);

    g_object_unref (node);
}

/* Reset zeroes the count silently; the next division starts over. */
static void
test_reset_is_silent (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, &rec);

    tick (node, 1.0);
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    send_on (node, PN_CLOCK_DIVIDER_IN_RESET, 0.0);
    PN_CHECK_CMPINT (rec.count, ==, 0u);
    PN_CHECK_CMPINT (pn_clock_divider_get_count (PN_CLOCK_DIVIDER (node)),
                     ==, 0u);

    tick (node, 2.0);   /* count 1: nothing */
    PN_CHECK_CMPINT (rec.count, ==, 0u);
    tick (node, 3.0);   /* count 2: out1    */
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 0);

    g_object_unref (node);
}

/* Outputs beyond the count never fire, and their divisors survive a
 * shrink/grow round trip. */
static void
test_shrink_keeps_divisors (void)
{
    Recorder        rec;
    PnNode         *node = make_node (4, &rec);
    PnClockDivider *cd   = PN_CLOCK_DIVIDER (node);
    guint           i;

    pn_clock_divider_set_divisor (cd, 3, 5);
    g_object_set (node, "outputs", 2, NULL);

    for (i = 1; i <= 10; i++)
        tick (node, (gdouble) i);
    for (i = 0; i < rec.count; i++)
        PN_CHECK_CMPINT (rec.seen[i].output, <, 2);

    g_object_set (node, "outputs", 4, NULL);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 3), ==, 5u);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 3), ==, "/5");

    g_object_unref (node);
}

/* "divisors" round-trips, trims trailing defaults, clamps out-of-range
 * values, and falls back to defaults for junk. */
static void
test_divisors_json (void)
{
    PnNode         *node = make_node (4, NULL);
    PnClockDivider *cd   = PN_CLOCK_DIVIDER (node);
    gchar          *json;

    g_object_set (node, "divisors", "[3, 4, 10]", NULL);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 0), ==, 3u);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 2), ==, 10u);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 3), ==, 16u);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 2), ==, "/10");

    g_object_get (node, "divisors", &json, NULL);
    PN_CHECK_CMPSTR (json, ==, "[3,4,10]");
    g_free (json);

    g_object_set (node, "divisors", "[0, \"x\", 5000000]", NULL);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 0), ==, 1u);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 1), ==, 4u);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 2), ==,
                     PN_CLOCK_DIVIDER_MAX_DIVISOR);

    g_object_set (node, "divisors", "not json", NULL);
    PN_CHECK_CMPINT (pn_clock_divider_get_divisor (cd, 0), ==, 2u);
    g_object_get (node, "divisors", &json, NULL);
    PN_CHECK_CMPSTR (json, ==, "[]");
    g_free (json);

    g_object_unref (node);
}

static void
count_notify (GObject *object, GParamSpec *pspec, gpointer counter)
{
    (void) object;
    (void) pspec;
    (*(guint *) counter)++;
}

/* Setting a divisor notifies "divisors" only when it changes. */
static void
test_set_divisor_notifies (void)
{
    PnNode *node = make_node (2, NULL);
    guint   notified = 0;

    g_signal_connect (node, "notify::divisors",
                      G_CALLBACK (count_notify), &notified);

    pn_clock_divider_set_divisor (PN_CLOCK_DIVIDER (node), 0, 7);
    pn_clock_divider_set_divisor (PN_CLOCK_DIVIDER (node), 0, 7);
    PN_CHECK_CMPINT (notified, ==, 1u);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-clock-divider");
    pn_test_add ("defaults",              test_defaults);
    pn_test_add ("powers_of_two",         test_powers_of_two_sequence);
    pn_test_add ("plan_orders_by_divisor", test_plan_orders_by_divisor);
    pn_test_add ("reset_is_silent",       test_reset_is_silent);
    pn_test_add ("shrink_keeps_divisors", test_shrink_keeps_divisors);
    pn_test_add ("divisors_json",         test_divisors_json);
    pn_test_add ("set_divisor_notifies",  test_set_divisor_notifies);
    return pn_test_run ();
}
