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

/* Unit tests for PnText: overwrite data.output with a fixed, pre-
 * configured string, with leading/trailing newlines stripped. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-text.h"

static PnNode *
make_node (const gchar *text, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_TEXT, "text", text, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* A plain string is written verbatim to data.output and the message is
 * forwarded once. */
static void
test_replaces_output (void)
{
    guint      emits;
    PnNode    *node = make_node ("hello world", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "old payload");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "hello world");

    g_object_unref (msg);
    g_object_unref (node);
}

/* The configured text replaces whatever the message carried, even when
 * there was no output member at all to begin with. */
static void
test_sets_output_when_absent (void)
{
    guint      emits;
    PnNode    *node = make_node ("fixed", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "fixed");

    g_object_unref (msg);
    g_object_unref (node);
}

/* Leading and trailing newlines are stripped; interior newlines and
 * trailing/leading spaces (non-newline whitespace) are preserved. */
static void
test_strips_edge_newlines (void)
{
    guint      emits;
    PnNode    *node = make_node ("\n\nline one\nline two\n\n", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "line one\nline two");

    g_object_unref (msg);
    g_object_unref (node);
}

/* Carriage returns at the edges are stripped alongside newlines. */
static void
test_strips_crlf (void)
{
    guint      emits;
    PnNode    *node = make_node ("\r\nbody\r\n", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "body");

    g_object_unref (msg);
    g_object_unref (node);
}

/* A text consisting only of newlines collapses to an empty string. */
static void
test_only_newlines_is_empty (void)
{
    guint      emits;
    PnNode    *node = make_node ("\n\n\n", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "");

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-text");
    pn_test_add ("replaces_output",     test_replaces_output);
    pn_test_add ("sets_when_absent",    test_sets_output_when_absent);
    pn_test_add ("strips_edge_newlines", test_strips_edge_newlines);
    pn_test_add ("strips_crlf",         test_strips_crlf);
    pn_test_add ("only_newlines_empty", test_only_newlines_is_empty);
    return pn_test_run ();
}
