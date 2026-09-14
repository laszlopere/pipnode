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

/* Unit tests for PnValueRouter: one input, N outputs, one numeric rule
 * per output, each message leaving by the first output whose rule
 * matches.  Emission is synchronous in receive(), so no main loop is
 * needed.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-value-router.h"

#include <string.h>

typedef struct
{
    guint count;
    gint  output[32];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    (void) message;
    if (r->count < G_N_ELEMENTS (r->output))
        r->output[r->count] = pn_node_current_output ();
    r->count++;
}

static PnNode *
make_node (gint outputs, const gchar *rules, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_value_router_new ());

    g_object_set (node, "outputs", outputs, "rules", rules, NULL);
    if (rec != NULL)
    {
        memset (rec, 0, sizeof *rec);
        g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    }
    return node;
}

/* Deliver a message whose data.@member is @value, and return the output
 * it left by, or -1 when it was dropped. */
static gint
route (PnNode *node, Recorder *rec, const gchar *member, gdouble value)
{
    PnMessage *m      = pn_message_new (NULL, NULL);
    guint      before = rec->count;

    pn_message_set_double (m, member, value);
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK (rec->count <= before + 1);
    return rec->count > before ? rec->output[rec->count - 1] : -1;
}

static PnValueRouter *
vr (PnNode *node)
{
    return PN_VALUE_ROUTER (node);
}

/* ------------------------------------------------------------------ */

static void
test_defaults (void)
{
    PnNode *node = PN_NODE (pn_value_router_new ());
    gchar  *path, *rules;
    gint    outputs;

    g_object_get (node, "outputs", &outputs, "path", &path,
                  "rules", &rules, NULL);
    PN_CHECK_CMPINT (outputs, ==, PN_VALUE_ROUTER_DEF_OUTPUTS);
    PN_CHECK_CMPSTR (path, ==, "value");
    PN_CHECK_CMPSTR (rules, ==, "[]");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "out1");

    /* No rules: everything is dropped. */
    PN_CHECK_CMPINT (pn_value_router_route_number (vr (node), TRUE, 1.0), ==, -1);

    g_free (path);
    g_free (rules);
    g_object_unref (node);
}

/* Every comparison operator, and inclusive range ends. */
static void
test_operators (void)
{
    PnNode *node = make_node (2, "[]", NULL);
    PnValueRouter *r = vr (node);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_LT, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 4.9), ==, 0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, -1);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_LE, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, 0);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_EQ, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, 0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.1), ==, -1);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_NE, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, -1);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 6.0), ==, 0);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_GE, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, 0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 4.0), ==, -1);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_GT, 5.0, 0.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.0), ==, -1);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 5.5), ==, 0);

    pn_value_router_set_rule (r, 0, PN_VALUE_ROUTER_OP_RANGE, 10.0, 20.0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 10.0), ==, 0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 20.0), ==, 0);
    PN_CHECK_CMPINT (pn_value_router_route_number (r, TRUE, 20.5), ==, -1);

    g_object_unref (node);
}

/* First match wins; "else" catches the rest, including messages with no
 * number; outputs beyond the count do not route. */
static void
test_first_match_and_else (void)
{
    Recorder rec;
    PnNode  *node = make_node (3,
            "[{\"op\":\"<\",\"value\":10},"
             "{\"op\":\"<\",\"value\":100},"
             "{\"op\":\"else\"},"
             "{\"op\":\"==\",\"value\":500}]", &rec);
    PnMessage *m;

    PN_CHECK_CMPINT (route (node, &rec, "value", 5.0),   ==, 0);
    PN_CHECK_CMPINT (route (node, &rec, "value", 50.0),  ==, 1);
    PN_CHECK_CMPINT (route (node, &rec, "value", 500.0), ==, 2);

    /* Missing member -> only "else". */
    PN_CHECK_CMPINT (route (node, &rec, "other", 5.0), ==, 2);

    /* A string is not a number. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "5");
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.output[rec.count - 1], ==, 2);

    /* Without the else, a no-match is dropped. */
    pn_value_router_set_rule (vr (node), 2, PN_VALUE_ROUTER_OP_UNUSED, 0, 0);
    PN_CHECK_CMPINT (route (node, &rec, "value", 500.0), ==, -1);

    g_object_unref (node);
}

/* "path" picks the member; dotted paths reach nested objects; booleans
 * read as 1 / 0. */
static void
test_path (void)
{
    Recorder   rec;
    PnNode    *node = make_node (2,
            "[{\"op\":\"==\",\"value\":1},{\"op\":\"==\",\"value\":0}]", &rec);
    PnMessage *m;
    JsonNode  *nested;

    g_object_set (node, "path", "op", NULL);
    PN_CHECK_CMPINT (route (node, &rec, "op", 1.0),    ==, 0);
    PN_CHECK_CMPINT (route (node, &rec, "value", 1.0), ==, -1);

    g_object_set (node, "path", "  ", NULL);
    {
        gchar *path;
        g_object_get (node, "path", &path, NULL);
        PN_CHECK_CMPSTR (path, ==, "value");
        g_free (path);
    }

    g_object_set (node, "path", "status.on", NULL);
    m = pn_message_new (NULL, NULL);
    nested = json_from_string ("{\"on\": false}", NULL);
    pn_message_set_member (m, "status", nested);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.count, ==, 2u);
    PN_CHECK_CMPINT (rec.output[1], ==, 1);

    g_object_unref (node);
}

