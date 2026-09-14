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

/* Unit tests for PnRoundRobin: an "in" and a "reset" input, N outputs,
 * each message leaving by the next output in turn.  Emission is
 * synchronous in receive(), so no main loop is needed.  Headless: no IO,
 * no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-round-robin.h"
#include "pn-wire.h"

#include <string.h>

typedef struct
{
    guint count;
    gint  output[64];
    gdouble value[64];
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
make_node (gint outputs, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_round_robin_new ());

    g_object_set (node, "outputs", outputs, NULL);
    memset (rec, 0, sizeof *rec);
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

static void
send_on (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);

    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

static void
send (PnNode *node, gdouble value)
{
    send_on (node, PN_ROUND_ROBIN_IN_MESSAGE, value);
}

static gint
next_of (PnNode *node)
{
    return pn_round_robin_get_next (PN_ROUND_ROBIN (node));
}

/* ------------------------------------------------------------------ */

static void
test_defaults (void)
{
    PnNode *node = PN_NODE (pn_round_robin_new ());
    gint    outputs;

    g_object_get (node, "outputs", &outputs, NULL);
    PN_CHECK_CMPINT (outputs, ==, PN_ROUND_ROBIN_DEF_OUTPUTS);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 3);
    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 2);
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "in");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "reset");
    PN_CHECK_CMPINT (next_of (node), ==, 0);

    g_object_unref (node);
}

/* One output per message, in turn, wrapping after the last; the message
 * goes out unchanged. */
static void
test_rotation_and_wrap (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, &rec);
    guint    i;

    for (i = 0; i < 7; i++)
        send (node, (gdouble) (i + 1));

    PN_CHECK_CMPINT (rec.count, ==, 7u);
    for (i = 0; i < 7; i++)
    {
        PN_CHECK_CMPINT (rec.output[i], ==, (gint) (i % 3));
        PN_CHECK_NEAR   (rec.value[i], (gdouble) (i + 1), 1e-9);
    }
    PN_CHECK_CMPINT (next_of (node), ==, 1);

    g_object_unref (node);
}

/* Reset makes output 1 next and emits nothing. */
static void
test_reset (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, &rec);

    send (node, 1.0);
    send (node, 2.0);
    PN_CHECK_CMPINT (next_of (node), ==, 2);

    send_on (node, PN_ROUND_ROBIN_IN_RESET, 9.0);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (next_of (node), ==, 0);

    send (node, 3.0);
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (rec.output[2], ==, 0);

    send (node, 4.0);
    pn_round_robin_reset (PN_ROUND_ROBIN (node));
    PN_CHECK_CMPINT (next_of (node), ==, 0);

    g_object_unref (node);
}

/* Shrinking past the pointer wraps it to output 1; shrinking while it
 * still fits keeps it; growing keeps it. */
static void
test_resize (void)
{
    Recorder rec;
    PnNode  *node = make_node (5, &rec);
    gint     i;

    for (i = 0; i < 4; i++)
        send (node, 1.0);
    PN_CHECK_CMPINT (next_of (node), ==, 4);

    g_object_set (node, "outputs", 3, NULL);
    PN_CHECK_CMPINT (next_of (node), ==, 0);

    send (node, 1.0);
    send (node, 1.0);
    PN_CHECK_CMPINT (next_of (node), ==, 2);

    g_object_set (node, "outputs", 3, NULL);   /* no change */
    PN_CHECK_CMPINT (next_of (node), ==, 2);

    g_object_set (node, "outputs", 8, NULL);
    PN_CHECK_CMPINT (next_of (node), ==, 2);
    send (node, 1.0);
    PN_CHECK_CMPINT (rec.output[rec.count - 1], ==, 2);

    g_object_set (node, "outputs", 4, NULL);   /* 3 still fits */
    PN_CHECK_CMPINT (next_of (node), ==, 3);

    g_object_unref (node);
}

/* A message fed back from an output into "in" during the emit takes the
 * following output: the turn advances before the emission. */
static void
test_feedback_takes_next_output (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, &rec);
    PnWire  *loop = pn_wire_new_ports (node, 0, node, PN_ROUND_ROBIN_IN_MESSAGE);

    send (node, 1.0);   /* out1 -> loops back once -> out2 */

    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.output[0], ==, 0);
    PN_CHECK_CMPINT (rec.output[1], ==, 1);
    PN_CHECK_CMPINT (next_of (node), ==, 2);

    g_object_unref (loop);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-round-robin");
    pn_test_add ("defaults",          test_defaults);
    pn_test_add ("rotation_and_wrap", test_rotation_and_wrap);
    pn_test_add ("reset",             test_reset);
    pn_test_add ("resize",            test_resize);
    pn_test_add ("feedback_next",     test_feedback_takes_next_output);
    return pn_test_run ();
}
