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

/* Unit tests for PnSet: stamp a list of {path, literal} assignments
 * onto every passing message.  An empty rule set forwards untouched;
 * a dotted path materialises the nested data-bag shape on demand. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-set.h"

#include <json-glib/json-glib.h>

static PnNode *
make_node (const gchar *props, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_SET, NULL);

    if (props != NULL)
        g_object_set (node, "props", props, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
test_empty_props_passthrough (void)
{
    guint      emits;
    PnNode    *node = make_node (NULL, &emits);     /* default "[]" */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 7.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 7.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_assigns_typed_literals (void)
{
    guint      emits;
    PnNode    *node = make_node (
            "[{\"path\":\"value\",\"literal\":1.5},"
            " {\"path\":\"output\",\"literal\":\"hi\"},"
            " {\"path\":\"success\",\"literal\":true}]",
            &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* The incoming message starts bare; every member below is created
     * by the assignments, each carrying its literal's JSON type. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num  (msg, "value"),  1.5, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str  (msg, "output"), ==, "hi");
    PN_CHECK        (pn_test_bool (msg, "success"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_overwrites_existing_member (void)
{
    guint      emits;
    PnNode    *node = make_node (
            "[{\"path\":\"value\",\"literal\":42.0}]", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 42.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_dotted_path_creates_nested (void)
{
    guint       emits;
    PnNode     *node = make_node (
            "[{\"path\":\"a.b\",\"literal\":\"deep\"}]", &emits);
    PnMessage  *msg  = pn_message_new (NULL, NULL);
    JsonNode   *a;
    JsonObject *a_obj;

    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (emits, ==, 1);

    /* "a.b" should have built a fresh object at data.a holding b. */
    a = pn_message_get_member (msg, "a");
    PN_CHECK (a != NULL && JSON_NODE_HOLDS_OBJECT (a));
    if (a != NULL && JSON_NODE_HOLDS_OBJECT (a))
    {
        a_obj = json_node_get_object (a);
        PN_CHECK (json_object_has_member (a_obj, "b"));
        PN_CHECK_CMPSTR (json_object_get_string_member (a_obj, "b"),
                         ==, "deep");
    }

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-set");
    pn_test_add ("empty_passthrough",      test_empty_props_passthrough);
    pn_test_add ("assigns_literals",       test_assigns_typed_literals);
    pn_test_add ("overwrites_member",      test_overwrites_existing_member);
    pn_test_add ("dotted_path_nested",     test_dotted_path_creates_nested);
    return pn_test_run ();
}
