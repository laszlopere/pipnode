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

/* Unit tests for PnShiftRegister — one input, N outputs, an open-ended
 * shift register of whole messages — and for the multi-output port model
 * it is the first user of: pn_node_emit_message_on_output() tags each
 * emission with its port, and a PnWire forwards only the emissions that
 * leave by its own source output.  Emission is synchronous in receive(),
 * so no main loop is needed.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-shift-register.h"
#include "pn-expression2.h"
#include "pn-flow.h"
#include "pn-wire.h"

#include <string.h>

/* One recorded emission: which output it left by, its data.value, and
 * its data.note (NULL when absent). */
typedef struct
{
    gint     output;
    gdouble  value;
    gchar   *note;
} Emission;

typedef struct
{
    guint    count;
    Emission seen[32];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
    {
        Emission *e = &r->seen[r->count];

        e->output = pn_node_current_output ();
        e->value  = pn_test_has (message, "value")
                  ? pn_test_num (message, "value") : 0.0;
        g_free (e->note);
        e->note   = pn_test_has (message, "note")
                  ? g_strdup (pn_test_str (message, "note")) : NULL;
    }
    r->count++;
}

static void
recorder_clear (Recorder *r)
{
    guint i;
    for (i = 0; i < G_N_ELEMENTS (r->seen); i++)
        g_clear_pointer (&r->seen[i].note, g_free);
    r->count = 0;
}

static PnNode *
make_node (gint outputs, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_shift_register_new ());

    g_object_set (node, "outputs", outputs, NULL);
    if (rec != NULL)
    {
        memset (rec, 0, sizeof *rec);
        g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    }
    return node;
}

static void
send (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

static guint
filled (PnNode *node)
{
    return pn_shift_register_get_n_filled (PN_SHIFT_REGISTER (node));
}

/* ------------------------------------------------------------------ */
/*  Ports                                                              */
/* ------------------------------------------------------------------ */

/* A fresh node has one input and the default four outputs; the stacked
 * port section is sized by the output side. */
static void
test_default_ports (void)
{
    PnNode *node    = PN_NODE (pn_shift_register_new ());
    gint    outputs = 0;

    g_object_get (node, "outputs", &outputs, NULL);
    PN_CHECK_CMPINT (outputs, ==, PN_SHIFT_REGISTER_DEF_OUTPUTS);
    PN_CHECK_CMPINT (pn_node_get_n_inputs  (node), ==, 1);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 4);
    PN_CHECK_NEAR   (pn_node_get_port_section_height (node),
                     4.0 * PN_NODE_INPUT_ROW_HEIGHT, 1e-9);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "out1");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 3), ==, "out4");

    g_object_unref (node);
}

/* An ordinary node still reports one output (none once has-output is
 * off) and no port section: the model change is invisible to it. */
static void
test_plain_node_single_output (void)
{
    PnNode *node = pn_node_new ();

    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 1);
    PN_CHECK_NEAR   (pn_node_get_port_section_height (node), 0.0, 1e-9);
    pn_node_set_has_output (node, FALSE);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 0);

    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Shifting                                                           */
/* ------------------------------------------------------------------ */

/* The first message goes out on output 1 only; unfilled stages are
 * silent. */
static void
test_first_message_output_one_only (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, &rec);

    send (node, 10.0);
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 0);
    PN_CHECK_NEAR   (rec.seen[0].value, 10.0, 1e-9);
    PN_CHECK_CMPINT (filled (node), ==, 1u);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* Each message re-emits every filled stage, newest on output 1, oldest
 * stage first and output 1 last. */
static void
test_shift_sequence_and_order (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, &rec);

    send (node, 1.0);     /* out1=1               */
    send (node, 2.0);     /* out2=1 out1=2        */
    send (node, 3.0);     /* out3=1 out2=2 out1=3 */
    PN_CHECK_CMPINT (rec.count, ==, 6u);

    PN_CHECK_CMPINT (rec.seen[1].output, ==, 1);
    PN_CHECK_NEAR   (rec.seen[1].value, 1.0, 1e-9);
    PN_CHECK_CMPINT (rec.seen[2].output, ==, 0);
    PN_CHECK_NEAR   (rec.seen[2].value, 2.0, 1e-9);

    PN_CHECK_CMPINT (rec.seen[3].output, ==, 2);
    PN_CHECK_NEAR   (rec.seen[3].value, 1.0, 1e-9);
    PN_CHECK_CMPINT (rec.seen[4].output, ==, 1);
    PN_CHECK_NEAR   (rec.seen[4].value, 2.0, 1e-9);
    PN_CHECK_CMPINT (rec.seen[5].output, ==, 0);
    PN_CHECK_NEAR   (rec.seen[5].value, 3.0, 1e-9);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* Open-ended, not looped: once full, the oldest message falls off. */
