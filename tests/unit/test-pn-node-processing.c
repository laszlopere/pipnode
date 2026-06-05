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

/* Unit tests for the base-class processing-activity indicator (TODO #42):
 * pn_node_processing_begin/end maintain a thread-safe refcount, the node
 * reads as busy while it is above zero, a minimum-visible linger keeps a
 * brief blip perceptible, and the "processing-changed" signal fires once
 * on each idle⇄busy edge (marshalled through the default main context).
 *
 * Timing is asserted against an explicit @now passed to
 * pn_node_is_processing_visible — the tests sample the monotonic clock once
 * up front and reason relative to it, so they never sleep and stay robust
 * to scheduler jitter (every margin is hundreds of ms away from the
 * 100 ms linger floor). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"

/* The minimum-visible linger baked into pn-node.c, in microseconds. */
#define MIN_VISIBLE_US (250 * 1000)

/* ---- a minimal concrete PnNode whose receive() we can observe ----
 *
 * The base PnNode has no receive vfunc, so to test that the dispatch
 * wrapper brackets the handler with begin/end (TODO #42.2) we need a
 * subclass that actually has one.  Its receive asserts the node already
 * reads as busy while it runs — i.e. begin() fired before the call. */

#define TEST_TYPE_PROC (test_proc_get_type ())
G_DECLARE_FINAL_TYPE (TestProc, test_proc, TEST, PROC, PnNode)

struct _TestProc {
    PnNode   parent_instance;
    gboolean saw_busy;   /* set from receive: was the node busy mid-handler? */
    guint    n_received;
};

G_DEFINE_TYPE (TestProc, test_proc, PN_TYPE_NODE)

static void
test_proc_receive (PnNode *node, PnMessage *message)
{
    TestProc *self = (TestProc *) node;
    (void) message;
    self->n_received++;
    self->saw_busy = pn_node_is_processing (node);
}

static void
test_proc_class_init (TestProcClass *klass)
{
    PN_NODE_CLASS (klass)->receive = test_proc_receive;
}

static void
test_proc_init (TestProc *self)
{
    self->saw_busy   = FALSE;
    self->n_received = 0;
}

