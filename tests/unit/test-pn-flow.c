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

/* Unit tests for PnFlow's modified flag.  The flag must track only the
 * parts of a node that round-trip to disk: a runtime self-update of a
 * cosmetic field (icon, body colour) — such as the one a PnMqtt node
 * performs when a delayed CONNACK lands after the file finished loading
 * — must NOT make a freshly-loaded flow report as dirty, while a change
 * to a persisted field (name, position, disabled, or a subclass
 * property) must. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-flow.h"
#include "pn-node.h"
#include "pn-inject.h"

/* Build a flow holding a single node and clear the dirty flag the add
 * itself raised, leaving a clean baseline for the caller to perturb. */
static PnFlow *
make_clean_flow (PnNode **out_node)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *node = PN_NODE (pn_inject_new ());

    pn_flow_add_node (flow, node);
    pn_flow_set_modified (flow, FALSE);

    PN_CHECK_FALSE (pn_flow_is_modified (flow));
    if (out_node != NULL)
        *out_node = node;
    return flow;
}

static void
test_cosmetic_change_stays_clean (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    /* Icon and colour mirror live state (connection status, configured
     * vs. unconfigured) and never persist, so flipping them must leave
     * the flow unmodified. */
    pn_node_set_icon (node, "\xef\x80\x97");
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    GdkRGBA green = { 0.36, 0.66, 0.36, 1.0 };
    pn_node_set_color (node, &green);
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

static void
test_name_change_marks_dirty (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    pn_node_set_name (node, "Renamed");
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

static void
test_position_change_marks_dirty (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    PnPoint p = { 123.0, 456.0 };
    pn_node_set_position (node, &p);
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

static void
test_disabled_change_marks_dirty (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    pn_node_set_disabled (node, TRUE);
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

static void
test_subclass_property_marks_dirty (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    /* "text" is a PnInject property that round-trips to disk. */
    g_object_set (node, "text", "boom", NULL);
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-flow");
    pn_test_add ("cosmetic_stays_clean",  test_cosmetic_change_stays_clean);
    pn_test_add ("name_marks_dirty",      test_name_change_marks_dirty);
    pn_test_add ("position_marks_dirty",  test_position_change_marks_dirty);
    pn_test_add ("disabled_marks_dirty",  test_disabled_change_marks_dirty);
    pn_test_add ("subclass_marks_dirty",  test_subclass_property_marks_dirty);
    return pn_test_run ();
}