static void
test_oldest_falls_off (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, &rec);

    send (node, 1.0);
    send (node, 2.0);
    rec.count = 0;
    send (node, 3.0);

    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0].value, 2.0, 1e-9);
    PN_CHECK_CMPINT (rec.seen[1].output, ==, 0);
    PN_CHECK_NEAR   (rec.seen[1].value, 3.0, 1e-9);
    PN_CHECK_CMPINT (filled (node), ==, 2u);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* The whole message is shifted, every data member included —
 * not just data.value. */
static void
test_whole_message_shifted (void)
{
    Recorder   rec;
    PnNode    *node = make_node (2, &rec);
    PnMessage *m    = pn_message_new (NULL, "/a/topic");

    pn_message_set_string (m, "note", "first");
    pn_node_receive_message (node, m);
    g_object_unref (m);

    send (node, 2.0);

    /* Second send: seen[1] is stage 2 (the "first" message) on output 2. */
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (rec.seen[1].output, ==, 1);
    PN_CHECK_CMPSTR (rec.seen[1].note, ==, "first");
    PN_CHECK_CMPINT (rec.seen[2].output, ==, 0);
    PN_CHECK (rec.seen[2].note == NULL);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* A handler that edits the message it is handed cannot reach back into
 * the stored stage. */
static void
mutate_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    (void) node; (void) user_data;
    pn_message_set_double (message, "value", -1.0);
}

static void
test_stages_isolated_from_handlers (void)
{
    Recorder rec;
    PnNode  *node = PN_NODE (pn_shift_register_new ());

    g_object_set (node, "outputs", 3, NULL);
    g_signal_connect (node, "message", G_CALLBACK (mutate_cb), NULL);
    memset (&rec, 0, sizeof rec);
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), &rec);

    send (node, 7.0);
    rec.count = 0;
    send (node, 8.0);

    /* Recorder runs after the mutator, so it sees -1 — but the stage the
     * mutator was handed last time still carries 7 into output 2. */
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0].value, -1.0, 1e-9);

    g_signal_handlers_disconnect_by_func (node, mutate_cb, NULL);
    rec.count = 0;
    send (node, 9.0);
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_NEAR   (rec.seen[0].value, 7.0, 1e-9);   /* output 3 */
    PN_CHECK_NEAR   (rec.seen[1].value, 8.0, 1e-9);   /* output 2 */
    PN_CHECK_NEAR   (rec.seen[2].value, 9.0, 1e-9);   /* output 1 */

    g_object_unref (node);
    recorder_clear (&rec);
}

/* Shrinking keeps the newest stages that still fit; growing adds empty
 * (silent) stages at the end. */
static void
test_resize_keeps_newest (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, &rec);

    send (node, 1.0);
    send (node, 2.0);
    send (node, 3.0);
    send (node, 4.0);

    g_object_set (node, "outputs", 2, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 2);
    PN_CHECK_CMPINT (filled (node), ==, 2u);

    rec.count = 0;
    send (node, 5.0);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_NEAR   (rec.seen[0].value, 4.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1].value, 5.0, 1e-9);

    g_object_set (node, "outputs", 3, NULL);
    rec.count = 0;
    send (node, 6.0);
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 2);
    PN_CHECK_NEAR   (rec.seen[0].value, 4.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2].value, 6.0, 1e-9);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* Clearing empties every stage silently. */
static void
test_clear (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, &rec);

    send (node, 1.0);
    send (node, 2.0);
    rec.count = 0;

    pn_shift_register_clear (PN_SHIFT_REGISTER (node));
    PN_CHECK_CMPINT (rec.count, ==, 0u);
    PN_CHECK_CMPINT (filled (node), ==, 0u);
    PN_CHECK (pn_node_get_output_value_display (node, 0) == NULL);

    send (node, 3.0);
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.seen[0].output, ==, 0);

    g_object_unref (node);
    recorder_clear (&rec);
}

/* Each output keeps a readout of the value last sent through it, for
 * the worksheet to paint beside the port. */