static void
send_empty (PnNode *node)
{
    PnMessage *msg = pn_message_new (NULL, NULL);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static PnNode *
make_node (void)
{
    return g_object_new (PN_TYPE_NODE, NULL);
}

static void
test_fresh_node_is_idle (void)
{
    PnNode *node = make_node ();

    PN_CHECK_FALSE (pn_node_is_processing (node));
    PN_CHECK_FALSE (pn_node_is_processing_visible (node,
                                                   g_get_monotonic_time ()));

    g_object_unref (node);
}

static void
test_begin_lights_node (void)
{
    PnNode *node = make_node ();

    pn_node_processing_begin (node);
    PN_CHECK (pn_node_is_processing (node));
    PN_CHECK (pn_node_is_processing_visible (node, g_get_monotonic_time ()));

    pn_node_processing_end (node);
    PN_CHECK_FALSE (pn_node_is_processing (node));

    g_object_unref (node);
}

static void
test_refcount_composition (void)
{
    PnNode *node = make_node ();

    /* Two overlapping work items — the node stays busy until both end. */
    pn_node_processing_begin (node);
    pn_node_processing_begin (node);
    PN_CHECK (pn_node_is_processing (node));

    pn_node_processing_end (node);
    PN_CHECK (pn_node_is_processing (node));   /* one still outstanding */

    pn_node_processing_end (node);
    PN_CHECK_FALSE (pn_node_is_processing (node));

    g_object_unref (node);
}

static void
test_min_visible_linger (void)
{
    PnNode *node = make_node ();
    gint64  t0   = g_get_monotonic_time ();

    /* A sub-millisecond blip: begin immediately followed by end. */
    pn_node_processing_begin (node);
    pn_node_processing_end (node);

    /* The refcount is back to zero ... */
    PN_CHECK_FALSE (pn_node_is_processing (node));

    /* ... but the node still reads as *visibly* busy through the linger.
     * Work began at or after t0, so visible_until >= t0 + 100 ms; anything
     * before that floor is still lit. */
    PN_CHECK (pn_node_is_processing_visible (node, t0));
    PN_CHECK (pn_node_is_processing_visible (node, t0 + MIN_VISIBLE_US / 2));

    /* Well past the floor the glow is gone.  The begin→end overhead is a
     * few µs, so visible_until is comfortably below t0 + 500 ms. */
    PN_CHECK_FALSE (pn_node_is_processing_visible (node, t0 + 5 * MIN_VISIBLE_US));

    g_object_unref (node);
}

static void
test_long_work_outlasts_linger (void)
{
    PnNode *node = make_node ();
    gint64  t0   = g_get_monotonic_time ();

    /* While work is outstanding the node is busy regardless of the clock —
     * the linger floor never shortens a genuinely long task. */
    pn_node_processing_begin (node);
    PN_CHECK (pn_node_is_processing_visible (node, t0 + 100 * MIN_VISIBLE_US));
    pn_node_processing_end (node);

    g_object_unref (node);
}

/* ---- "processing-changed" signal edges ---- */

typedef struct {
    guint n_true;
    guint n_false;
} EdgeCounts;

static void
on_processing_changed (PnNode *node, gboolean busy, gpointer data)
{
    EdgeCounts *c = data;
    (void) node;
    if (busy)
        c->n_true++;
    else
        c->n_false++;
}

/* Drain the marshalled emissions: begin/end post the signal to the default
 * main context rather than firing inline, so the test must pump the loop. */
static void
pump (void)
{
    while (g_main_context_iteration (NULL, FALSE))
        ;
}

static void
test_signal_fires_once_per_edge (void)
{
    PnNode    *node = make_node ();
    EdgeCounts c    = { 0, 0 };

    g_signal_connect (node, "processing-changed",
                      G_CALLBACK (on_processing_changed), &c);

    pn_node_processing_begin (node);   /* idle→busy: one TRUE  */
    pump ();
    PN_CHECK_CMPINT (c.n_true,  ==, 1);
    PN_CHECK_CMPINT (c.n_false, ==, 0);

    pn_node_processing_begin (node);   /* nested: no edge, no emit */
    pump ();
    PN_CHECK_CMPINT (c.n_true,  ==, 1);

    pn_node_processing_end (node);     /* still busy: no edge, no emit */
    pump ();
    PN_CHECK_CMPINT (c.n_false, ==, 0);

    pn_node_processing_end (node);     /* busy→idle: one FALSE */
    pump ();
    PN_CHECK_CMPINT (c.n_true,  ==, 1);
    PN_CHECK_CMPINT (c.n_false, ==, 1);

    g_object_unref (node);
}

/* ---- unbalanced end is a safe, warned no-op ---- */

static guint unbalanced_warnings;

static void
count_warning (const gchar    *domain,
               GLogLevelFlags  level,
               const gchar    *message,
               gpointer        user_data)
{
    (void) domain; (void) level; (void) message; (void) user_data;
    unbalanced_warnings++;
}

static void
test_unbalanced_end_is_safe (void)
{
    PnNode *node = make_node ();
    guint   id;

    unbalanced_warnings = 0;
    id = g_log_set_handler (NULL,
                            G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL |
                            G_LOG_FLAG_RECURSION,
                            count_warning, NULL);

    /* end() with no matching begin() warns and otherwise does nothing —
     * the count must not underflow into a stuck-busy state. */
    pn_node_processing_end (node);

    g_log_remove_handler (NULL, id);

    PN_CHECK_CMPINT (unbalanced_warnings, ==, 1);
    PN_CHECK_FALSE (pn_node_is_processing (node));

    g_object_unref (node);
}

/* ---- the receive() dispatch wrap (TODO #42.2) ---- */

static void
test_receive_wrap_brackets_handler (void)
{
    TestProc *node = g_object_new (TEST_TYPE_PROC, NULL);
    gint64    t0   = g_get_monotonic_time ();

    send_empty (PN_NODE (node));

    /* The handler ran ... */
    PN_CHECK_CMPINT (node->n_received, ==, 1);
    /* ... and saw itself as busy: begin() fired before receive. */
    PN_CHECK (node->saw_busy);
    /* The bracket balanced, so the count is back to zero, but the linger
     * keeps the node visibly busy after the synchronous return. */
    PN_CHECK_FALSE (pn_node_is_processing (PN_NODE (node)));
    PN_CHECK (pn_node_is_processing_visible (PN_NODE (node), t0));

    g_object_unref (node);
}

static void
test_disabled_node_does_not_light (void)
{
    TestProc *node = g_object_new (TEST_TYPE_PROC, NULL);

    /* A disabled node never enters receive (the dispatch returns early),
     * so the wrap never fires and the node stays dark. */
    pn_node_set_disabled (PN_NODE (node), TRUE);
    send_empty (PN_NODE (node));

    PN_CHECK_CMPINT (node->n_received, ==, 0);
    PN_CHECK_FALSE (pn_node_is_processing_visible (PN_NODE (node),
                                                   g_get_monotonic_time ()));

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-node-processing");
    pn_test_add ("fresh_node_idle",       test_fresh_node_is_idle);
    pn_test_add ("begin_lights_node",     test_begin_lights_node);
    pn_test_add ("refcount_composition",  test_refcount_composition);
    pn_test_add ("min_visible_linger",    test_min_visible_linger);
    pn_test_add ("long_work_outlasts",    test_long_work_outlasts_linger);
    pn_test_add ("signal_edges",          test_signal_fires_once_per_edge);
    pn_test_add ("unbalanced_end_safe",   test_unbalanced_end_is_safe);
    pn_test_add ("receive_wrap",          test_receive_wrap_brackets_handler);
    pn_test_add ("disabled_dark",         test_disabled_node_does_not_light);
    return pn_test_run ();
}
