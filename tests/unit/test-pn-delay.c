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

/* Unit tests for PnDelay: forward every message unchanged, but only
 * after the configured number of milliseconds.  Each message rides its
 * own one-shot main-loop timer, so a test must drive a #GMainContext to
 * observe the deferred emission rather than see it land synchronously.
 * Headless: no nodes beyond the one under test, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-delay.h"

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
        g_usleep (500);   /* 0.5 ms, so the loop does not spin hot */
    }
}

static PnNode *
make_node (guint delay_ms, guint *out_emits)
{
    PnNode *node = PN_NODE (pn_delay_new ());

    g_object_set (node, "delay-ms", delay_ms, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* A message is not forwarded synchronously; it appears downstream only
 * after the main loop has been pumped past the delay, and arrives
 * unchanged. */
static void
test_holds_then_emits (void)
{
    guint      emits;
    PnNode    *node = make_node (20, &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 7.0);
    pn_node_receive_message (node, msg);

    /* Still pending: receive() returned without emitting. */
    PN_CHECK_CMPINT (emits, ==, 0);

    pump_until (&emits, 1, 2000);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 7.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* Records the data.value of each message as it is forwarded, so the
 * test can assert both the count and the order. */
typedef struct
{
    guint  count;
    gdouble seen[8];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
        r->seen[r->count] = pn_test_num (message, "value");
    r->count++;
}

/* A burst is neither dropped nor coalesced: every message is forwarded,
 * in arrival order, after the delay (unlike Throttle, which would drop
 * the closely-spaced ones). */
static void
test_burst_all_forwarded_in_order (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = PN_NODE (pn_delay_new ());
    gulong    h;

    g_object_set (node, "delay-ms", 20, NULL);
    h = g_signal_connect (node, "message",
                          G_CALLBACK (recorder_cb), &rec);

    for (int i = 0; i < 3; i++)
    {
        PnMessage *m = pn_message_new (NULL, NULL);
        pn_message_set_double (m, "value", (gdouble) (i + 1));
        pn_node_receive_message (node, m);
        g_object_unref (m);
    }

    PN_CHECK_CMPINT (rec.count, ==, 0);   /* all three still pending */

    pump_until (&rec.count, 3, 2000);

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 2.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 3.0, 1e-9);

    g_signal_handler_disconnect (node, h);
    g_object_unref (node);
}

/* Destroying the node while a message is still on its timer must cancel
 * the timer and release the held message — no emission, no use-after-
 * free when the loop is later pumped. */
static void
test_dispose_cancels_pending (void)
{
    guint      emits;
    PnNode    *node = make_node (5000, &emits);   /* long delay */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (emits, ==, 0);

    /* Drop the node before the timer fires. */
    g_object_unref (node);

    /* Pump well past where a 5 s timer would *not* fire anyway, but
     * long enough to surface a stray callback or a freed-memory hit;
     * the counter must stay at zero. */
    pump_until (&emits, 1, 100);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-delay");
    pn_test_add ("holds_then_emits",   test_holds_then_emits);
    pn_test_add ("burst_in_order",     test_burst_all_forwarded_in_order);
    pn_test_add ("dispose_cancels",    test_dispose_cancels_pending);
    return pn_test_run ();
}
