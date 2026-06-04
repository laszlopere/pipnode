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

/* Unit tests for PnComparator: a two-input ordering comparator that
 * latches the numeric data.value of input 1 (A) and input 2 (B) and
 * forwards a message (with data.value rewritten to 1.0 / 0.0) only when
 * the "A >= B" state changes.  Emission is synchronous in receive(), so
 * no main loop is needed.  Headless: one node under test, no IO, no
 * GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-comparator.h"

/* Records the data.value of every forwarded message so a test can
 * assert both how many state changes fired and what each emitted. */
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
make_node (gdouble hysteresis, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_comparator_new ());

    g_object_set (node, "hysteresis", hysteresis, NULL);

    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Feed one numeric value through input @input (0 = A, 1 = B). */
static void
feed (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

/* A value on a single input is latched but produces nothing: the node
 * stays silent until the other input has also carried a number. */
static void
test_waits_for_both_inputs (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    feed (node, 0, 5.0);   /* only A so far -> silent */
    feed (node, 0, 9.0);   /* still only A -> silent  */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, 1, 3.0);   /* B arrives, pair complete: 9 >= 3 -> 1.0 */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);

    g_object_unref (node);
}

/* With hysteresis 0 the node is a plain comparator: it emits on every
 * crossing of A past B, with the value rewritten to 1.0 / 0.0, and
 * stays silent while the order is unchanged.  Either input can drive a
 * change. */
static void
test_plain_comparator (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    feed (node, 1, 10.0);  /* B = 10, no A yet -> silent          */
    feed (node, 0, 4.0);   /* A = 4: 4 < 10 -> first state OFF, 0.0 */
    feed (node, 0, 8.0);   /* 8 < 10 -> no change, silent          */
    feed (node, 0, 12.0);  /* 12 >= 10 -> ON, emits 1.0            */
    feed (node, 1, 20.0);  /* B = 20: 12 < 20 -> OFF, emits 0.0    */
    feed (node, 1, 11.0);  /* B = 11: 12 >= 11 -> ON, emits 1.0    */

    PN_CHECK_CMPINT (rec.count, ==, 4);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[3], 1.0, 1e-9);

    g_object_unref (node);
}

/* The on edge is inclusive: A == B turns the output on (>= semantics). */
static void
test_on_edge_is_inclusive (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    feed (node, 1, 10.0);  /* B = 10            */
    feed (node, 0, 9.0);   /* A = 9 -> OFF, 0.0  */
    feed (node, 0, 10.0);  /* A == B -> ON, 1.0  */

    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);

    g_object_unref (node);
}

/* Hysteresis is a dead-band centered on equality: with B = 10 and
 * hysteresis 4 the output turns on only once A reaches 12 and off only
 * once A drops below 8.  Readings inside the band hold the state, so a
 * pair of values hovering around each other does not flap the output. */
static void
test_hysteresis_holds_state (void)
{
    Recorder rec;
    PnNode  *node = make_node (4.0, &rec);   /* band [B-2, B+2) */

    feed (node, 1, 10.0);  /* B = 10                                   */
    feed (node, 0, 0.0);   /* A = 0: first pair, 0 < 10 -> OFF, 0.0    */
    feed (node, 0, 11.0);  /* in band, below upper edge 12 -> stays OFF */
    feed (node, 0, 12.0);  /* reaches upper edge -> ON, emits 1.0      */
    feed (node, 0, 9.0);   /* in band, above lower edge 8 -> hold ON   */
    feed (node, 0, 8.0);   /* still >= lower edge -> hold ON, silent   */
    feed (node, 0, 7.9);   /* below lower edge -> OFF, emits 0.0       */

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 0.0, 1e-9);

    g_object_unref (node);
}

/* Moving B (the reference) can shift the band under a fixed A and flip
 * the state, with the same centered hysteresis. */
