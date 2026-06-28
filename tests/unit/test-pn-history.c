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

/* Unit tests for PnHistory — the document undo/redo stack.  Restores
 * rebuild the flow from JSON, so the old node pointers go stale: every
 * assertion therefore inspects the serialised document (does it still
 * mention a distinguishing value, how many nodes does it hold) rather
 * than reading a node directly.  The coalesced record→idle path needs a
 * main loop, so most tests drive the synchronous commit_now / freeze /
 * thaw entry points; one test pumps the loop to cover the idle path. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"
#include "pn-flow.h"
#include "pn-history.h"
#include "pn-node.h"
#include "pn-node-store.h"
#include "pn-inject.h"

/* TRUE iff the document's JSON currently mentions @needle. */
static gboolean
flow_has (PnFlow *flow, const gchar *needle)
{
    gchar   *json = pn_flow_to_string (flow);
    gboolean hit  = json != NULL && strstr (json, needle) != NULL;
    g_free (json);
    return hit;
}

static guint
flow_node_count (PnFlow *flow)
{
    return pn_node_store_get_length (pn_flow_get_nodes (flow));
}

/* A flow holding one PnInject node, with a clean history baselined on
 * that state and both stacks empty. */
static PnFlow *
make_history_flow (PnHistory **out_hist)
{
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = PN_NODE (pn_inject_new ());
    PnHistory *h;

    pn_flow_add_node (flow, node);
    h = pn_flow_get_history (flow);

    PN_CHECK (h != NULL);

    /* Drop the step the add scheduled and rebaseline here. */
    pn_history_clear (h);
    PN_CHECK_FALSE (pn_history_can_undo (h));
    PN_CHECK_FALSE (pn_history_can_redo (h));

    if (out_hist != NULL)
        *out_hist = h;
    return flow;
}

/* ------------------------------------------------------------------ */

/* A committed property edit can be undone and redone, and the stack
 * availability flips correctly at each end. */
static void
test_commit_undo_redo (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);
    PnNode    *node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);

    PN_CHECK_FALSE (flow_has (flow, "boom"));

    g_object_set (node, "text", "boom", NULL);
    pn_history_commit_now (h);
    PN_CHECK (pn_history_can_undo (h));
    PN_CHECK_FALSE (pn_history_can_redo (h));
    PN_CHECK (flow_has (flow, "boom"));

    pn_history_undo (h);
    PN_CHECK_FALSE (pn_history_can_undo (h));
    PN_CHECK (pn_history_can_redo (h));
    PN_CHECK_FALSE (flow_has (flow, "boom"));

    pn_history_redo (h);
    PN_CHECK (pn_history_can_undo (h));
    PN_CHECK_FALSE (pn_history_can_redo (h));
    PN_CHECK (flow_has (flow, "boom"));

    g_object_unref (flow);
}

/* Committing with nothing changed since the baseline records no step. */
static void
test_noop_commit_records_nothing (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);

    pn_history_commit_now (h);
    PN_CHECK_FALSE (pn_history_can_undo (h));

    g_object_unref (flow);
}

/* A freeze/thaw span collapses several edits into one undo step, and a
 * frozen span that changed nothing records no step at all. */
static void
test_freeze_collapses_to_one_step (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);
    PnNode    *node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);

    pn_history_freeze (h);
    g_object_set (node, "text", "aaa", NULL);
    g_object_set (node, "text", "bbb", NULL);
    g_object_set (node, "text", "ccc", NULL);
    pn_history_thaw (h);

    /* Exactly one step: a single undo returns to the clean baseline. */
    PN_CHECK (pn_history_can_undo (h));
    pn_history_undo (h);
    PN_CHECK_FALSE (pn_history_can_undo (h));
    PN_CHECK_FALSE (flow_has (flow, "ccc"));

    /* An empty frozen span (a drag that went nowhere) records nothing. */
    pn_history_redo (h);            /* back to "ccc" */
    PN_CHECK_FALSE (pn_history_can_redo (h));
    pn_history_freeze (h);
    pn_history_thaw (h);
    PN_CHECK_FALSE (pn_history_can_redo (h));

    g_object_unref (flow);
}

/* A burst of structural mutations followed by a single commit is one
 * undo step — the shape a multi-node delete or paste takes. */
static void
test_burst_is_one_step (void)
{
    PnHistory *h;
    PnFlow    *flow  = make_history_flow (&h);
    guint      start = flow_node_count (flow);

    pn_flow_add_node (flow, PN_NODE (pn_inject_new ()));
    pn_flow_add_node (flow, PN_NODE (pn_inject_new ()));
    pn_history_commit_now (h);
    PN_CHECK_CMPINT (flow_node_count (flow), ==, start + 2);
    PN_CHECK (pn_history_can_undo (h));

    pn_history_undo (h);
    PN_CHECK_CMPINT (flow_node_count (flow), ==, start);
    PN_CHECK_FALSE (pn_history_can_undo (h));

    g_object_unref (flow);
}

/* A fresh edit invalidates the redo stack. */
static void
test_new_edit_clears_redo (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);
    PnNode    *node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);

    g_object_set (node, "text", "one", NULL);
    pn_history_commit_now (h);
    pn_history_undo (h);
    PN_CHECK (pn_history_can_redo (h));

    /* The node pointer is stale after the undo rebuilt the flow. */
    node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);
    g_object_set (node, "text", "two", NULL);
    pn_history_commit_now (h);
    PN_CHECK_FALSE (pn_history_can_redo (h));

    g_object_unref (flow);
}

/* The record() → idle commit path coalesces a burst into one step once
 * the main loop runs. */
static void
test_idle_coalesces_one_step (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);
    PnNode    *node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);

    /* Each set funnels through pn_history_record; the second finds an
     * idle already pending, so only one commit should land. */
    g_object_set (node, "text", "p", NULL);
    g_object_set (node, "text", "q", NULL);

    while (g_main_context_iteration (NULL, FALSE))
        ;

    PN_CHECK (pn_history_can_undo (h));
    pn_history_undo (h);
    PN_CHECK_FALSE (pn_history_can_undo (h));   /* it was a single step */
    PN_CHECK_FALSE (flow_has (flow, "\"q\""));

    g_object_unref (flow);
}

/* The modified flag follows the saved point across undo and redo. */
static void
test_modified_tracks_saved_point (void)
{
    PnHistory *h;
    PnFlow    *flow = make_history_flow (&h);
    PnNode    *node = pn_node_store_get_node (pn_flow_get_nodes (flow), 0);

    /* Establish the current state as "on disk". */
    pn_history_mark_saved (h);
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    g_object_set (node, "text", "edit1", NULL);
    pn_history_commit_now (h);
    PN_CHECK (pn_flow_is_modified (flow));

    /* Undoing back to the saved state clears the dirty flag... */
    pn_history_undo (h);
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    /* ...and redoing away from it sets it again. */
    pn_history_redo (h);
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-history");
    pn_test_add ("commit_undo_redo",      test_commit_undo_redo);
    pn_test_add ("noop_commit_nothing",   test_noop_commit_records_nothing);
    pn_test_add ("freeze_one_step",       test_freeze_collapses_to_one_step);
    pn_test_add ("burst_one_step",        test_burst_is_one_step);
    pn_test_add ("new_edit_clears_redo",  test_new_edit_clears_redo);
    pn_test_add ("idle_coalesces",        test_idle_coalesces_one_step);
    pn_test_add ("modified_saved_point",  test_modified_tracks_saved_point);
    return pn_test_run ();
}
