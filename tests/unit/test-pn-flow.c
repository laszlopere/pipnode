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
#include "pn-subst.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>

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

    PnColor green = { 0.36, 0.66, 0.36, 1.0 };
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

/* ------------------------------------------------------------------ */
/*  Document globals                                                   */
/* ------------------------------------------------------------------ */

/* Define a typed document global on @flow in one expression. */
#define SET_GLOBAL(flow, name, gtype, setter, val)        \
    G_STMT_START {                                         \
        GValue v_ = G_VALUE_INIT;                         \
        g_value_init (&v_, (gtype));                       \
        setter (&v_, (val));                               \
        pn_flow_set_global ((flow), (name), &v_);          \
        g_value_unset (&v_);                              \
    } G_STMT_END

/* Each of the four supported scalar types round-trips through set/get
 * preserving both its GType and its value. */
static void
test_global_typed_roundtrip (void)
{
    PnFlow *flow = pn_flow_new ();
    GValue  v    = G_VALUE_INIT;

    SET_GLOBAL (flow, "s", G_TYPE_STRING,  g_value_set_string,  "prod");
    SET_GLOBAL (flow, "i", G_TYPE_INT64,   g_value_set_int64,   42);
    SET_GLOBAL (flow, "d", G_TYPE_DOUBLE,  g_value_set_double,  1.5);
    SET_GLOBAL (flow, "b", G_TYPE_BOOLEAN, g_value_set_boolean, TRUE);

    PN_CHECK (pn_flow_get_global (flow, "s", &v));
    PN_CHECK (G_VALUE_HOLDS_STRING (&v));
    PN_CHECK_CMPSTR (g_value_get_string (&v), ==, "prod");
    g_value_unset (&v);

    PN_CHECK (pn_flow_get_global (flow, "i", &v));
    PN_CHECK (G_VALUE_HOLDS_INT64 (&v));
    PN_CHECK_CMPINT (g_value_get_int64 (&v), ==, 42);
    g_value_unset (&v);

    PN_CHECK (pn_flow_get_global (flow, "d", &v));
    PN_CHECK (G_VALUE_HOLDS_DOUBLE (&v));
    PN_CHECK_NEAR (g_value_get_double (&v), 1.5, 1e-9);
    g_value_unset (&v);

    PN_CHECK (pn_flow_get_global (flow, "b", &v));
    PN_CHECK (G_VALUE_HOLDS_BOOLEAN (&v));
    PN_CHECK (g_value_get_boolean (&v));
    g_value_unset (&v);

    g_object_unref (flow);
}

/* Getting an unknown name fails and leaves the out-value untouched;
 * removing a global makes a later get fail too. */
static void
test_global_missing_and_remove (void)
{
    PnFlow *flow = pn_flow_new ();
    GValue  v    = G_VALUE_INIT;

    PN_CHECK_FALSE (pn_flow_get_global (flow, "nope", &v));
    PN_CHECK (G_VALUE_TYPE (&v) == G_TYPE_INVALID);   /* untouched */

    SET_GLOBAL (flow, "env", G_TYPE_STRING, g_value_set_string, "prod");
    PN_CHECK (pn_flow_get_global (flow, "env", &v));
    g_value_unset (&v);

    pn_flow_remove_global (flow, "env");
    PN_CHECK_FALSE (pn_flow_get_global (flow, "env", &v));

    g_object_unref (flow);
}

/* The name list is returned in ascending order. */
static void
test_global_list_sorted (void)
{
    PnFlow *flow = pn_flow_new ();
    GList  *names;

    SET_GLOBAL (flow, "zeta",  G_TYPE_INT64, g_value_set_int64, 1);
    SET_GLOBAL (flow, "alpha", G_TYPE_INT64, g_value_set_int64, 2);
    SET_GLOBAL (flow, "mid",   G_TYPE_INT64, g_value_set_int64, 3);

    names = pn_flow_list_globals (flow);
    PN_CHECK_CMPINT (g_list_length (names), ==, 3);
    PN_CHECK_CMPSTR (g_list_nth_data (names, 0), ==, "alpha");
    PN_CHECK_CMPSTR (g_list_nth_data (names, 1), ==, "mid");
    PN_CHECK_CMPSTR (g_list_nth_data (names, 2), ==, "zeta");
    g_list_free_full (names, g_free);

    g_object_unref (flow);
}

/* Defining a global marks the flow modified so the change persists on
 * the next save. */
