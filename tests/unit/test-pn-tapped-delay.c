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

/* Unit tests for PnTappedDelay: one input, N outputs, every message
 * forwarded on output k after k * step-ms.  Each copy rides its own
 * main-loop timer, so the tests pump a #GMainContext with short steps
 * and a safety deadline.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tapped-delay.h"

#include <string.h>

/* One recorded emission: its output, its data.value and when it left,
 * in ms since the recorder was started. */
typedef struct
{
    gint    output;
    gdouble value;
    gint64  at_ms;
} Emission;

typedef struct
{
    guint    count;
    gint64   start;
    Emission seen[32];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
    {
        Emission *e = &r->seen[r->count];

        e->output = pn_node_current_output ();
        e->value  = pn_test_num (message, "value");
        e->at_ms  = (g_get_monotonic_time () - r->start) / 1000;

        /* A handler that edits its message must not reach the copies
         * still waiting for later outputs. */
        pn_message_set_double (message, "value", -1.0);
    }
    r->count++;
}

/* Pump the default main context until @counter reaches @want or a
 * safety deadline passes, so a regression cannot hang the suite. */
static void
pump_until (const guint *counter, guint want, guint timeout_ms)
{
    GMainContext *ctx = g_main_context_default ();
    gint64        end = g_get_monotonic_time () + (gint64) timeout_ms * 1000;

    while (*counter < want && g_get_monotonic_time () < end)
    {
        g_main_context_iteration (ctx, FALSE);
        g_usleep (500);
    }
}

static PnNode *
make_node (gint outputs, guint step_ms, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_tapped_delay_new ());

    g_object_set (node, "outputs", outputs, "step-ms", step_ms, NULL);
    memset (rec, 0, sizeof *rec);
    rec->start = g_get_monotonic_time ();
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

static void
send (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);

    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

static guint
pending (PnNode *node)
{
    return pn_tapped_delay_get_n_pending (PN_TAPPED_DELAY (node));
}

/* ------------------------------------------------------------------ */

/* A fresh node: one input, the default four outputs, labelled with
 * their delays. */
static void
test_defaults (void)
{
    PnNode *node = PN_NODE (pn_tapped_delay_new ());
    gint    outputs;
    guint   step;

    g_object_get (node, "outputs", &outputs, "step-ms", &step, NULL);
    PN_CHECK_CMPINT (outputs, ==, PN_TAPPED_DELAY_DEF_OUTPUTS);
    PN_CHECK_CMPINT (step, ==, PN_TAPPED_DELAY_STEP_MS_DEF);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 4);
    PN_CHECK (pn_node_get_has_input (node));
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "250 ms");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 3), ==, "1 s");

    g_object_unref (node);
}

static void
test_format_delay (void)
{
    gchar *s;

    s = pn_tapped_delay_format_delay (0);
    PN_CHECK_CMPSTR (s, ==, "0 ms");
    g_free (s);
    s = pn_tapped_delay_format_delay (999);
    PN_CHECK_CMPSTR (s, ==, "999 ms");
    g_free (s);
    s = pn_tapped_delay_format_delay (1500);
    PN_CHECK_CMPSTR (s, ==, "1.5 s");
    g_free (s);
    s = pn_tapped_delay_format_delay (60000);
    PN_CHECK_CMPSTR (s, ==, "60 s");
    g_free (s);
}

/* Output names follow both properties. */
static void
test_names_follow_properties (void)
{
    PnNode *node = PN_NODE (pn_tapped_delay_new ());

    g_object_set (node, "step-ms", 100u, NULL);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 1), ==, "200 ms");

    g_object_set (node, "outputs", 6, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 6);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 5), ==, "600 ms");

    g_object_unref (node);
}

/* One message leaves once on every output, output 1 first, nothing
 * synchronously, each copy no earlier than its k * step deadline and
 * unchanged by the handler edits on earlier copies. */
