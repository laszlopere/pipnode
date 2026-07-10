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

/* Unit tests for PnParse: turn a JSON string held in one member of the
 * message into real structure.  A non-empty result-field writes that one
 * member; an empty result-field replaces the whole data bag.  Failures
 * never drop the message and never destroy the unparsed text. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-parse.h"

static PnNode *
make_node (const gchar *source_field,
           const gchar *result_field,
           guint       *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_PARSE,
                                 "source-field", source_field,
                                 "result-field", result_field,
                                 NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/** Reach into data.<field>.<key> for a number. */
static gdouble
nested_num (PnMessage *msg, const gchar *field, const gchar *key)
{
    JsonNode *n = pn_message_get_member (msg, field);
    g_assert_nonnull (n);
    g_assert_true (JSON_NODE_HOLDS_OBJECT (n));
    return json_object_get_double_member (json_node_get_object (n), key);
}

static void
test_parses_into_named_field (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/output", "result", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "{\"temp\": 21.5}");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (nested_num (msg, "result", "temp"), 21.5, 1e-9);
    PN_CHECK_FALSE  (pn_test_has (msg, "error"));
    /* The named-field form leaves the raw body in place. */
    PN_CHECK        (pn_test_has (msg, "output"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_empty_result_field_replaces_bag (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/output", "", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "{\"temp\": 21.5}");
    pn_message_set_string (msg, "extra",  "gone");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "temp"), 21.5, 1e-9);
    /* The document became the bag: raw body and strays are gone. */
    PN_CHECK_FALSE  (pn_test_has (msg, "output"));
    PN_CHECK_FALSE  (pn_test_has (msg, "extra"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_scalar_document_wrapped_under_value (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/output", "data", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A bare scalar is a valid JSON document but not a valid data bag. */
    pn_message_set_string (msg, "output", "42");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 42.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_bad_json_forwards_and_keeps_body (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/output", "result", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "{not json");
    pn_node_receive_message (node, msg);

    /* Never dropped, never destructive: the body survives for the user
     * to look at, and the reason lands in data.error. */
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (pn_test_has (msg, "error"));
    PN_CHECK_FALSE  (pn_test_has (msg, "result"));
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "{not json");
    PN_CHECK        (pn_node_get_has_error (node));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_missing_field_is_an_error (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/nope", "result", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "{}");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "error"), ==, "no such field");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_non_string_field_is_an_error (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/value", "result", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 7.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "error"), ==, "field is not a string");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_success_clears_previous_error (void)
{
    guint      emits;
    PnNode    *node = make_node ("data/output", "result", &emits);
    PnMessage *bad  = pn_message_new (NULL, NULL);
    PnMessage *good = pn_message_new (NULL, NULL);

    pn_message_set_string (bad, "output", "{oops");
    pn_node_receive_message (node, bad);
    PN_CHECK (pn_node_get_has_error (node));

    pn_message_set_string (good, "output", "{\"ok\": 1}");
    pn_node_receive_message (node, good);

    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_FALSE  (pn_test_has (good, "error"));
    PN_CHECK_FALSE  (pn_node_get_has_error (node));

    g_object_unref (good);
    g_object_unref (bad);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-parse");
    pn_test_add ("parses_into_field",   test_parses_into_named_field);
    pn_test_add ("empty_field_replaces", test_empty_result_field_replaces_bag);
    pn_test_add ("scalar_wrapped",      test_scalar_document_wrapped_under_value);
    pn_test_add ("bad_json_keeps_body", test_bad_json_forwards_and_keeps_body);
    pn_test_add ("missing_field",       test_missing_field_is_an_error);
    pn_test_add ("non_string_field",    test_non_string_field_is_an_error);
    pn_test_add ("success_clears_error", test_success_clears_previous_error);
    return pn_test_run ();
}
