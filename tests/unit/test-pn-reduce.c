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

/* Unit tests for PnReduce: a vector -> scalar fold.  Reads data.value;
 * when it is a $pnvector the node folds it per its "mode" property and
 * overwrites data.value with the scalar; a plain scalar folds as a
 * one-element vector (pass-through).  Emission is synchronous in
 * receive(), so no main loop is needed.  Headless: one node, no IO, no
 * GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-reduce.h"
#include "pn-message.h"
#include "pn-vector.h"

typedef struct
{
    guint   count;
    gdouble last;    /* data.value of the most recent emitted message */
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    r->last = pn_test_num (message, "value");
    r->count++;
}

static PnNode *
make_node (PnReduceMode mode, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_reduce_new ());

    g_object_set (node, "mode", mode, NULL);
    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Feed a vector data.value built from @n doubles. */
static void
feed_vector (PnNode *node, const gdouble *d, gsize n)
{
    PnMessage *m   = pn_message_new (NULL, NULL);
    PnVector  *vec = pn_vector_new_copy (d, n);

    pn_message_set_vector (m, "value", vec);
    pn_node_receive_message (node, m);
    g_object_unref (vec);
    g_object_unref (m);
}

/* Run the standard probe vector [1, 2, 3, 4] through @mode and return the
 * folded scalar. */
static gdouble
fold4 (PnReduceMode mode)
{
    static const gdouble d[4] = { 1.0, 2.0, 3.0, 4.0 };
    Recorder rec;
    PnNode  *node = make_node (mode, &rec);
    gdouble  result;

    feed_vector (node, d, 4);
    g_assert_cmpint (rec.count, ==, 1);
    result = rec.last;

    g_object_unref (node);
    return result;
}

/* Every mode folds [1, 2, 3, 4] to its defining scalar. */
static void
test_all_modes (void)
{
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_SUM),     10.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_MEAN),     2.5, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_MIN),      1.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_MAX),      4.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_RANGE),    3.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_COUNT),    4.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_PRODUCT), 24.0, 1e-9);
    /* population stddev of [1,2,3,4] = sqrt(1.25). */
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_STDDEV), 1.1180339887, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_FIRST),   1.0, 1e-9);
    PN_CHECK_NEAR (fold4 (PN_REDUCE_MODE_LAST),    4.0, 1e-9);
}

/* A plain scalar data.value folds as a one-element vector: it always
 * passes through unchanged for value-like folds and counts as 1. */
static void
test_scalar_passthrough (void)
{
    Recorder rec;
    PnNode  *node;

    node = make_node (PN_REDUCE_MODE_SUM, &rec);
    {
        PnMessage *m = pn_message_new (NULL, NULL);
        pn_message_set_double (m, "value", 7.0);
        pn_node_receive_message (node, m);
        g_object_unref (m);
    }
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.last, 7.0, 1e-9);   /* sum of [7] = 7 */
    g_object_unref (node);

    node = make_node (PN_REDUCE_MODE_COUNT, &rec);
    {
        PnMessage *m = pn_message_new (NULL, NULL);
        pn_message_set_double (m, "value", 7.0);
        pn_node_receive_message (node, m);
        g_object_unref (m);
    }
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.last, 1.0, 1e-9);   /* count of [7] = 1 */
    g_object_unref (node);
}

/* The forwarded message keeps its topic; only data.value is rewritten to
 * the folded scalar. */
static void
test_preserves_topic (void)
{
    static const gdouble d[3] = { 2.0, 4.0, 6.0 };
    Recorder   rec;
    PnNode    *node = make_node (PN_REDUCE_MODE_MEAN, &rec);
    PnMessage *m    = pn_message_new (NULL, "sensors/x");
    PnVector  *vec  = pn_vector_new_copy (d, 3);

    pn_message_set_vector (m, "value", vec);
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m, "value"), 4.0, 1e-9);   /* mean */
    g_assert_cmpstr (pn_message_get_topic (m), ==, "sensors/x");

    g_object_unref (vec);
    g_object_unref (m);
    g_object_unref (node);
}

/* A message with no numeric/vector value is dropped: nothing to fold. */
static void
test_non_numeric_ignored (void)
{
    Recorder   rec;
    PnNode    *node = make_node (PN_REDUCE_MODE_SUM, &rec);
    PnMessage *m    = pn_message_new (NULL, NULL);

    pn_message_set_string (m, "value", "nope");
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (rec.count, ==, 0);

    g_object_unref (m);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-reduce");
    pn_test_add ("all_modes",           test_all_modes);
    pn_test_add ("scalar_passthrough",  test_scalar_passthrough);
    pn_test_add ("preserves_topic",     test_preserves_topic);
    pn_test_add ("non_numeric_ignored", test_non_numeric_ignored);
    return pn_test_run ();
}
