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

/* Unit tests for PnWatchdog: watch the input stream and, on each timer
 * tick, raise an alarm only if no message arrived during the preceding
 * interval.  The node starts pre-armed so the first tick after
 * construction stays silent; thereafter a quiet interval alarms and a
 * busy one stays silent.  Ticks are forced via pn_auto_trigger_kick()
 * (see pntimer.h); between two ticks pn_timer_pump() lets the first one
 * run to completion. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pntimer.h"
#include "pn-watchdog.h"

/* Settle time between two forced ticks: long enough for the worker
 * thread to wake, run the first trigger, and return to waiting before
 * the second kick arrives. */
#define WATCHDOG_SETTLE_MS  200u

static void
feed_message (PnNode *node)
{
    PnMessage *msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_alarm_when_idle (void)
{
    PnNode       *node = g_object_new (PN_TYPE_WATCHDOG, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    g_object_set (node, "output-text", "no traffic!", NULL);

    /* The construction-time prearm makes the first tick silent. */
    pn_auto_trigger_kick (PN_AUTO_TRIGGER (node));
    pn_timer_pump (WATCHDOG_SETTLE_MS);
    PN_CHECK_CMPINT (d.count, ==, 0);

    /* Second tick, with no traffic since the first, raises the alarm. */
    pn_timer_kick_wait (node, &d);
    PN_CHECK_CMPINT (d.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (d.last, "success"));
    PN_CHECK_CMPSTR (pn_test_str  (d.last, "output"), ==, "no traffic!");

    pn_timer_destroy (node, &d);
}

static void
test_traffic_suppresses_alarm (void)
{
    PnNode       *node = g_object_new (PN_TYPE_WATCHDOG, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);

    /* Consume the prearm. */
    pn_auto_trigger_kick (PN_AUTO_TRIGGER (node));
    pn_timer_pump (WATCHDOG_SETTLE_MS);
    PN_CHECK_CMPINT (d.count, ==, 0);

    /* A message arrives during the interval, so the next tick stays
     * silent rather than alarming. */
    feed_message (node);
    pn_auto_trigger_kick (PN_AUTO_TRIGGER (node));
    pn_timer_pump (WATCHDOG_SETTLE_MS);
    PN_CHECK_CMPINT (d.count, ==, 0);

    pn_timer_destroy (node, &d);
}

static void
test_watchdog_does_not_forward_input (void)
{
    PnNode       *node = g_object_new (PN_TYPE_WATCHDOG, NULL);
    PnTimerDriver d;

    /* The node watches the stream rather than passing it on, so feeding
     * it never produces an emission by itself. */
    pn_timer_attach (node, &d);
    feed_message (node);
    feed_message (node);
    pn_timer_pump (WATCHDOG_SETTLE_MS);

    PN_CHECK_CMPINT (d.count, ==, 0);

    pn_timer_destroy (node, &d);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-watchdog");
    pn_test_add ("alarm_when_idle",        test_alarm_when_idle);
    pn_test_add ("traffic_suppresses",     test_traffic_suppresses_alarm);
    pn_test_add ("does_not_forward",       test_watchdog_does_not_forward_input);
    return pn_test_run ();
}
