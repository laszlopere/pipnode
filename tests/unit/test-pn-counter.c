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

/* Unit tests for PnCounter: a self-advancing value register (the CPU
 * program counter).  Inputs — 0 tick (emit then advance), 1 load
 * (silent JUMP target), 2 reset (silent).  A load landing during a
 * tick's synchronous emit wins over that cycle's advance.  Headless:
 * one node, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-counter.h"

#define IN_TICK  0
#define IN_LOAD  1
#define IN_RESET 2

typedef struct
{
    guint    count;
    gdouble  seen[32];

    /* Optional re-entrant JUMP: while the tick that emits index
     * @load_after is being delivered, inject a load of @load_val (once). */
    PnNode  *node;
    gboolean do_load;
    gint     load_after;
    gdouble  load_val;
} Recorder;

static void feed (PnNode *node, gint input, gdouble value);

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    guint     i = r->count;

    (void) node;
    if (i < G_N_ELEMENTS (r->seen))
        r->seen[i] = pn_test_num (message, "value");
    r->count++;

    if (r->do_load && (gint) i == r->load_after)
    {
        r->do_load = FALSE;   /* once, and before feeding to be safe */
        feed (r->node, IN_LOAD, r->load_val);
    }
}

static PnNode *
make_node (gdouble initial, gdouble step, gdouble modulo, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_counter_new ());
    g_object_set (node, "initial", initial, "step", step,
                  "modulo", modulo, NULL);
    rec->count      = 0;
    rec->node       = node;
    rec->do_load    = FALSE;
    rec->load_after = -1;
    rec->load_val   = 0.0;
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

/* Each tick emits the current value then advances by `step`. */
static void
test_ticks_advance (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, 1.0, 0.0, &rec);

    feed (node, IN_TICK, 0.0);
    feed (node, IN_TICK, 0.0);
    feed (node, IN_TICK, 0.0);

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 2.0, 1e-9);

    g_object_unref (node);
}

/* A custom step and non-zero initial. */
static void
test_step_and_initial (void)
{
    Recorder rec;
    PnNode  *node = make_node (10.0, 4.0, 0.0, &rec);

    feed (node, IN_TICK, 0.0);
    feed (node, IN_TICK, 0.0);
    feed (node, IN_TICK, 0.0);

    PN_CHECK_NEAR (rec.seen[0], 10.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[1], 14.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[2], 18.0, 1e-9);

    g_object_unref (node);
}

/* A load arriving OUTSIDE a tick applies immediately; the next tick
 * emits it, then advances from there. */
static void
test_standalone_load (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, 1.0, 0.0, &rec);

    feed (node, IN_TICK, 0.0);   /* emits 0 -> now 1 */
    feed (node, IN_LOAD, 5.0);   /* silent, stored := 5 */
    PN_CHECK_CMPINT (rec.count, ==, 1);

    feed (node, IN_TICK, 0.0);   /* emits 5 -> now 6 */
    feed (node, IN_TICK, 0.0);   /* emits 6 */
    PN_CHECK_NEAR (rec.seen[1], 5.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[2], 6.0, 1e-9);

    g_object_unref (node);
}

/* A load injected DURING a tick's downstream delivery (a JUMP decoded
 * from the fetched instruction) overrides that cycle's implicit +step,
 * so the target is what the next tick emits. */
static void
test_jump_during_tick (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, 1.0, 0.0, &rec);

    rec.do_load    = TRUE;
    rec.load_after = 0;      /* on the very first emitted value (0) */
    rec.load_val   = 10.0;

    feed (node, IN_TICK, 0.0);   /* emits 0; JUMP 10 injected; stored := 10 */
    feed (node, IN_TICK, 0.0);   /* emits 10 -> now 11 */
    feed (node, IN_TICK, 0.0);   /* emits 11 */

    PN_CHECK_CMPINT (rec.count, ==, 3);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[1], 10.0, 1e-9);
    PN_CHECK_NEAR   (rec.seen[2], 11.0, 1e-9);

    g_object_unref (node);
}

/* Reset (silent) returns the counter to its initial value. */
static void
test_reset (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, 1.0, 0.0, &rec);

    feed (node, IN_TICK, 0.0);
    feed (node, IN_TICK, 0.0);
    feed (node, IN_RESET, 0.0);   /* silent, stored := 0 */
    PN_CHECK_CMPINT (rec.count, ==, 2);

    feed (node, IN_TICK, 0.0);
    PN_CHECK_NEAR (rec.seen[2], 0.0, 1e-9);

    g_object_unref (node);
}

/* With a modulo the advanced value wraps, so the counter cycles. */
static void
test_modulo_wraps (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, 1.0, 3.0, &rec);
    guint    i;

    for (i = 0; i < 6; i++)
        feed (node, IN_TICK, 0.0);

    PN_CHECK_CMPINT (rec.count, ==, 6);
    PN_CHECK_NEAR (rec.seen[0], 0.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[1], 1.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[2], 2.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[3], 0.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[4], 1.0, 1e-9);
    PN_CHECK_NEAR (rec.seen[5], 2.0, 1e-9);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-counter");
    pn_test_add ("ticks_advance",     test_ticks_advance);
    pn_test_add ("step_and_initial",  test_step_and_initial);
    pn_test_add ("standalone_load",   test_standalone_load);
    pn_test_add ("jump_during_tick",  test_jump_during_tick);
    pn_test_add ("reset",             test_reset);
    pn_test_add ("modulo_wraps",      test_modulo_wraps);
    return pn_test_run ();
}
