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

/* Unit tests for the core's opt-in per-input value collation
 * (pn_node_set_collate_inputs).  While enabled on a node with two or more
 * inputs, the core latches each input's last /data/value and, before the
 * node's receive() runs, re-injects every latched value into the arriving
 * message's data bag under the input's display name — so a multi-input
 * node sees all of its inputs at once even when only one just fired.  The
 * latch is keyed by input index (stable across renames) and stores the
 * typed JSON node, not a double, so it stays type-faithful for the day
 * /data/value is no longer a double. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"
#include "pn-message.h"
#include "pn-vector.h"

/* --- a throwaway multi-input node whose receive() does nothing; the
 *     collation happens in the core before receive(), so each test just
 *     inspects the message it passed in after dispatch. --- */

#define PROBE_TYPE_NODE (probe_node_get_type ())
G_DECLARE_FINAL_TYPE (ProbeNode, probe_node, PROBE, NODE, PnNode)

struct _ProbeNode
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (ProbeNode, probe_node, PN_TYPE_NODE)

static void
probe_node_receive (PnNode *node, PnMessage *message)
{
    /* No-op: a receive vfunc must exist for the core to run collation,
     * but the test reads the (already collated) message directly. */
    (void) node;
    (void) message;
}

static void
probe_node_class_init (ProbeNodeClass *klass)
{
    PnNodeClass *nc = PN_NODE_CLASS (klass);
    nc->class_name = "Probe Node";
    nc->category   = "Test";
    nc->receive    = probe_node_receive;
}

static void
probe_node_init (ProbeNode *self)
{
    pn_node_set_class_name (PN_NODE (self), "Probe Node");
}

/* Build a probe node with @n inputs and the given collation setting. */
static PnNode *
make_probe (gint n, gboolean collate)
{
    PnNode *node = g_object_new (PROBE_TYPE_NODE, NULL);
    pn_node_set_n_inputs (node, n);
    pn_node_set_collate_inputs (node, collate);
    return node;
}

/* Deliver a fresh message carrying data.value=@v (double) on @input and
 * return it (caller unrefs) so the test can assert on its collated bag. */
static PnMessage *
send_double (PnNode *node, gint input, gdouble v)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", v);
    pn_node_receive_message_on_input (node, m, input);
    return m;
}

/* ------------------------------------------------------------------ */

/* Flag off, two inputs: nothing is injected. */
static void
test_flag_off_no_injection (void)
{
    PnNode    *node = make_probe (2, FALSE);
    PnMessage *m    = send_double (node, 1, 7.0);

    PN_CHECK_FALSE (pn_test_has (m, "value1"));
    PN_CHECK_FALSE (pn_test_has (m, "value2"));
    PN_CHECK_NEAR  (pn_test_num (m, "value"), 7.0, 1e-9);

    g_object_unref (m);
    g_object_unref (node);
}

/* Single input + flag on: collation only applies to 2+ inputs, so it is
 * still a no-op. */
static void
test_single_input_no_op (void)
{
    PnNode    *node = make_probe (1, TRUE);
    PnMessage *m    = send_double (node, 0, 7.0);

    PN_CHECK_FALSE (pn_test_has (m, "value1"));
    PN_CHECK_NEAR  (pn_test_num (m, "value"), 7.0, 1e-9);

    g_object_unref (node);
    g_object_unref (m);
}

/* Flag on, default names: input 0 latched, then a message on input 1 sees
 * both value1 (the remembered input-0 value) and value2 (its own value). */
static void
test_default_names_injection (void)
{
    PnNode    *node = make_probe (2, TRUE);
    PnMessage *ma   = send_double (node, 0, 42.42);
    PnMessage *mb;

    /* The input-0 message itself only has value1 injected (input 1 has
     * not fired yet). */
    PN_CHECK_NEAR  (pn_test_num (ma, "value1"), 42.42, 1e-9);
    PN_CHECK_FALSE (pn_test_has (ma, "value2"));

    mb = send_double (node, 1, 7.0);
    PN_CHECK_NEAR (pn_test_num (mb, "value1"), 42.42, 1e-9);  /* latched */
    PN_CHECK_NEAR (pn_test_num (mb, "value2"), 7.0,   1e-9);  /* current */
    PN_CHECK_NEAR (pn_test_num (mb, "value"),  7.0,   1e-9);

    g_object_unref (ma);
    g_object_unref (mb);
    g_object_unref (node);
}

