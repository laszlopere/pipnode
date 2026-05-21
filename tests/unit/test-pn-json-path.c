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

/* Unit tests for the JSON path helpers shared by Dedup/Format/Graph/…:
 * pn_json_resolve_path() walks a "/"-separated path through a lookup
 * root, and pn_json_lookup_root_for_message() builds that root from a
 * message's envelope plus a reference to its live data bag.  Headless. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-json-path.h"
#include "pn-message.h"

/* Build a small root:  { "topic": "sensors",
 *                        "data": { "value": 42, "label": "cpu" } }  */
static JsonObject *
make_root (void)
{
    JsonObject *root = json_object_new ();
    JsonObject *data = json_object_new ();

    json_object_set_double_member (data, "value", 42.0);
    json_object_set_string_member (data, "label", "cpu");
    json_object_set_object_member (root, "data", data);   /* transfers data */
    json_object_set_string_member (root, "topic", "sensors");

    return root;
}

static void
test_resolve_simple (void)
{
    JsonObject *root = make_root ();
    JsonNode   *n;

    n = pn_json_resolve_path (root, "topic");
    PN_CHECK (n != NULL && JSON_NODE_HOLDS_VALUE (n));
    PN_CHECK_CMPSTR (json_node_get_string (n), ==, "sensors");

    n = pn_json_resolve_path (root, "data");
    PN_CHECK (n != NULL && JSON_NODE_HOLDS_OBJECT (n));

    json_object_unref (root);
}

static void
test_resolve_nested (void)
{
    JsonObject *root = make_root ();
    JsonNode   *n;

    n = pn_json_resolve_path (root, "data/value");
    PN_CHECK (n != NULL && JSON_NODE_HOLDS_VALUE (n));
    PN_CHECK_NEAR (json_node_get_double (n), 42.0, 1e-9);

    n = pn_json_resolve_path (root, "data/label");
    PN_CHECK (n != NULL);
    PN_CHECK_CMPSTR (json_node_get_string (n), ==, "cpu");

    json_object_unref (root);
}

static void
test_resolve_slash_tolerance (void)
{
    JsonObject *root = make_root ();
    JsonNode   *n;

    /* Leading, trailing, and doubled separators are all skipped. */
    n = pn_json_resolve_path (root, "/data//value/");
    PN_CHECK (n != NULL && JSON_NODE_HOLDS_VALUE (n));
    PN_CHECK_NEAR (json_node_get_double (n), 42.0, 1e-9);

    json_object_unref (root);
}

static void
test_resolve_misses (void)
{
    JsonObject *root = make_root ();

    /* Missing member. */
    PN_CHECK (pn_json_resolve_path (root, "data/nope") == NULL);
    /* Descending through a scalar. */
    PN_CHECK (pn_json_resolve_path (root, "data/value/x") == NULL);
    /* Empty path and NULL root. */
    PN_CHECK (pn_json_resolve_path (root, "") == NULL);
    PN_CHECK (pn_json_resolve_path (NULL, "topic") == NULL);

    json_object_unref (root);
}

static void
test_root_for_message (void)
{
    PnMessage  *msg  = pn_message_new (NULL, "mytopic");
    JsonObject *root;
    JsonNode   *n;

    pn_message_set_double (msg, "value", 7.0);

    root = pn_json_lookup_root_for_message (msg);

    /* Envelope fields are carried as string members. */
    PN_CHECK (json_object_has_member (root, "topic"));
    PN_CHECK (json_object_has_member (root, "id"));
    PN_CHECK (json_object_has_member (root, "created"));
    PN_CHECK_CMPSTR (json_object_get_string_member (root, "topic"),
                     ==, "mytopic");

    /* The data bag is reachable through the root by path. */
    n = pn_json_resolve_path (root, "data/value");
    PN_CHECK (n != NULL && JSON_NODE_HOLDS_VALUE (n));
    PN_CHECK_NEAR (json_node_get_double (n), 7.0, 1e-9);

    json_object_unref (root);
    g_object_unref (msg);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-json-path");
    pn_test_add ("resolve_simple",     test_resolve_simple);
    pn_test_add ("resolve_nested",     test_resolve_nested);
    pn_test_add ("resolve_slashes",    test_resolve_slash_tolerance);
    pn_test_add ("resolve_misses",     test_resolve_misses);
    pn_test_add ("root_for_message",   test_root_for_message);
    return pn_test_run ();
}
