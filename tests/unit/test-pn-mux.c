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

/* Unit tests for PnMux: a value-selected multiplexer.  Input 0 is the
 * selector; inputs 1..N are data lines whose last data.value the core
 * latches.  A message emits (carrying the selected line's value) when
 * the selector changes or the selected line updates.  Headless: one
 * node, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mux.h"

#define IN_SELECT 0

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
make_node (gint data_inputs, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_mux_new ());
    if (data_inputs != 2)
        g_object_set (node, "inputs", data_inputs, NULL);
    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

static void
feed (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

/* The default node has a select input plus two data inputs (ports 1,2). */
static void
test_default_ports (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, &rec);

    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 3);
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "select");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "in1");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 2), ==, "in2");

    g_object_unref (node);
}

/* Selector 0 forwards in1, selector 1 forwards in2. */
static void
test_select_routes (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, &rec);

    feed (node, IN_SELECT, 0.0);   /* pick in1, not yet valued -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, 1, 100.0);         /* in1 updates and is selected -> 100 */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 100.0, 1e-9);

    feed (node, 2, 200.0);         /* in2 updates, not selected -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    feed (node, IN_SELECT, 1.0);   /* switch to in2 (latched 200) -> 200 */
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], 200.0, 1e-9);

    feed (node, 1, 111.0);         /* in1 updates, not selected -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 2);

    feed (node, IN_SELECT, 0.0);   /* back to in1 (now 111) -> 111 */
    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[2], 111.0, 1e-9);

    g_object_unref (node);
}

/* An out-of-range selector clamps to the last data line. */
static void
test_selector_clamps (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, &rec);

    feed (node, IN_SELECT, 9.0);   /* clamps to in2, not yet valued -> silent */
    feed (node, 1, 10.0);          /* in1 not selected -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, 2, 20.0);          /* in2 selected (clamped) -> 20 */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 20.0, 1e-9);

    g_object_unref (node);
}

/* Growing the data-input count adds ports that route like the rest. */
static void
test_resize_inputs (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, &rec);

    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 4);
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 3), ==, "in3");

    feed (node, 3, 300.0);         /* in3, not selected (sel default 0) */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, IN_SELECT, 2.0);   /* pick in3 -> 300 */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 300.0, 1e-9);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mux");
    pn_test_add ("default_ports",   test_default_ports);
    pn_test_add ("select_routes",   test_select_routes);
    pn_test_add ("selector_clamps", test_selector_clamps);
    pn_test_add ("resize_inputs",   test_resize_inputs);
    return pn_test_run ();
}
