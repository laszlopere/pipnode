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

/* Unit tests for PnStaircase: a monostable timer that forwards a trigger
 * (data.value > 0.5), stays silent for on-time-ms, then emits a
 * data.value = 0.0 turn-off message of its own.  The turn-off rides a
 * one-shot main-loop timer, so a test must drive a #GMainContext to
 * observe it rather than see it land synchronously.  Headless: one node,
 * no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-staircase.h"

/* Pump the default main context until @counter reaches @want or a safety
 * deadline passes, so a regression cannot hang the suite. */
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

/* Pump the default main context for a fixed wall-clock duration, used to
 * let a window run partway down without waiting on a counter. */
static void
pump_for (guint ms)
{
    GMainContext *ctx = g_main_context_default ();
    gint64        end = g_get_monotonic_time () + (gint64) ms * 1000;

    while (g_get_monotonic_time () < end)
    {
        g_main_context_iteration (ctx, FALSE);
        g_usleep (500);
    }
}

/* Records the data.value of each forwarded message, so a test can assert
 * both the count and the on/off order. */
typedef struct
{
    guint   count;
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

static PnNode *
make_node (guint on_time_ms, gboolean retriggerable, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_staircase_new ());

    g_object_set (node,
                  "on-time-ms",    on_time_ms,
                  "retriggerable", retriggerable,
                  NULL);

    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

static void
send_value (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* A trigger is forwarded right away, then after the on-time the node
 * emits its own value = 0.0 turn-off message. */
static void
test_trigger_forwards_then_off (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = make_node (20, TRUE, &rec);

    send_value (node, 1.0);

    /* The trigger passed straight through, unchanged. */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);

    /* ...and the turn-off arrives only after the window elapses. */
    pump_until (&rec.count, 2, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 0.0, 1e-9);

    g_object_unref (node);
}

/* Messages that are not triggers — value at or below 0.5, or no numeric
 * value at all — are dropped and never arm the timer. */
static void
test_non_triggers_dropped (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = make_node (20, TRUE, &rec);
    PnMessage *bare = pn_message_new (NULL, NULL);   /* no "value" member */

    send_value (node, 0.0);
    send_value (node, 0.5);    /* the boundary itself is not a trigger */
    send_value (node, 0.3);

    pn_node_receive_message (node, bare);
    g_object_unref (bare);

    /* Nothing forwarded, and pumping produces no stray turn-off because
     * no window was ever opened. */
    pump_for (60);
    PN_CHECK_CMPINT (rec.count, ==, 0);

    g_object_unref (node);
}

/* While on, a fresh trigger is swallowed (never re-forwarded) whether or
 * not the node is retriggerable; exactly one turn-off eventually fires,
 * so the stream is on, (silence), off — two emissions total. */
static void
test_retrigger_not_reforwarded (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = make_node (40, TRUE, &rec);

    send_value (node, 1.0);          /* opens the window: forwarded */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    send_value (node, 1.0);          /* in-window re-trigger: swallowed */
    send_value (node, 2.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);

    pump_until (&rec.count, 2, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 0.0, 1e-9);

    /* No extra turn-off from the swallowed triggers. */
    pump_for (60);
    PN_CHECK_CMPINT (rec.count, ==, 2);

    g_object_unref (node);
}

/* Retriggerable: a trigger that arrives mid-window pushes the turn-off
 * out, so it has not fired by the time the original window would have
 * closed. */
static void
test_retriggerable_extends_window (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = make_node (200, TRUE, &rec);

    send_value (node, 1.0);          /* window would close at ~200 ms */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    pump_for (100);                  /* halfway in */
    send_value (node, 1.0);          /* re-arm: now closes at ~300 ms */

    /* At ~250 ms the un-extended window would already have fired the
     * turn-off; the extended one has not. */
    pump_for (150);
    PN_CHECK_CMPINT (rec.count, ==, 1);

    /* It does still fire, eventually. */
    pump_until (&rec.count, 2, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 0.0, 1e-9);

    g_object_unref (node);
}

/* Non-retriggerable: a mid-window trigger is ignored, so the turn-off
 * stays on its original schedule and fires while triggers keep coming. */
static void
test_non_retriggerable_keeps_schedule (void)
{
    Recorder  rec  = { 0, { 0 } };
    PnNode   *node = make_node (80, FALSE, &rec);

    send_value (node, 1.0);          /* window closes at ~80 ms, fixed */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    pump_for (40);
    send_value (node, 1.0);          /* ignored: schedule unchanged */

    /* By ~160 ms the original window has definitely closed. */
    pump_until (&rec.count, 2, 2000);
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 0.0, 1e-9);

    g_object_unref (node);
}

/* Captures the topic of every forwarded message, so a test can confirm
 * the synthesised turn-off inherits the trigger's topic. */
typedef struct
{
    guint  count;
    gchar *topics[4];
} TopicRecorder;

static void
topic_recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    TopicRecorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->topics))
        r->topics[r->count] = g_strdup (pn_message_get_topic (message));
    r->count++;
}

/* The turn-off message carries the topic of the trigger that opened the
 * window, so downstream can pair the on and off edges. */
static void
test_off_inherits_trigger_topic (void)
{
    TopicRecorder rec  = { 0, { NULL } };
    PnNode       *node = PN_NODE (pn_staircase_new ());
    PnMessage    *trig = pn_message_new (NULL, "stairwell");

    g_object_set (node, "on-time-ms", 20u, NULL);
    g_signal_connect (node, "message", G_CALLBACK (topic_recorder_cb), &rec);

    pn_message_set_double (trig, "value", 1.0);
    pn_node_receive_message (node, trig);
    g_object_unref (trig);

    pump_until (&rec.count, 2, 2000);

    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_CMPSTR (rec.topics[0], ==, "stairwell");   /* forwarded trigger */
    PN_CHECK_CMPSTR (rec.topics[1], ==, "stairwell");   /* synthesised off */

    for (guint i = 0; i < rec.count && i < G_N_ELEMENTS (rec.topics); i++)
        g_free (rec.topics[i]);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-staircase");
    pn_test_add ("trigger_forwards_then_off",  test_trigger_forwards_then_off);
    pn_test_add ("non_triggers_dropped",       test_non_triggers_dropped);
    pn_test_add ("retrigger_not_reforwarded",  test_retrigger_not_reforwarded);
    pn_test_add ("retriggerable_extends",      test_retriggerable_extends_window);
    pn_test_add ("non_retrig_keeps_schedule",  test_non_retriggerable_keeps_schedule);
    pn_test_add ("off_inherits_topic",         test_off_inherits_trigger_topic);
    return pn_test_run ();
}
