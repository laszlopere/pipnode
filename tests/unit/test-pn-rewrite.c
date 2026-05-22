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

/* Unit tests for PnRewrite: rebuild an outgoing message from a JSON
 * template whose ${path} placeholders expand to values from the
 * incoming message.  An empty template forwards untouched; a template
 * with envelope members (topic/data) reshapes those, while one without
 * is treated as the new data bag. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-rewrite.h"
#include "pn-flow.h"

#include <json-glib/json-glib.h>

static PnNode *
make_node (const gchar *template_text, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_REWRITE, NULL);

    if (template_text != NULL)
        g_object_set (node, "template", template_text, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Define a string-valued document global on @flow. */
static void
set_string_global (PnFlow *flow, const gchar *name, const gchar *value)
{
    GValue v = G_VALUE_INIT;
    g_value_init (&v, G_TYPE_STRING);
    g_value_set_string (&v, value);
    pn_flow_set_global (flow, name, &v);
    g_value_unset (&v);
}

/* Define an int64-valued document global on @flow. */
static void
set_int_global (PnFlow *flow, const gchar *name, gint64 value)
{
    GValue v = G_VALUE_INIT;
    g_value_init (&v, G_TYPE_INT64);
    g_value_set_int64 (&v, value);
    pn_flow_set_global (flow, name, &v);
    g_value_unset (&v);
}

static void
test_empty_template_passthrough (void)
{
    guint      emits;
    PnNode    *node = make_node ("", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 3.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 3.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_data_bag_reshape (void)
{
    guint      emits;
    /* No envelope members, so the whole object becomes the new data
     * bag: data.value is replaced by data.doubled, and the original
     * value member disappears. */
    PnNode    *node = make_node (
            "{ \"doubled\": ${data/value} }", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 21.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "doubled"), 21.0, 1e-9);
    PN_CHECK_FALSE  (pn_test_has (msg, "value"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_topic_and_string_placeholder (void)
{
    guint      emits;
    PnNode    *node = make_node (
            "{ \"topic\": \"new/${topic}\","
            "  \"data\": { \"keep\": \"${data/output}\" } }", &emits);
    PnMessage *msg  = pn_message_new (NULL, "orig");

    pn_message_set_string (msg, "output", "hello");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "new/orig");
    PN_CHECK_CMPSTR (pn_test_str (msg, "keep"), ==, "hello");

    g_object_unref (msg);
    g_object_unref (node);
}

/* A do-nothing log handler used to swallow the warning the node emits
 * for a malformed template, so the expected-failure case below does not
 * spew to stderr.  (The harness does not run under g_test_init(), so the
 * g_test_expect_message() machinery is unavailable here.) */
static void
swallow_log (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        user_data)
{
    (void) domain; (void) level; (void) message; (void) user_data;
}

static void
test_invalid_json_leaves_message_untouched (void)
{
    guint      emits;
    /* After expansion this is not valid JSON; the node logs a warning
     * and forwards the message unchanged rather than dropping it. */
    PnNode    *node = make_node ("{ not valid json", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    GLogFunc   prev;

    pn_message_set_double (msg, "value", 5.0);

    prev = g_log_set_default_handler (swallow_log, NULL);
    pn_node_receive_message (node, msg);
    g_log_set_default_handler (prev, NULL);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 5.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* In JSON mode an unknown path is rendered as the bare token `null` in a
 * value slot, but collapses to empty inside a string literal — both must
 * keep the document well-formed (PN_SUBST_MISS_NULL). */
static void
test_missing_placeholder_null_and_empty (void)
{
    guint      emits;
    PnNode    *node = make_node (
            "{ \"miss\": ${data/nope},"
            "  \"label\": \"v=${data/nope}\" }", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    JsonNode  *miss;

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    /* Value slot: the member exists and holds JSON null. */
    miss = pn_test_member (msg, "miss");
    PN_CHECK (miss != NULL && JSON_NODE_HOLDS_NULL (miss));
    /* String slot: the placeholder vanished, leaving just the prefix. */
    PN_CHECK_CMPSTR (pn_test_str (msg, "label"), ==, "v=");

    g_object_unref (msg);
    g_object_unref (node);
}

/* A ${name} absent from the message resolves from a document global:
 * a string global inside a string slot, an int64 global as a bare JSON
 * number in a value slot. */
static void
test_document_global_placeholder (void)
{
    guint      emits;
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = make_node (
            "{ \"env\": \"${env}\", \"build\": ${build} }", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    set_string_global (flow, "env",   "prod");
    set_int_global    (flow, "build", 42);
    pn_flow_add_node (flow, node);   /* wires the node's back-pointer */

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "env"),   ==, "prod");
    PN_CHECK_NEAR   (pn_test_num (msg, "build"), 42.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
    g_object_unref (flow);
}

/* The message resolver precedes the globals resolver: a path served by
 * the message wins over a same-named global, while a miss falls through.
 * The envelope "topic" exists in the message and also as a global; the
 * message must win.  data.env has no message field, so it resolves from
 * the global. */
static void
test_message_field_beats_global (void)
{
    guint      emits;
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = make_node (
            "{ \"topic\": \"${topic}\","
            "  \"data\": { \"env\": \"${env}\","
            "             \"echo\": \"${topic}\" } }", &emits);
    PnMessage *msg  = pn_message_new (NULL, "msg-topic");

    set_string_global (flow, "topic", "global-topic");
    set_string_global (flow, "env",   "prod");
    pn_flow_add_node (flow, node);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    /* topic resolves from the message, beating the same-named global. */
    PN_CHECK_CMPSTR (pn_message_get_topic (msg), ==, "msg-topic");
    PN_CHECK_CMPSTR (pn_test_str (msg, "echo"), ==, "msg-topic");
    /* env has no message field, so it falls through to the global. */
    PN_CHECK_CMPSTR (pn_test_str (msg, "env"),  ==, "prod");

    g_object_unref (msg);
    g_object_unref (node);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-rewrite");
    pn_test_add ("empty_passthrough",      test_empty_template_passthrough);
    pn_test_add ("data_bag_reshape",       test_data_bag_reshape);
    pn_test_add ("topic_placeholder",      test_topic_and_string_placeholder);
    pn_test_add ("invalid_json_untouched", test_invalid_json_leaves_message_untouched);
    pn_test_add ("missing_placeholder",    test_missing_placeholder_null_and_empty);
    pn_test_add ("global_placeholder",     test_document_global_placeholder);
    pn_test_add ("field_beats_global",     test_message_field_beats_global);
    return pn_test_run ();
}
