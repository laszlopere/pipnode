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

/* Unit tests for PnFilter: forward a message only when every compiled
 * {path, op, literal} rule passes.  An empty rule set forwards
 * everything; a string literal with a glob metacharacter switches the
 * equality operators to pattern matching. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-filter.h"

static PnNode *
make_node (const gchar *rules, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_FILTER, NULL);

    if (rules != NULL)
        g_object_set (node, "rules", rules, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
send_value (PnNode *node, gdouble value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
send_name (PnNode *node, const gchar *name)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "name", name);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_empty_rules_passthrough (void)
{
    guint   emits;
    PnNode *node = make_node (NULL, &emits);       /* default "[]" */

    send_value (node, 1.0);
    send_value (node, 99.0);
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

static void
test_numeric_threshold (void)
{
    guint   emits;
    PnNode *node = make_node (
            "[{\"path\":\"value\",\"op\":\">=\",\"literal\":10}]", &emits);

    send_value (node, 5.0);          /* below threshold -> dropped */
    PN_CHECK_CMPINT (emits, ==, 0);

    send_value (node, 10.0);         /* on the boundary -> forward */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, 15.0);         /* above           -> forward */
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

static void
test_string_glob_match (void)
{
    guint   emits;
    PnNode *node = make_node (
            "[{\"path\":\"name\",\"op\":\"==\",\"literal\":\"fo*\"}]", &emits);

    send_name (node, "foo");         /* matches fo*  -> forward */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_name (node, "bar");         /* no match     -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

static void
test_rules_are_anded (void)
{
    guint   emits;
    PnNode *node = make_node (
            "[{\"path\":\"value\",\"op\":\">\",\"literal\":0},"
            " {\"path\":\"value\",\"op\":\"<\",\"literal\":100}]", &emits);

    send_value (node, 50.0);         /* in (0, 100)   -> forward */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, 150.0);        /* fails the < rule -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, -1.0);         /* fails the > rule -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-filter");
    pn_test_add ("empty_passthrough",      test_empty_rules_passthrough);
    pn_test_add ("numeric_threshold",      test_numeric_threshold);
    pn_test_add ("string_glob_match",      test_string_glob_match);
    pn_test_add ("rules_anded",            test_rules_are_anded);
    return pn_test_run ();
}
