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

/* Unit tests for PnFormat: pick one of two templates based on
 * data.success, expand ${path} placeholders against the message JSON,
 * and write the result to data.output. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-format.h"
#include "pn-flow.h"

static PnNode *
make_node (const gchar *success_text,
           const gchar *failure_text,
           guint       *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_FORMAT,
                                 "success-text", success_text,
                                 "failure-text", failure_text,
                                 NULL);

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
test_success_template_with_placeholder (void)
{
    guint      emits;
    PnNode    *node = make_node ("OK ${data/value}", "BAD", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_boolean (msg, "success", TRUE);
    pn_message_set_string  (msg, "value",   "hi");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "OK hi");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_failure_template (void)
{
    guint      emits;
    PnNode    *node = make_node ("OK", "BAD", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_boolean (msg, "success", FALSE);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "BAD");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_missing_success_is_failure (void)
{
    guint      emits;
    PnNode    *node = make_node ("OK", "BAD", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* No "success" member at all — treated as false. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "BAD");

    g_object_unref (msg);
    g_object_unref (node);
}

/* A placeholder whose path is absent from the message renders empty
 * (PN_SUBST_MISS_EMPTY), leaving the surrounding literal intact. */
static void
test_missing_placeholder_is_empty (void)
{
    guint      emits;
    PnNode    *node = make_node ("v=[${data/nope}]", "BAD", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_boolean (msg, "success", TRUE);
    pn_message_set_string  (msg, "value",   "hi");   /* unrelated field */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "v=[]");

    g_object_unref (msg);
    g_object_unref (node);
}

/* A ${name} with no matching message field falls through to a document
 * global of that name; the int64 global renders as its decimal text. */
static void
test_document_global_placeholder (void)
{
    guint      emits;
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = make_node ("${env} build ${build}", "BAD", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    set_string_global (flow, "env",   "prod");
    set_int_global    (flow, "build", 42);
    pn_flow_add_node (flow, node);   /* wires the node's back-pointer */

    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "prod build 42");

    g_object_unref (msg);
    g_object_unref (node);
    g_object_unref (flow);
}

/* The message-field resolver precedes the globals resolver, so a field
 * present in the message wins over a same-named document global; only a
 * miss against the message falls through to the global. */
static void
test_message_field_beats_global (void)
{
    guint      emits;
    PnFlow    *flow = pn_flow_new ();
    PnNode    *node = make_node ("${topic}/${env}", "BAD", &emits);
    /* "topic" exists in the message envelope and also as a global. */
    PnMessage *msg  = pn_message_new (NULL, "msg-topic");

    set_string_global (flow, "topic", "global-topic");
    set_string_global (flow, "env",   "prod");
    pn_flow_add_node (flow, node);

    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    /* topic resolves from the message, env only from the global. */
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "msg-topic/prod");

    g_object_unref (msg);
    g_object_unref (node);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-format");
    pn_test_add ("success_template",   test_success_template_with_placeholder);
    pn_test_add ("failure_template",   test_failure_template);
    pn_test_add ("missing_success",    test_missing_success_is_failure);
    pn_test_add ("missing_placeholder", test_missing_placeholder_is_empty);
    pn_test_add ("global_placeholder",  test_document_global_placeholder);
    pn_test_add ("field_beats_global",  test_message_field_beats_global);
    return pn_test_run ();
}
