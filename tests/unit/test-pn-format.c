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

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-format");
    pn_test_add ("success_template", test_success_template_with_placeholder);
    pn_test_add ("failure_template", test_failure_template);
    pn_test_add ("missing_success",  test_missing_success_is_failure);
    return pn_test_run ();
}
