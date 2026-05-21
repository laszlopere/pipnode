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

/* Unit tests for PnStats: on every arrival, emit a fresh message
 * summarising how many messages landed in the rolling window and the
 * derived per-minute / per-hour / per-day rates.  All sends in a test
 * run land in the same default 60-second window, so the count is
 * deterministic; the gap timings (which depend on wall-clock spacing)
 * are only asserted where they are structurally zero. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-stats.h"

/* Captures the most recently emitted message so a test can assert on
 * the statistics payload, which is a brand-new message rather than the
 * one that was fed in. */
typedef struct
{
    guint      count;
    PnMessage *last;
} Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;

    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

static PnNode *
make_node (Capture *cap)
{
    PnNode *node = g_object_new (PN_TYPE_STATS, NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

static void
send_tick (PnNode *node)
{
    PnMessage *msg = pn_message_new (NULL, NULL);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_first_message_has_no_gap (void)
{
    Capture cap;
    PnNode *node = make_node (&cap);

    /* A single arrival: count is one and there is no inter-arrival gap
     * yet, so every gap statistic is zero. */
    send_tick (node);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "count"),           1.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "average_seconds"), 0.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "min_seconds"),     0.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "max_seconds"),     0.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_count_and_rates (void)
{
    Capture cap;
    PnNode *node = make_node (&cap);

    /* Three arrivals within the default 60-second window.  The rate is
     * reported relative to the window length: 3 messages / 60 s gives
     * 3 per minute and 180 per hour. */
    send_tick (node);
    send_tick (node);
    send_tick (node);

    PN_CHECK_CMPINT (cap.count, ==, 3);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "count"),      3.0,   1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "per_minute"), 3.0,   1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "per_hour"),   180.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "per_day"),    4320.0, 1e-6);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_window_scales_rate (void)
{
    Capture cap;
    PnNode *node = make_node (&cap);

    /* With a 10-second window a single message implies a rate of
     * 1/10 per second == 6 per minute. */
    g_object_set (node, "window", 10u, NULL);
    send_tick (node);

    PN_CHECK_NEAR (pn_test_num (cap.last, "count"),      1.0, 1e-9);
    PN_CHECK_NEAR (pn_test_num (cap.last, "per_minute"), 6.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-stats");
    pn_test_add ("first_has_no_gap",       test_first_message_has_no_gap);
    pn_test_add ("count_and_rates",        test_count_and_rates);
    pn_test_add ("window_scales_rate",     test_window_scales_rate);
    return pn_test_run ();
}