static void
test_moving_reference_flips (void)
{
    Recorder rec;
    PnNode  *node = make_node (4.0, &rec);   /* half-band = 2 */

    feed (node, 0, 10.0);  /* A = 10                                  */
    feed (node, 1, 5.0);   /* B = 5: pair done, 10 >= 5 -> ON, 1.0    */
    feed (node, 1, 9.0);   /* off-edge = 7; 10 >= 7 -> hold ON        */
    feed (node, 1, 11.0);  /* off-edge = 9; 10 >= 9 -> hold ON, silent */
    feed (node, 1, 13.0);  /* off-edge = 11; 10 < 11 -> OFF, 0.0      */

    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 0.0, 1e-9);

    g_object_unref (node);
}

/* Messages without a numeric value are ignored on either input: they
 * neither latch nor emit, so the pair stays incomplete until a real
 * number arrives on each side. */
static void
test_non_numeric_value_ignored (void)
{
    Recorder   rec;
    PnNode    *node = make_node (0.0, &rec);
    PnMessage *m;

    /* A string on input A — not a number, must not count as A's value. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "hot");
    pn_node_receive_message_on_input (node, m, 0);
    g_object_unref (m);

    feed (node, 1, 5.0);   /* B = 5, but A still unset -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, 0, 9.0);   /* A = 9 now real: 9 >= 5 -> 1.0 */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 1.0, 1e-9);

    g_object_unref (node);
}

/* The forwarded message keeps its topic and unrelated data members;
 * only "value" is rewritten to the boolean state. */
static void
test_preserves_other_members (void)
{
    Recorder   rec;
    PnNode    *node = make_node (0.0, &rec);
    PnMessage *m;

    feed (node, 1, 5.0);   /* B = 5 */

    /* A arrives on a richly-populated message; it completes the pair
     * and is mutated in place before being forwarded. */
    m = pn_message_new (NULL, "sensor/a");
    pn_message_set_double (m, "value", 42.0);
    pn_message_set_string (m, "unit", "C");
    pn_node_receive_message_on_input (node, m, 0);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m, "value"), 1.0, 1e-9);
    g_assert_cmpstr (pn_test_str (m, "unit"), ==, "C");
    g_assert_cmpstr (pn_message_get_topic (m), ==, "sensor/a");

    g_object_unref (m);
    g_object_unref (node);
}

/* A multi-input node reserves a stacked lower section: its footprint
 * grows by one PN_NODE_INPUT_ROW_HEIGHT row per input below the header,
 * while the header height itself is unchanged.  The comparator (2
 * inputs) is the worked example. */
static void
test_multi_input_footprint (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);
    double   w, h, hh, section;

    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 2);

    section = pn_node_get_input_section_height (node);
    PN_CHECK_NEAR (section, 2.0 * PN_NODE_INPUT_ROW_HEIGHT, 1e-9);

    hh = pn_node_get_header_height (node);
    pn_node_get_size (node, &w, &h);
    /* Header stays put; total height = header + the two input rows. */
    PN_CHECK_NEAR (h, hh + section, 1e-9);

    /* Header-only client-area logic must still report nothing: the
     * input section is solid body, not a paint_plot client area. */
    PN_CHECK_CMPINT (pn_node_get_client_area (node, NULL, NULL, NULL, NULL),
                     ==, FALSE);

    g_object_unref (node);
}

/* Inputs are named "value1", "value2", … by default, and a node may
 * override (and revert) any input's name. */
static void
test_input_names (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "value1");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "value2");

    pn_node_set_input_name (node, 0, "A");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "A");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "value2");

    pn_node_set_input_name (node, 0, NULL);   /* revert to default */
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "value1");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-comparator");
    pn_test_add ("waits_for_both",      test_waits_for_both_inputs);
    pn_test_add ("plain_comparator",    test_plain_comparator);
    pn_test_add ("on_edge_inclusive",   test_on_edge_is_inclusive);
    pn_test_add ("hysteresis_holds",    test_hysteresis_holds_state);
    pn_test_add ("moving_reference",    test_moving_reference_flips);
    pn_test_add ("non_numeric_ignored", test_non_numeric_value_ignored);
    pn_test_add ("preserves_members",   test_preserves_other_members);
    pn_test_add ("multi_input_footprint", test_multi_input_footprint);
    pn_test_add ("input_names",         test_input_names);
    return pn_test_run ();
}
