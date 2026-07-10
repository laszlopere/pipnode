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

/* Unit tests for the jump flags: a message reaching a PnJumpIn is
 * delivered to every PnJumpOut sharing its tag, document-wide.  Also
 * covers the has-error state that surfaces a half-connected flag. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-jump-in.h"
#include "pn-jump-out.h"
#include "pn-flow.h"
#include "pn-wire.h"
#include "pn-wire-store.h"

/* Add a flag of the given direction to @flow and count what it emits. */
static PnNode *
add_in (PnFlow *flow, const gchar *tag)
{
    PnNode *node = g_object_new (PN_TYPE_JUMP_IN, "tag", tag, NULL);

    pn_flow_add_node (flow, node);
    return node;
}

static PnNode *
add_out (PnFlow *flow, const gchar *tag, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_JUMP_OUT, "tag", tag, NULL);

    pn_flow_add_node (flow, node);

    if (out_emits != NULL)
    {
        *out_emits = 0;
        g_signal_connect (node, "message",
                          G_CALLBACK (pn_test_count_emits), out_emits);
    }
    return node;
}

static void
send (PnNode *node, gdouble value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* ------------------------------------------------------------------ */

static void
test_delivers_to_matching_tag_only (void)
{
    PnFlow *flow = pn_flow_new ();
    guint   hit = 0, miss = 0;
    PnNode *in;

    in = add_in (flow, "temp");
    add_out (flow, "temp",  &hit);
    add_out (flow, "other", &miss);

    send (in, 1.0);

    PN_CHECK_CMPINT (hit,  ==, 1);
    PN_CHECK_CMPINT (miss, ==, 0);

    g_object_unref (flow);
}

/* Many-to-many: one jump-in feeds every same-tag jump-out, and two
 * jump-ins sharing a tag merge onto the same bus. */
static void
test_fans_out_to_every_match (void)
{
    PnFlow *flow = pn_flow_new ();
    guint   a = 0, b = 0;
    PnNode *in1, *in2;

    in1 = add_in (flow, "bus");
    in2 = add_in (flow, "bus");
    add_out (flow, "bus", &a);
    add_out (flow, "bus", &b);

    send (in1, 1.0);
    PN_CHECK_CMPINT (a, ==, 1);
    PN_CHECK_CMPINT (b, ==, 1);

    send (in2, 2.0);
    PN_CHECK_CMPINT (a, ==, 2);
    PN_CHECK_CMPINT (b, ==, 2);

    g_object_unref (flow);
}

/* Each jump-out must get its own deep copy: the first handler mutating
 * its message must not be visible to the second, exactly as for a
 * wire's fan-out. */
static void
mutate_value (PnNode *node, PnMessage *message, gpointer unused)
{
    (void) node; (void) unused;
    pn_message_set_double (message, "value", 99.0);
}

static void
record_value (PnNode *node, PnMessage *message, gpointer seen)
{
    (void) node;
    *(gdouble *) seen =
        json_object_get_double_member (pn_message_get_data (message), "value");
}

static void
test_fan_out_branches_are_independent (void)
{
    PnFlow  *flow = pn_flow_new ();
    gdouble  seen = 0.0;
    PnNode  *in, *first, *second;

    in     = add_in  (flow, "bus");
    first  = add_out (flow, "bus", NULL);
    second = add_out (flow, "bus", NULL);

    g_signal_connect (first,  "message", G_CALLBACK (mutate_value), NULL);
    g_signal_connect (second, "message", G_CALLBACK (record_value), &seen);

    send (in, 1.0);

    PN_CHECK_NEAR (seen, 1.0, 1e-9);

    g_object_unref (flow);
}

/* An empty tag names nothing: two untagged flags must not connect, or
 * every freshly-dropped pair would join one accidental bus. */
static void
test_empty_tag_never_matches (void)
{
    PnFlow *flow = pn_flow_new ();
    guint   emits = 0;
    PnNode *in;

    in = add_in (flow, "");
    add_out (flow, "", &emits);

    send (in, 1.0);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (flow);
}

static void
test_disabled_jump_out_drops (void)
{
    PnFlow *flow = pn_flow_new ();
    guint   emits = 0;
    PnNode *in, *out;

    in  = add_in  (flow, "t");
    out = add_out (flow, "t", &emits);
    pn_node_set_disabled (out, TRUE);

    send (in, 1.0);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (flow);
}

/* A flag with no owning flow (freshly constructed, not yet added) must
 * not crash on receive. */
static void
test_orphan_jump_in_is_inert (void)
{
    PnNode *in = g_object_new (PN_TYPE_JUMP_IN, "tag", "t", NULL);

    send (in, 1.0);
    PN_CHECK (TRUE);

    g_object_unref (in);
}

/* ------------------------------------------------------------------ */
/*  has-error                                                          */
/* ------------------------------------------------------------------ */

static void
test_unmatched_flag_is_error (void)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *in, *out, *lonely;

    /* Untagged: error on sight. */
    lonely = add_in (flow, "");
    PN_CHECK (pn_node_get_has_error (lonely));

    /* Tagged but with no counterpart yet. */
    in = add_in (flow, "temp");
    PN_CHECK (pn_node_get_has_error (in));

    /* Adding the partner clears both ends. */
    out = add_out (flow, "temp", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (in));
    PN_CHECK_FALSE (pn_node_get_has_error (out));

    /* Two jump-ins on one tag is legal — the second needs no new
     * jump-out to be considered connected. */
    PN_CHECK_FALSE (pn_node_get_has_error (add_in (flow, "temp")));

    g_object_unref (flow);
}