static void
test_global_set_marks_dirty (void)
{
    PnFlow *flow = make_clean_flow (NULL);

    SET_GLOBAL (flow, "env", G_TYPE_STRING, g_value_set_string, "prod");
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

/* The globals resolver feeds ${name} placeholders directly: a defined
 * global renders by type, an undefined one misses and (verbatim policy)
 * leaves the literal in place. */
static void
test_globals_as_subst_source (void)
{
    PnFlow          *flow = pn_flow_new ();
    PnSubstResolver  globals;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;
    gchar           *out;

    SET_GLOBAL (flow, "env",   G_TYPE_STRING, g_value_set_string, "prod");
    SET_GLOBAL (flow, "build", G_TYPE_INT64,  g_value_set_int64,  42);

    pn_flow_subst_resolver_globals (&globals, flow);
    chain[0] = &globals;
    chain[1] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_TEXT;
    ctx.miss      = PN_SUBST_MISS_VERBATIM;

    /* Existing globals resolve; an unknown name stays verbatim. */
    out = pn_subst_expand ("${env}-${build} ${unknown}", &ctx);
    PN_CHECK_CMPSTR (out, ==, "prod-42 ${unknown}");
    g_free (out);

    g_object_unref (flow);
}

/* A NULL flow yields a resolver that always misses — the documented
 * behaviour a node relies on before it is added to any flow. */
static void
test_globals_null_flow_misses (void)
{
    PnSubstResolver  globals;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;
    gchar           *out;

    pn_flow_subst_resolver_globals (&globals, NULL);
    chain[0] = &globals;
    chain[1] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_TEXT;
    ctx.miss      = PN_SUBST_MISS_EMPTY;

    out = pn_subst_expand ("[${env}]", &ctx);
    PN_CHECK_CMPSTR (out, ==, "[]");
    g_free (out);
}

/* A node's topic template resolves through the same node→globals chain:
 * the built-in ${nodeclass}/${nodename} come from the node itself, while
 * an unmatched ${name} falls through to a document global.  Observed via
 * pn_message_new(source, NULL), which stamps the resolved topic, and via
 * a leftover ${unknown} that no source supplies (left verbatim). */
static void
test_topic_template_node_and_globals (void)
{
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = PN_NODE (pn_inject_new ());
    PnMessage *msg;

    pn_node_set_name  (node, "probe");
    pn_node_set_topic (node, "${nodeclass}/${nodename}/${env}/${unknown}");
    SET_GLOBAL (flow, "env", G_TYPE_STRING, g_value_set_string, "prod");
    pn_flow_add_node (flow, node);   /* wires the node's back-pointer */

    msg = pn_message_new (node, NULL);   /* NULL topic → resolve template */
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==,
                     "Injector/probe/prod/${unknown}");

    g_object_unref (msg);
    g_object_unref (flow);
}

/* ------------------------------------------------------------------ */
/*  Panel-applet layout                                                */
/* ------------------------------------------------------------------ */

/* set/get preserves the placement; an unplaced node misses. */
static void
test_panel_position_roundtrip (void)
{
    PnFlow  *flow = pn_flow_new ();
    gdouble  x = -1, y = -1;

    PN_CHECK_FALSE (pn_flow_get_panel_position (flow, "abc", NULL, NULL));

    pn_flow_set_panel_position (flow, "abc", 120.0, 240.0);
    PN_CHECK (pn_flow_get_panel_position (flow, "abc", &x, &y));
    PN_CHECK_NEAR (x, 120.0, 1e-9);
    PN_CHECK_NEAR (y, 240.0, 1e-9);

    g_object_unref (flow);
}

/* The first placement dirties the flow; re-setting the same coordinates
 * does not, so a click that does not move a widget keeps the file clean. */
static void
test_panel_position_marks_dirty (void)
{
    PnNode *node;
    PnFlow *flow = make_clean_flow (&node);

    pn_flow_set_panel_position (flow, "abc", 10.0, 20.0);
    PN_CHECK (pn_flow_is_modified (flow));

    pn_flow_set_modified (flow, FALSE);
    pn_flow_set_panel_position (flow, "abc", 10.0, 20.0);   /* unchanged */
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    pn_flow_set_panel_position (flow, "abc", 11.0, 20.0);   /* moved */
    PN_CHECK (pn_flow_is_modified (flow));

    g_object_unref (flow);
}

/* Placements survive a save/load to disk, keyed by node UUID; a placement
 * whose node is gone is pruned on save rather than written out. */