/* Renamed inputs: latched values surface under the custom names. */
static void
test_custom_names_injection (void)
{
    PnNode    *node = make_probe (2, TRUE);
    PnMessage *m1, *m2;

    pn_node_set_input_name (node, 0, "a");
    pn_node_set_input_name (node, 1, "b");

    m1 = send_double (node, 0, 42.42);
    m2 = send_double (node, 1, 7.0);

    PN_CHECK_NEAR  (pn_test_num (m2, "a"), 42.42, 1e-9);
    PN_CHECK_NEAR  (pn_test_num (m2, "b"), 7.0,   1e-9);
    /* The default value1/value2 names are not used now. */
    PN_CHECK_FALSE (pn_test_has (m2, "value1"));
    PN_CHECK_FALSE (pn_test_has (m2, "value2"));

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

/* The latch keeps the typed node (here int64), not a coerced double, and
 * is a copy independent of the message it was taken from. */
static void
test_typed_node_and_independence (void)
{
    PnNode    *node = make_probe (2, TRUE);
    PnMessage *m1, *m2, *m3;
    JsonNode  *injected;

    /* Latch an int64 on input 0. */
    m1 = pn_message_new (NULL, NULL);
    pn_message_set_int64 (m1, "value", 10);
    pn_node_receive_message_on_input (node, m1, 0);

    m2 = send_double (node, 1, 1.0);
    injected = pn_test_member (m2, "value1");
    PN_CHECK (injected != NULL);
    PN_CHECK_CMPINT (json_node_get_value_type (injected), ==, G_TYPE_INT64);
    PN_CHECK_NEAR   (pn_test_num (m2, "value1"), 10.0, 1e-9);

    /* Mutating the injected copy must not disturb the node's latch. */
    pn_message_set_double (m2, "value1", 999.0);
    m3 = send_double (node, 1, 2.0);
    PN_CHECK_NEAR (pn_test_num (m3, "value1"), 10.0, 1e-9);

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (m3);
    g_object_unref (node);
}

/* A message with no data.value keeps the prior latch for its own input
 * and still gets the other inputs' latched values injected. */
static void
test_missing_value_keeps_latch (void)
{
    PnNode    *node = make_probe (2, TRUE);
    PnMessage *m1   = send_double (node, 0, 5.0);
    PnMessage *m2   = pn_message_new (NULL, NULL);   /* no "value" member */

    pn_node_receive_message_on_input (node, m2, 1);

    PN_CHECK_NEAR  (pn_test_num (m2, "value1"), 5.0, 1e-9);  /* input-0 latch */
    PN_CHECK_FALSE (pn_test_has (m2, "value2"));             /* never latched */

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

/* Two inputs sharing a name collide; the higher index (later in the
 * injection loop) wins. */
static void
test_name_collision_last_wins (void)
{
    PnNode    *node = make_probe (2, TRUE);
    PnMessage *m1, *m2;

    pn_node_set_input_name (node, 0, "x");
    pn_node_set_input_name (node, 1, "x");

    m1 = send_double (node, 0, 1.0);
    m2 = send_double (node, 1, 2.0);

    PN_CHECK_NEAR (pn_test_num (m2, "x"), 2.0, 1e-9);

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Per-input value readout (pn_node_get_input_value_display)           */
/* ------------------------------------------------------------------ */

/* The readout is maintained for every multi-input node, independent of the
 * collation flag, so the worksheet can paint each input's last value. */
static void
test_readout_without_collation (void)
{
    PnNode    *node = make_probe (2, FALSE);   /* collation OFF */
    PnMessage *m1   = send_double (node, 0, 42.0);
    PnMessage *m2;

    PN_CHECK_CMPSTR (pn_node_get_input_value_display (node, 0), ==, "42");
    PN_CHECK (pn_node_get_input_value_display (node, 1) == NULL);  /* unseen */

    m2 = send_double (node, 1, 3.5);
    PN_CHECK_CMPSTR (pn_node_get_input_value_display (node, 0), ==, "42");   /* held */
    PN_CHECK_CMPSTR (pn_node_get_input_value_display (node, 1), ==, "3.5");

    /* A new value on input 0 refreshes its readout. */
    g_object_unref (m1);
    m1 = send_double (node, 0, -7.25);
    PN_CHECK_CMPSTR (pn_node_get_input_value_display (node, 0), ==, "-7.25");

    g_object_unref (m1);
    g_object_unref (m2);
    g_object_unref (node);
}

/* Single-input nodes keep no readout (NULL on every index). */
static void
test_readout_single_input_none (void)
{
    PnNode    *node = make_probe (1, FALSE);
    PnMessage *m    = send_double (node, 0, 9.0);

    PN_CHECK (pn_node_get_input_value_display (node, 0) == NULL);

    g_object_unref (m);
    g_object_unref (node);
}

/* A vector value is elided to a bounded sample, like the debug pane. */
static void
test_readout_vector_sample (void)
{
    PnNode      *node = make_probe (2, FALSE);
    PnMessage   *m    = pn_message_new (NULL, NULL);
    gdouble      data[6] = { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0 };
    PnVector    *vec  = pn_vector_new_copy (data, 6);
    const gchar *got;

    pn_message_set_vector (m, "value", vec);
    pn_node_receive_message_on_input (node, m, 0);

    got = pn_node_get_input_value_display (node, 0);
    PN_CHECK (got != NULL);
    /* First few values shown, the rest elided, with the full count. */
    PN_CHECK_CMPSTR (got, ==, "[0, 1, 2, 3, \xE2\x80\xA6] (6 values)");

    g_object_unref (vec);
    g_object_unref (m);
    g_object_unref (node);
}

/* Shrinking the input count drops the readout for inputs that vanish. */
static void
test_readout_shrink_drops_slot (void)
{
    PnNode    *node = make_probe (3, FALSE);
    PnMessage *m    = send_double (node, 2, 1.0);

    PN_CHECK_CMPSTR (pn_node_get_input_value_display (node, 2), ==, "1");
    pn_node_set_n_inputs (node, 2);
    PN_CHECK (pn_node_get_input_value_display (node, 2) == NULL);

    g_object_unref (m);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-node-collate");
    pn_test_add ("flag_off_no_injection",   test_flag_off_no_injection);
    pn_test_add ("single_input_no_op",       test_single_input_no_op);
    pn_test_add ("default_names_injection",   test_default_names_injection);
    pn_test_add ("custom_names_injection",    test_custom_names_injection);
    pn_test_add ("typed_node_independence",   test_typed_node_and_independence);
    pn_test_add ("missing_value_keeps_latch", test_missing_value_keeps_latch);
    pn_test_add ("name_collision_last_wins",  test_name_collision_last_wins);
    pn_test_add ("readout_without_collation", test_readout_without_collation);
    pn_test_add ("readout_single_input_none", test_readout_single_input_none);
    pn_test_add ("readout_vector_sample",     test_readout_vector_sample);
    pn_test_add ("readout_shrink_drops_slot", test_readout_shrink_drops_slot);
    return pn_test_run ();
}