static void
test_output_value_readout (void)
{
    PnNode *node = make_node (3, NULL);

    PN_CHECK (pn_node_get_output_value_display (node, 0) == NULL);

    send (node, 1.5);
    send (node, 2.0);
    PN_CHECK_CMPSTR (pn_node_get_output_value_display (node, 0), ==, "2");
    PN_CHECK_CMPSTR (pn_node_get_output_value_display (node, 1), ==, "1.5");
    PN_CHECK (pn_node_get_output_value_display (node, 2) == NULL);

    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Wires                                                              */
/* ------------------------------------------------------------------ */

static void
count_passed (PnWire *wire, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    (void) wire;
    if (r->count < G_N_ELEMENTS (r->seen))
        r->seen[r->count].value = pn_test_num (message, "value");
    r->count++;
}

/* A wire carries only the emissions that leave by its source output. */
static void
test_wire_routes_by_output (void)
{
    PnNode  *sr   = make_node (3, NULL);
    PnNode  *sink = PN_NODE (pn_shift_register_new ());
    PnWire  *w0   = pn_wire_new_ports (sr, 0, sink, 0);
    PnWire  *w2   = pn_wire_new_ports (sr, 2, sink, 0);
    Recorder r0, r2;

    memset (&r0, 0, sizeof r0);
    memset (&r2, 0, sizeof r2);
    g_signal_connect (w0, "message-passed", G_CALLBACK (count_passed), &r0);
    g_signal_connect (w2, "message-passed", G_CALLBACK (count_passed), &r2);

    PN_CHECK_CMPINT (pn_wire_get_source_output (w2), ==, 2);

    send (sr, 1.0);
    send (sr, 2.0);
    PN_CHECK_CMPINT (r0.count, ==, 2u);
    PN_CHECK_CMPINT (r2.count, ==, 0u);

    send (sr, 3.0);
    PN_CHECK_CMPINT (r0.count, ==, 3u);
    PN_CHECK_CMPINT (r2.count, ==, 1u);
    PN_CHECK_NEAR   (r2.seen[0].value, 1.0, 1e-9);

    /* Everything that crossed a wire reached the sink. */
    PN_CHECK_CMPINT (filled (sink), ==, 4u);

    g_object_unref (w0);
    g_object_unref (w2);
    g_object_unref (sink);
    g_object_unref (sr);
}

/* Messages on output 0 (plain pn_node_emit_message) keep flowing through
 * the ordinary wires every existing node uses. */
static void
test_plain_wire_is_output_zero (void)
{
    PnNode  *src  = pn_node_new ();
    PnNode  *sink = PN_NODE (pn_shift_register_new ());
    PnWire  *w    = pn_wire_new (src, sink);
    PnMessage *m  = pn_message_new (NULL, NULL);

    PN_CHECK_CMPINT (pn_wire_get_source_output (w), ==, 0);

    pn_message_set_double (m, "value", 1.0);
    pn_node_emit_message (src, m);
    PN_CHECK_CMPINT (filled (sink), ==, 1u);

    /* An emission on a port the wire does not hang off is not carried. */
    pn_node_emit_message_on_output (src, m, 1);
    PN_CHECK_CMPINT (filled (sink), ==, 1u);

    g_object_unref (m);
    g_object_unref (w);
    g_object_unref (sink);
    g_object_unref (src);
}

/* A delivery that emits downstream (a nested synchronous emission on the
 * target's own output 0) must leave the source's output index intact for
 * the source's remaining wires on the same port. */
static void
test_nested_emission_restores_output (void)
{
    PnNode  *sr   = make_node (2, NULL);
    PnNode  *next = PN_NODE (pn_shift_register_new ());
    PnNode  *tail = PN_NODE (pn_shift_register_new ());
    PnWire  *a    = pn_wire_new_ports (sr,   1, next, 0);  /* delivers first */
    PnWire  *b    = pn_wire_new_ports (sr,   1, tail, 0);  /* same port      */
    PnWire  *c    = pn_wire_new_ports (next, 0, tail, 0);  /* nested emit    */

    send (sr, 1.0);
    send (sr, 2.0);   /* output 2 carries 1.0 through a, then b */

    PN_CHECK_CMPINT (filled (next), ==, 1u);
    /* tail got next's nested emission (via c) AND sr's output 2 (via b). */
    PN_CHECK_CMPINT (filled (tail), ==, 2u);

    g_object_unref (a);
    g_object_unref (b);
    g_object_unref (c);
    g_object_unref (tail);
    g_object_unref (next);
    g_object_unref (sr);
}

/* ------------------------------------------------------------------ */
/*  Port-count shrink prunes wires                                     */
/* ------------------------------------------------------------------ */

/* Is @wire still in @flow's wire store? */
static gboolean
flow_has_wire (PnFlow *flow, PnWire *wire)
{
    PnWireStore *store = pn_flow_get_wires (flow);
    guint        i;

    for (i = 0; i < pn_wire_store_get_length (store); i++)
        if (pn_wire_store_get_wire (store, i) == wire)
            return TRUE;
    return FALSE;
}

/* Lowering "outputs" drops the wires on the outputs that went away and
 * leaves the rest; raising it again does not resurrect them. */
static void
test_shrink_outputs_prunes_wires (void)
{
    PnFlow  *flow = pn_flow_new ();
    PnNode  *sr   = make_node (4, NULL);
    PnNode  *sink = PN_NODE (pn_shift_register_new ());
    PnWire  *w0   = pn_wire_new_ports (sr, 0, sink, 0);
    PnWire  *w1   = pn_wire_new_ports (sr, 1, sink, 0);
    PnWire  *w3   = pn_wire_new_ports (sr, 3, sink, 0);
    PnWire  *in   = pn_wire_new_ports (sink, 2, sr, 0);  /* sr as target */

    pn_node_store_add (pn_flow_get_nodes (flow), sr);
    pn_node_store_add (pn_flow_get_nodes (flow), sink);
    pn_wire_store_add (pn_flow_get_wires (flow), w0);
    pn_wire_store_add (pn_flow_get_wires (flow), w1);
    pn_wire_store_add (pn_flow_get_wires (flow), w3);
    pn_wire_store_add (pn_flow_get_wires (flow), in);
    PN_CHECK_CMPINT (pn_wire_store_get_length (pn_flow_get_wires (flow)),
                     ==, 4u);

    g_object_set (sr, "outputs", 2, NULL);
    PN_CHECK_CMPINT (pn_wire_store_get_length (pn_flow_get_wires (flow)),
                     ==, 3u);
    PN_CHECK (flow_has_wire (flow, w0));
    PN_CHECK (flow_has_wire (flow, w1));
    PN_CHECK (!flow_has_wire (flow, w3));
    PN_CHECK (flow_has_wire (flow, in));   /* input side untouched */

    g_object_set (sr, "outputs", 4, NULL);
    PN_CHECK_CMPINT (pn_wire_store_get_length (pn_flow_get_wires (flow)),
                     ==, 3u);

    /* The sink losing its third output takes the wire into sr with it. */
    g_object_set (sink, "outputs", 2, NULL);
    PN_CHECK (!flow_has_wire (flow, in));
    PN_CHECK_CMPINT (pn_wire_store_get_length (pn_flow_get_wires (flow)),
                     ==, 2u);

    g_object_unref (w0);
    g_object_unref (w1);
    g_object_unref (w3);
    g_object_unref (in);
    g_object_unref (sink);
    g_object_unref (sr);
    g_object_unref (flow);
}

/* The input side: lowering a multi-input node's count drops the wires
 * feeding the removed inputs. */
static void
test_shrink_inputs_prunes_wires (void)
{
    PnFlow  *flow = pn_flow_new ();
    PnNode  *src  = make_node (2, NULL);
    PnNode  *expr = PN_NODE (pn_expression2_new ());
    PnWire  *a, *b, *c;

    g_object_set (expr, "inputs", 3, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_inputs (expr), ==, 3);
    a = pn_wire_new_ports (src, 0, expr, 0);
    b = pn_wire_new_ports (src, 1, expr, 1);
    c = pn_wire_new_ports (src, 0, expr, 2);

    pn_node_store_add (pn_flow_get_nodes (flow), src);
    pn_node_store_add (pn_flow_get_nodes (flow), expr);
    pn_wire_store_add (pn_flow_get_wires (flow), a);
    pn_wire_store_add (pn_flow_get_wires (flow), b);
    pn_wire_store_add (pn_flow_get_wires (flow), c);

    g_object_set (expr, "inputs", 2, NULL);
    PN_CHECK (flow_has_wire (flow, a));
    PN_CHECK (flow_has_wire (flow, b));
    PN_CHECK (!flow_has_wire (flow, c));

    g_object_unref (a);
    g_object_unref (b);
    g_object_unref (c);
    g_object_unref (expr);
    g_object_unref (src);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-shift-register");
    pn_test_add ("default_ports",          test_default_ports);
    pn_test_add ("plain_node_one_output",  test_plain_node_single_output);
    pn_test_add ("first_message",          test_first_message_output_one_only);
    pn_test_add ("shift_sequence_order",   test_shift_sequence_and_order);
    pn_test_add ("oldest_falls_off",       test_oldest_falls_off);
    pn_test_add ("whole_message_shifted",  test_whole_message_shifted);
    pn_test_add ("stages_isolated",        test_stages_isolated_from_handlers);
    pn_test_add ("resize_keeps_newest",    test_resize_keeps_newest);
    pn_test_add ("clear",                  test_clear);
    pn_test_add ("output_value_readout",   test_output_value_readout);
    pn_test_add ("wire_routes_by_output",  test_wire_routes_by_output);
    pn_test_add ("plain_wire_output_zero", test_plain_wire_is_output_zero);
    pn_test_add ("nested_emit_restores",   test_nested_emission_restores_output);
    pn_test_add ("shrink_outputs_prunes",  test_shrink_outputs_prunes_wires);
    pn_test_add ("shrink_inputs_prunes",   test_shrink_inputs_prunes_wires);
    return pn_test_run ();
}