/* The message goes out as the same object, untouched. */
static void
test_forwards_unchanged (void)
{
    PnNode    *node = make_node (2, "[{\"op\":\"else\"}]", NULL);
    PnMessage *m    = pn_message_new (NULL, NULL);
    guint      emits = 0;

    g_signal_connect (node, "message", G_CALLBACK (pn_test_count_emits), &emits);
    pn_message_set_topic (m, "t/x");
    pn_message_set_double (m, "value", 3.0);
    pn_message_set_string (m, "note", "keep");
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (emits, ==, 1u);
    PN_CHECK_CMPSTR (pn_message_get_topic (m), ==, "t/x");
    PN_CHECK_CMPSTR (pn_test_str (m, "note"), ==, "keep");

    g_object_unref (m);
    g_object_unref (node);
}

/* "rules" JSON: round trip, trailing unused trimmed, unused holes kept,
 * junk cleared; output names follow; shrink keeps rules. */
static void
test_rules_json_and_names (void)
{
    PnNode *node = make_node (4,
            "[{\"op\":\"==\",\"value\":3},{},"
             "{\"op\":\"range\",\"low\":10,\"high\":20.5},"
             "{\"op\":\"else\"},{\"op\":\"bogus\"}]", NULL);
    gchar  *json;
    gdouble a, b;

    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "== 3");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 1), ==, "out2");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 2), ==, "10..20.5");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 3), ==, "else");

    PN_CHECK_CMPINT (pn_value_router_get_rule (vr (node), 2, &a, &b), ==,
                     PN_VALUE_ROUTER_OP_RANGE);
    PN_CHECK_NEAR (a, 10.0, 1e-9);
    PN_CHECK_NEAR (b, 20.5, 1e-9);
    PN_CHECK_CMPINT (pn_value_router_get_rule (vr (node), 4, NULL, NULL), ==,
                     PN_VALUE_ROUTER_OP_UNUSED);

    g_object_get (node, "rules", &json, NULL);
    PN_CHECK_CMPSTR (json, ==,
            "[{\"op\":\"==\",\"value\":3.0},{},"
             "{\"op\":\"range\",\"low\":10.0,\"high\":20.5},"
             "{\"op\":\"else\"}]");

    /* Round trip. */
    {
        PnNode *copy = make_node (4, json, NULL);
        gchar  *again;

        g_object_get (copy, "rules", &again, NULL);
        PN_CHECK_CMPSTR (again, ==, json);
        g_free (again);
        g_object_unref (copy);
    }
    g_free (json);

    /* Shrink keeps the rules of removed outputs. */
    g_object_set (node, "outputs", 2, NULL);
    PN_CHECK_CMPINT (pn_value_router_route_number (vr (node), TRUE, 15.0), ==, -1);
    g_object_set (node, "outputs", 4, NULL);
    PN_CHECK_CMPINT (pn_value_router_route_number (vr (node), TRUE, 15.0), ==, 2);

    g_object_set (node, "rules", "not json", NULL);
    g_object_get (node, "rules", &json, NULL);
    PN_CHECK_CMPSTR (json, ==, "[]");
    g_free (json);

    g_object_unref (node);
}

static void
count_notify (GObject *object, GParamSpec *pspec, gpointer counter)
{
    (void) object;
    (void) pspec;
    (*(guint *) counter)++;
}

/* set_rule notifies only on a change; values an op ignores do not count. */
static void
test_set_rule_notifies (void)
{
    PnNode *node     = make_node (2, "[]", NULL);
    guint   notified = 0;

    g_signal_connect (node, "notify::rules", G_CALLBACK (count_notify), &notified);

    pn_value_router_set_rule (vr (node), 0, PN_VALUE_ROUTER_OP_GT, 1.0, 7.0);
    pn_value_router_set_rule (vr (node), 0, PN_VALUE_ROUTER_OP_GT, 1.0, 9.0);
    PN_CHECK_CMPINT (notified, ==, 1u);

    pn_value_router_set_rule (vr (node), 1, PN_VALUE_ROUTER_OP_ELSE, 4.0, 0.0);
    pn_value_router_set_rule (vr (node), 1, PN_VALUE_ROUTER_OP_ELSE, 8.0, 0.0);
    PN_CHECK_CMPINT (notified, ==, 2u);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-value-router");
    pn_test_add ("defaults",            test_defaults);
    pn_test_add ("operators",           test_operators);
    pn_test_add ("first_match_else",    test_first_match_and_else);
    pn_test_add ("path",                test_path);
    pn_test_add ("forwards_unchanged",  test_forwards_unchanged);
    pn_test_add ("rules_json_names",    test_rules_json_and_names);
    pn_test_add ("set_rule_notifies",   test_set_rule_notifies);
    return pn_test_run ();
}
