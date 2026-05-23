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

/* Unit tests for PnDebug's message rendering.  The node never writes a
 * file or opens a GUI here: pointing its target at the debug view makes
 * it emit the rendered string as the "debug-message" signal, which the
 * test captures.  The three formats are exercised:
 *   TEXT      -> just the message's data.output string
 *   ONELINER  -> a single "[name] from=... topic=... data={...}" line
 *   JSON      -> the whole envelope as pretty JSON */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-debug.h"

#include <string.h>

typedef struct
{
    guint  count;
    gchar *last;     /* most recent rendered string (owned) */
} Capture;

static void
on_debug_message (PnNode *node, const gchar *text, gpointer user_data)
{
    Capture *cap = user_data;

    (void) node;
    cap->count++;
    g_free (cap->last);
    cap->last = g_strdup (text);
}

static PnNode *
make_node (PnDebugFormat format, Capture *cap)
{
    PnNode *node = g_object_new (PN_TYPE_DEBUG,
                                 "target", PN_DEBUG_TARGET_DEBUG_VIEW,
                                 "format", format,
                                 NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "debug-message",
                      G_CALLBACK (on_debug_message), cap);
    return node;
}

/* A message carrying data.output = @output (plus a topic so the
 * one-liner has something non-trivial to print). */
static PnMessage *
make_message (const gchar *output)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_topic  (msg, "sensors/x");
    pn_message_set_string (msg, "output", output);
    return msg;
}

static void
test_text_renders_output_only (void)
{
    Capture    cap;
    PnNode    *node = make_node (PN_DEBUG_FORMAT_TEXT, &cap);
    PnMessage *msg  = make_message ("hello world");

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (cap.last, ==, "hello world");

    g_object_unref (msg);
    g_free (cap.last);
    g_object_unref (node);
}

static void
test_text_blank_without_output (void)
{
    Capture    cap;
    PnNode    *node = make_node (PN_DEBUG_FORMAT_TEXT, &cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* No data.output member -> an empty line, so the event still
     * registers downstream without crashing on a missing field. */
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (cap.last, ==, "");

    g_object_unref (msg);
    g_free (cap.last);
    g_object_unref (node);
}

static void
test_oneliner_has_fields (void)
{
    Capture    cap;
    PnNode    *node = make_node (PN_DEBUG_FORMAT_ONELINER, &cap);
    PnMessage *msg  = make_message ("ignored-by-oneliner");

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK (cap.last != NULL && cap.last[0] == '[');
    PN_CHECK (cap.last != NULL && strstr (cap.last, "topic=sensors/x") != NULL);
    PN_CHECK (cap.last != NULL && strstr (cap.last, "data=") != NULL);

    g_object_unref (msg);
    g_free (cap.last);
    g_object_unref (node);
}

static void
test_json_is_pretty_envelope (void)
{
    Capture    cap;
    PnNode    *node = make_node (PN_DEBUG_FORMAT_JSON, &cap);
    PnMessage *msg  = make_message ("hello world");

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    /* Pretty JSON object: opens with a brace and carries the data the
     * payload set. */
    PN_CHECK (cap.last != NULL && cap.last[0] == '{');
    PN_CHECK (cap.last != NULL && strstr (cap.last, "\"output\"") != NULL);
    PN_CHECK (cap.last != NULL && strstr (cap.last, "hello world") != NULL);

    g_object_unref (msg);
    g_free (cap.last);
    g_object_unref (node);
}

/* Debug is a header-only node — its output goes to the debug view, not
 * to a body drawn on the worksheet — so it reports no client area, and
 * the palette drag-preview outlines its header alone. */
static void
test_has_no_client_area (void)
{
    PnNode *node = g_object_new (PN_TYPE_DEBUG, NULL);
    double  x = -1.0, y = -1.0, w = -1.0, h = -1.0;

    PN_CHECK_FALSE (pn_node_get_client_area (node, &x, &y, &w, &h));
    /* The out-parameters are zeroed when there is no client area. */
    PN_CHECK_NEAR (x, 0.0, 0.01);
    PN_CHECK_NEAR (y, 0.0, 0.01);
    PN_CHECK_NEAR (w, 0.0, 0.01);
    PN_CHECK_NEAR (h, 0.0, 0.01);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-debug");
    pn_test_add ("text_output_only",       test_text_renders_output_only);
    pn_test_add ("text_blank_no_output",   test_text_blank_without_output);
    pn_test_add ("oneliner_has_fields",    test_oneliner_has_fields);
    pn_test_add ("json_pretty_envelope",   test_json_is_pretty_envelope);
    pn_test_add ("has_no_client_area",     test_has_no_client_area);
    return pn_test_run ();
}
