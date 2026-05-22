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

/* Unit tests for PnThreshold: a Schmitt-trigger comparator that turns
 * a numeric data.value into an on/off state and forwards a message
 * (with data.value rewritten to 1.0 / 0.0) only when that state
 * changes.  Emission is synchronous in receive(), so no main loop is
 * needed.  Headless: one node under test, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-threshold.h"

/* Records the data.value of every forwarded message so a test can
 * assert both how many crossings fired and what each emitted. */
typedef struct
{
    guint   count;
    gdouble seen[16];
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
make_node (gdouble threshold, gdouble hysteresis, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_threshold_new ());

    g_object_set (node,
                  "threshold",  threshold,
                  "hysteresis", hysteresis,
                  NULL);

    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Feed one numeric value through the node. */
static void
feed (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* With hysteresis 0 the node is a plain comparator: it emits on every
 * crossing of the threshold, with the value rewritten to 1.0 / 0.0,
 * and stays silent while the state is unchanged. */
static void
test_plain_comparator (void)
{
    Recorder rec;
    PnNode  *node = make_node (10.0, 0.0, &rec);

    feed (node, 5.0);    /* first message -> state OFF, emits 0.0      */
    feed (node, 7.0);    /* still below -> no change, silent           */
    feed (node, 12.0);   /* crosses up -> emits 1.0                    */
    feed (node, 20.0);   /* still on -> silent                         */
    feed (node, 3.0);    /* crosses down -> emits 0.0                  */

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 0.0, 1e-9);

    g_object_unref (node);
}

/* The on edge sits exactly at the threshold: value == threshold turns
 * the output on (>= semantics). */
static void
test_on_edge_is_inclusive (void)
{
    Recorder rec;
    PnNode  *node = make_node (10.0, 0.0, &rec);

    feed (node, 9.0);     /* first -> OFF, emits 0.0 */
    feed (node, 10.0);    /* exactly at threshold -> ON, emits 1.0 */

    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);

    g_object_unref (node);
}

/* Hysteresis widens the off trip point: once on, the value must drop a
 * full hysteresis below the threshold before the output turns off.
 * Readings inside the dead-band leave the state untouched, so a signal
 * hovering near the threshold does not flap the output. */
static void
test_hysteresis_holds_state (void)
{
    Recorder rec;
    PnNode  *node = make_node (10.0, 3.0, &rec);   /* off-trip at 7.0 */

    feed (node, 0.0);    /* first -> OFF, emits 0.0          */
    feed (node, 12.0);   /* >= 10 -> ON, emits 1.0           */
    feed (node, 8.0);    /* in band [7,10) -> hold ON, silent */
    feed (node, 9.5);    /* still in band -> hold ON, silent  */
    feed (node, 7.0);    /* still >= 7 -> hold ON, silent     */
    feed (node, 6.9);    /* below 7 -> OFF, emits 0.0         */
    feed (node, 9.9);    /* in band but rising -> stays OFF (< 10) */

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 0.0, 1e-9);

    g_object_unref (node);
}

/* The first numeric message always emits, establishing the initial
 * state — here a value above the threshold yields an immediate 1.0. */
static void
test_first_message_above_emits_on (void)
{
    Recorder rec;
    PnNode  *node = make_node (10.0, 2.0, &rec);

    feed (node, 50.0);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);

    g_object_unref (node);
}

/* Messages that carry no numeric value are ignored: they neither emit
 * nor disturb the latched state, so a later real reading still behaves
 * as if they never arrived. */
static void
test_non_numeric_value_ignored (void)
{
    Recorder   rec;
    PnNode    *node = make_node (10.0, 0.0, &rec);
    PnMessage *m;

    /* A string "value" — not a number. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "hot");
    pn_node_receive_message (node, m);
    g_object_unref (m);

    /* A message with no "value" member at all. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "other", 99.0);
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 0);

    /* The first real numeric reading still establishes state OFF. */
    feed (node, 1.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);

    g_object_unref (node);
}

/* The forwarded message keeps its topic and unrelated data members;
 * only "value" is rewritten to the boolean state. */
static void
test_preserves_other_members (void)
{
    Recorder   rec;
    PnNode    *node = make_node (10.0, 0.0, &rec);
    PnMessage *m    = pn_message_new (NULL, "sensor/temp");

    pn_message_set_double (m, "value", 42.0);
    pn_message_set_string (m, "unit", "C");

    /* receive() mutates the message in place before forwarding it, so
     * inspecting the same object after the call shows what downstream
     * saw. */
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m, "value"), 1.0, 1e-9);
    g_assert_cmpstr (pn_test_str (m, "unit"), ==, "C");
    g_assert_cmpstr (pn_message_get_topic (m), ==, "sensor/temp");

    g_object_unref (m);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-threshold");
    pn_test_add ("plain_comparator",   test_plain_comparator);
    pn_test_add ("on_edge_inclusive",  test_on_edge_is_inclusive);
    pn_test_add ("hysteresis_holds",   test_hysteresis_holds_state);
    pn_test_add ("first_msg_on",       test_first_message_above_emits_on);
    pn_test_add ("non_numeric_ignored", test_non_numeric_value_ignored);
    pn_test_add ("preserves_members",  test_preserves_other_members);
    return pn_test_run ();
}
