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

/* Unit tests for PnValue: overwrite data.value with a fixed double set
 * in the node properties, forwarding the rest of the message untouched. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-value.h"

static PnNode *
make_node (gdouble value, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_VALUE, "value", value, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* The configured number replaces whatever the message carried and the
 * message is forwarded once. */
static void
test_replaces_value (void)
{
    guint      emits;
    PnNode    *node = make_node (3.5, &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 99.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 3.5, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* The value is set even when the message had no value member to begin
 * with. */
static void
test_sets_value_when_absent (void)
{
    guint      emits;
    PnNode    *node = make_node (-12.25, &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), -12.25, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* Only data.value is touched; the rest of the bag passes through. */
static void
test_passes_others_through (void)
{
    guint      emits;
    PnNode    *node = make_node (1.0, &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "output", "payload");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "payload");

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-value");
    pn_test_add ("replaces_value",      test_replaces_value);
    pn_test_add ("sets_when_absent",    test_sets_value_when_absent);
    pn_test_add ("passes_others",       test_passes_others_through);
    return pn_test_run ();
}