static void
test_panel_position_disk_roundtrip (void)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *node = PN_NODE (pn_inject_new ());
    gchar  *uuid;
    gchar  *path = NULL;
    gint    fd;
    gdouble x = -1, y = -1;

    pn_flow_add_node (flow, node);
    uuid = g_strdup (pn_node_get_uuid (node));

    /* Layout is only persisted while the editor is open. */
    pn_flow_set_panel_editor_open (flow, TRUE);
    pn_flow_set_panel_position (flow, uuid, 150.0, 275.0);
    /* An orphan placement (no such node) must not be serialised. */
    pn_flow_set_panel_position (flow, "no-such-node", 7.0, 9.0);

    fd = g_file_open_tmp ("pn-flow-panel-XXXXXX.json", &path, NULL);
    PN_CHECK (fd >= 0);
    g_close (fd, NULL);
    PN_CHECK (pn_flow_save_to_file (flow, path, NULL));
    g_object_unref (flow);

    flow = pn_flow_new ();
    PN_CHECK (pn_flow_load_from_file (flow, path, NULL));
    PN_CHECK_FALSE (pn_flow_is_modified (flow));   /* load alone is clean */

    PN_CHECK (pn_flow_get_panel_editor_open (flow));   /* reopens on load */
    PN_CHECK (pn_flow_get_panel_position (flow, uuid, &x, &y));
    PN_CHECK_NEAR (x, 150.0, 1e-9);
    PN_CHECK_NEAR (y, 275.0, 1e-9);
    PN_CHECK_FALSE (pn_flow_get_panel_position (flow, "no-such-node",
                                                NULL, NULL));

    g_remove (path);
    g_free (path);
    g_free (uuid);
    g_object_unref (flow);
}

/* Opening the editor marks the flow dirty; re-opening an already-open
 * editor does not.  Closing it marks dirty too, but the layout outlives
 * the editor tab — it is document data that drives the real panel. */
static void
test_panel_editor_open_flag (void)
{
    PnFlow *flow = make_clean_flow (NULL);

    pn_flow_set_panel_editor_open (flow, TRUE);
    PN_CHECK (pn_flow_get_panel_editor_open (flow));
    PN_CHECK (pn_flow_is_modified (flow));

    pn_flow_set_modified (flow, FALSE);
    pn_flow_set_panel_editor_open (flow, TRUE);          /* already open */
    PN_CHECK_FALSE (pn_flow_is_modified (flow));

    pn_flow_set_panel_position (flow, "abc", 10.0, 20.0);
    PN_CHECK (pn_flow_get_panel_position (flow, "abc", NULL, NULL));

    pn_flow_set_panel_editor_open (flow, FALSE);
    PN_CHECK_FALSE (pn_flow_get_panel_editor_open (flow));
    PN_CHECK (pn_flow_is_modified (flow));
    /* Closing the editor keeps the layout. */
    PN_CHECK (pn_flow_get_panel_position (flow, "abc", NULL, NULL));

    g_object_unref (flow);
}

/* A document saved with the editor closed drops the open flag but keeps
 * the panel layout: the placements drive the real panel applet whether or
 * not the editor tab happens to be open. */
static void
test_panel_layout_persists_when_closed (void)
{
    PnFlow *flow = pn_flow_new ();
    PnNode *node = PN_NODE (pn_inject_new ());
    gchar  *uuid;
    gchar  *path = NULL;
    gdouble x = 0.0, y = 0.0;
    gint    fd;

    pn_flow_add_node (flow, node);
    uuid = g_strdup (pn_node_get_uuid (node));

    /* Arrange a layout, then close the editor before saving. */
    pn_flow_set_panel_editor_open (flow, TRUE);
    pn_flow_set_panel_position (flow, uuid, 30.0, 40.0);
    pn_flow_set_panel_editor_open (flow, FALSE);

    fd = g_file_open_tmp ("pn-flow-panel-XXXXXX.json", &path, NULL);
    PN_CHECK (fd >= 0);
    g_close (fd, NULL);
    PN_CHECK (pn_flow_save_to_file (flow, path, NULL));
    g_object_unref (flow);

    flow = pn_flow_new ();
    PN_CHECK (pn_flow_load_from_file (flow, path, NULL));
    PN_CHECK_FALSE (pn_flow_get_panel_editor_open (flow));
    PN_CHECK (pn_flow_get_panel_position (flow, uuid, &x, &y));
    PN_CHECK (x == 30.0);
    PN_CHECK (y == 40.0);

    g_remove (path);
    g_free (path);
    g_free (uuid);
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
    pn_test_add ("global_roundtrip",      test_global_typed_roundtrip);
    pn_test_add ("global_miss_remove",    test_global_missing_and_remove);
    pn_test_add ("global_list_sorted",    test_global_list_sorted);
    pn_test_add ("global_marks_dirty",    test_global_set_marks_dirty);
    pn_test_add ("globals_subst_source",  test_globals_as_subst_source);
    pn_test_add ("globals_null_flow",     test_globals_null_flow_misses);
    pn_test_add ("topic_node_globals",    test_topic_template_node_and_globals);
    pn_test_add ("panel_pos_roundtrip",   test_panel_position_roundtrip);
    pn_test_add ("panel_pos_marks_dirty", test_panel_position_marks_dirty);
    pn_test_add ("panel_pos_disk",        test_panel_position_disk_roundtrip);
    pn_test_add ("panel_editor_open_flag", test_panel_editor_open_flag);
    pn_test_add ("panel_layout_persists", test_panel_layout_persists_when_closed);
    return pn_test_run ();
}