static void
test_staggered_fan_out (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, 30, &rec);
    gint     k;

    send (node, 7.0);
    PN_CHECK_CMPINT (rec.count, ==, 0u);
    PN_CHECK_CMPINT (pending (node), ==, 3u);

    pump_until (&rec.count, 3, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (pending (node), ==, 0u);

    for (k = 0; k < 3; k++)
    {
        PN_CHECK_CMPINT (rec.seen[k].output, ==, k);
        PN_CHECK_NEAR   (rec.seen[k].value, 7.0, 1e-9);
        PN_CHECK_CMPINT (rec.seen[k].at_ms, >=, 30 * (k + 1) - 2);
    }

    g_object_unref (node);
}

/* A burst keeps its order on each output, and is neither dropped nor
 * coalesced. */
static void
test_burst_in_order (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, 20, &rec);
    gdouble  last[2] = { 0.0, 0.0 };
    guint    per[2]  = { 0, 0 };
    guint    i;

    send (node, 1.0);
    send (node, 2.0);
    send (node, 3.0);
    PN_CHECK_CMPINT (pending (node), ==, 6u);

    pump_until (&rec.count, 6, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 6u);

    for (i = 0; i < rec.count; i++)
    {
        gint o = rec.seen[i].output;

        PN_CHECK (rec.seen[i].value > last[o]);
        last[o] = rec.seen[i].value;
        per[o]++;
    }
    PN_CHECK_CMPINT (per[0], ==, 3u);
    PN_CHECK_CMPINT (per[1], ==, 3u);

    g_object_unref (node);
}

/* With step 0 every output still fires on a later main-loop iteration,
 * output 1 first. */
static void
test_zero_step (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, 0, &rec);
    gint     k;

    send (node, 5.0);
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    pump_until (&rec.count, 4, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 4u);
    for (k = 0; k < 4; k++)
        PN_CHECK_CMPINT (rec.seen[k].output, ==, k);

    g_object_unref (node);
}

/* Lowering "outputs" cancels only the copies for removed outputs. */
static void
test_shrink_cancels_removed_outputs (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, 20, &rec);

    send (node, 1.0);
    PN_CHECK_CMPINT (pending (node), ==, 4u);

    g_object_set (node, "outputs", 2, NULL);
    PN_CHECK_CMPINT (pending (node), ==, 2u);

    pump_until (&rec.count, 3, 300);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 0);
    PN_CHECK_CMPINT (rec.seen[1].output, ==, 1);

    g_object_unref (node);
}

/* Changing step-ms leaves the armed copies on their old schedule and
 * applies to the next message. */
static void
test_step_change_applies_to_next_message (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, 5000, &rec);

    send (node, 1.0);
    g_object_set (node, "step-ms", 10u, NULL);
    PN_CHECK_CMPINT (pending (node), ==, 2u);

    send (node, 2.0);
    pump_until (&rec.count, 2, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_NEAR   (rec.seen[0].value, 2.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1].value, 2.0, 1e-9);

    /* The first message's copies are still waiting on 5 s / 10 s. */
    PN_CHECK_CMPINT (pending (node), ==, 2u);

    g_object_unref (node);
}

/* Destroying the node cancels every waiting copy: no emission, no
 * use-after-free when the loop is pumped later. */
static void
test_dispose_cancels_pending (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, 10, &rec);

    send (node, 1.0);
    g_object_unref (node);

    pump_until (&rec.count, 1, 100);
    PN_CHECK_CMPINT (rec.count, ==, 0u);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tapped-delay");
    pn_test_add ("defaults",               test_defaults);
    pn_test_add ("format_delay",           test_format_delay);
    pn_test_add ("names_follow_props",     test_names_follow_properties);
    pn_test_add ("staggered_fan_out",      test_staggered_fan_out);
    pn_test_add ("burst_in_order",         test_burst_in_order);
    pn_test_add ("zero_step",              test_zero_step);
    pn_test_add ("shrink_cancels_removed", test_shrink_cancels_removed_outputs);
    pn_test_add ("step_change_next_msg",   test_step_change_applies_to_next_message);
    pn_test_add ("dispose_cancels",        test_dispose_cancels_pending);
    return pn_test_run ();
}
