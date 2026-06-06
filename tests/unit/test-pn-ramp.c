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

/* Unit tests for PnRamp: a three-input vector generator.  Inputs "from"
 * (0), "to" (1) and "N" (2) each latch their numeric data.value; once all
 * three have arrived the node emits a fresh message whose data.value is a
 * $pnvector holding N evenly-spaced values from "from" to "to".  A new
 * value on ANY input recomputes from the remembered trio.  Emission is
 * synchronous in receive(), so no main loop is needed.  Headless: one
 * node, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ramp.h"
#include "pn-message.h"
#include "pn-vector.h"

/* Records a summary of every emitted ramp: how many fired, and the
 * length / endpoints / a mid-sample of the most recent one. */
typedef struct
{
    guint   count;
    gsize   len;
    gdouble first;
    gdouble last;
    gdouble mid;     /* element at len/2, or 0 when empty */
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r   = user_data;
    JsonNode *val = pn_message_get_member (message, "value");
    PnVector *vec = pn_message_resolve_vector (message, val);

    (void) node;
    r->count++;
    if (vec != NULL)
    {
        const gdouble *d = pn_vector_get_data (vec);
        r->len   = pn_vector_get_len (vec);
        r->first = r->len ? d[0]           : 0.0;
        r->last  = r->len ? d[r->len - 1]  : 0.0;
        r->mid   = r->len ? d[r->len / 2]  : 0.0;
    }
}

static PnNode *
make_node (Recorder *rec)
{
    PnNode *node = PN_NODE (pn_ramp_new ());

    rec->count = 0;
    rec->len   = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Feed one numeric value through input @input (0 = from, 1 = to, 2 = N). */
static void
feed (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

/* The node stays silent until all three inputs have carried a number;
 * the message that completes the trio emits the first ramp. */
static void
test_waits_for_all_three (void)
{
    Recorder rec;
    PnNode  *node = make_node (&rec);

    feed (node, 0, 0.0);    /* from only            -> silent */
    feed (node, 1, 10.0);   /* from + to            -> silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, 2, 5.0);    /* N arrives, trio done -> emit   */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_CMPINT ((gint) rec.len, ==, 5);

    g_object_unref (node);
}

/* from=0, to=10, N=5 -> [0, 2.5, 5, 7.5, 10]: both endpoints exact and
 * the spacing even. */
static void
test_linspace_values (void)
{
    Recorder rec;
    PnNode  *node = make_node (&rec);

    feed (node, 0, 0.0);
    feed (node, 1, 10.0);
    feed (node, 2, 5.0);

    PN_CHECK_CMPINT ((gint) rec.len, ==, 5);
    PN_CHECK_NEAR   (rec.first, 0.0,  1e-9);
    PN_CHECK_NEAR   (rec.last,  10.0, 1e-9);
    PN_CHECK_NEAR   (rec.mid,   5.0,  1e-9);   /* element [2] */

    g_object_unref (node);
}

/* Once the trio is complete a new value on ANY single input recomputes
 * the ramp and emits again, using the remembered values for the others. */
static void
test_recompute_on_any_input (void)
{
    Recorder rec;
    PnNode  *node = make_node (&rec);

    feed (node, 0, 0.0);
    feed (node, 1, 10.0);
    feed (node, 2, 5.0);     /* emit #1: len 5, [0..10] */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    feed (node, 2, 3.0);     /* only N changes -> emit #2: len 3 */
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_CMPINT ((gint) rec.len, ==, 3);
    PN_CHECK_NEAR   (rec.first, 0.0,  1e-9);
    PN_CHECK_NEAR   (rec.last,  10.0, 1e-9);

    feed (node, 1, 20.0);    /* only "to" changes -> emit #3: span stretches */
    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_CMPINT ((gint) rec.len, ==, 3);
    PN_CHECK_NEAR   (rec.last, 20.0, 1e-9);

    g_object_unref (node);
}

/* N == 1 yields a single sample equal to "from"; N below 1 produces
 * nothing at all. */
static void
test_n_edge_cases (void)
{
    Recorder rec;
    PnNode  *node = make_node (&rec);

    feed (node, 0, 4.0);
    feed (node, 1, 9.0);
    feed (node, 2, 1.0);     /* N = 1 -> single sample [4] */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_CMPINT ((gint) rec.len, ==, 1);
    PN_CHECK_NEAR   (rec.first, 4.0, 1e-9);

    feed (node, 2, 0.0);     /* N = 0 -> nothing to emit */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    feed (node, 2, 0.4);     /* rounds to 0 -> still nothing */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    g_object_unref (node);
}

/* A non-numeric message on an input neither latches nor emits, so the
 * trio stays incomplete until a real number arrives on that input. */
static void
test_non_numeric_ignored (void)
{
    Recorder   rec;
    PnNode    *node = make_node (&rec);
    PnMessage *m;

    feed (node, 0, 0.0);
    feed (node, 1, 10.0);

    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "lots");   /* not a number */
    pn_node_receive_message_on_input (node, m, 2);
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.count, ==, 0);            /* N never latched */

    feed (node, 2, 4.0);                           /* real N -> emit */
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_CMPINT ((gint) rec.len, ==, 4);

    g_object_unref (node);
}

/* The node exposes exactly three inputs, named from / to / N. */
static void
test_three_named_inputs (void)
{
    Recorder rec;
    PnNode  *node = make_node (&rec);

    PN_CHECK_CMPINT (pn_node_get_n_inputs (node), ==, 3);
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 0), ==, "from");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 1), ==, "to");
    PN_CHECK_CMPSTR (pn_node_get_input_name (node, 2), ==, "N");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ramp");
    pn_test_add ("waits_for_all_three",   test_waits_for_all_three);
    pn_test_add ("linspace_values",       test_linspace_values);
    pn_test_add ("recompute_on_any",      test_recompute_on_any_input);
    pn_test_add ("n_edge_cases",          test_n_edge_cases);
    pn_test_add ("non_numeric_ignored",   test_non_numeric_ignored);
    pn_test_add ("three_named_inputs",    test_three_named_inputs);
    return pn_test_run ();
}
