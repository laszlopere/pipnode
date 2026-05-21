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

/* Unit tests for PnQuery: run a JMESPath expression against (part of)
 * the incoming message and store the result.  A non-empty result-field
 * writes that one member; an empty result-field replaces the whole data
 * bag (scalars wrapped under "value"). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-query.h"

static PnNode *
make_node (const gchar *expression,
           const gchar *result_field,
           const gchar *source_field,
           guint       *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_QUERY,
                                 "expression",   expression,
                                 "result-field", result_field,
                                 "source-field", source_field,
                                 NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
test_extracts_into_named_field (void)
{
    guint      emits;
    /* Whole-message root (source-field empty) -> data.value -> answer. */
    PnNode    *node = make_node ("data.value", "answer", "", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 99.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "answer"), 99.0, 1e-9);
    PN_CHECK_FALSE  (pn_test_has (msg, "error"));
    /* The named-field form leaves the original members in place. */
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 99.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_empty_result_field_replaces_bag (void)
{
    guint      emits;
    /* Empty result-field replaces the bag; a scalar result is wrapped
     * under "value". */
    PnNode    *node = make_node ("data.value", "", "", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 99.0);
    pn_message_set_string (msg, "extra", "gone");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 99.0, 1e-9);
    PN_CHECK_FALSE  (pn_test_has (msg, "extra"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_source_field_scopes_input (void)
{
    guint      emits;
    /* Run the query against the data bag directly: "value" resolves to
     * data.value, stored back as data.doubled. */
    PnNode    *node = make_node ("value", "doubled", "data", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 7.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "doubled"), 7.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"),   7.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-query");
    pn_test_add ("extracts_into_field",   test_extracts_into_named_field);
    pn_test_add ("empty_field_replaces",  test_empty_result_field_replaces_bag);
    pn_test_add ("source_field_scopes",   test_source_field_scopes_input);
    return pn_test_run ();
}