/* Retagging re-forms the connection: the abandoned partner goes into
 * error, and a flag that finds a new partner comes out of it. */
static void
test_retag_restrands_partner (void)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *in, *out;

    in  = add_in  (flow, "a");
    out = add_out (flow, "a", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (in));

    pn_jump_out_set_tag (PN_JUMP_OUT (out), "b");
    PN_CHECK (pn_node_get_has_error (in));
    PN_CHECK (pn_node_get_has_error (out));

    pn_jump_in_set_tag (PN_JUMP_IN (in), "b");
    PN_CHECK_FALSE (pn_node_get_has_error (in));
    PN_CHECK_FALSE (pn_node_get_has_error (out));

    g_object_unref (flow);
}

static void
test_removing_partner_strands_survivor (void)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *in, *out;

    in  = add_in  (flow, "a");
    out = add_out (flow, "a", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (in));

    pn_node_store_remove (pn_flow_get_nodes (flow), out);
    PN_CHECK (pn_node_get_has_error (in));

    g_object_unref (flow);
}

/* ------------------------------------------------------------------ */
/*  Cycles                                                             */
/* ------------------------------------------------------------------ */

/* out --wire--> in, both tagged "loop": the jump closes a feedback path
 * a wire alone could not.  The thread-local dispatch-depth guard in
 * pn_node_receive_message_on_input must stop it rather than recurse
 * until the stack dies. */
static void
test_tag_cycle_terminates (void)
{
    PnFlow *flow = pn_flow_new ();
    guint   emits = 0;
    PnNode *in, *out;
    PnWire *wire;

    in   = add_in  (flow, "loop");
    out  = add_out (flow, "loop", &emits);
    wire = pn_wire_new (out, in);
    pn_wire_store_add (pn_flow_get_wires (flow), wire);

    send (in, 1.0);

    /* Bounded, and bounded by the dispatch guard rather than by luck. */
    PN_CHECK (emits > 0);
    PN_CHECK (emits <= 256);

    g_object_unref (wire);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-jump");
    pn_test_add ("delivers_to_matching_tag",  test_delivers_to_matching_tag_only);
    pn_test_add ("fans_out_to_every_match",   test_fans_out_to_every_match);
    pn_test_add ("fan_out_independent",       test_fan_out_branches_are_independent);
    pn_test_add ("empty_tag_never_matches",   test_empty_tag_never_matches);
    pn_test_add ("disabled_jump_out_drops",   test_disabled_jump_out_drops);
    pn_test_add ("orphan_jump_in_is_inert",   test_orphan_jump_in_is_inert);
    pn_test_add ("unmatched_flag_is_error",   test_unmatched_flag_is_error);
    pn_test_add ("retag_restrands_partner",   test_retag_restrands_partner);
    pn_test_add ("remove_strands_survivor",   test_removing_partner_strands_survivor);
    pn_test_add ("tag_cycle_terminates",      test_tag_cycle_terminates);
    return pn_test_run ();
}
