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

/* Unit tests for PnSwitch: a latch that forwards a message only when
 * the inbound data.value crosses the midpoint into the opposite
 * position; a value matching the current latch is dropped to break the
 * status/command echo loop.  Messages without a usable "value" pass
 * through unchanged and leave the latch alone. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-switch.h"

static PnNode *
make_node (guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_SWITCH, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Send data.value = @value; returns the (possibly forwarded) message so
 * a caller can assert the payload passed through unchanged. */
static void
send_value (PnNode *node, gdouble value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static gboolean
switch_on (PnNode *node)
{
    gboolean on = FALSE;
    g_object_get (node, "on", &on, NULL);
    return on;
}

static void
test_latches_and_drops_noops (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* Default position is off.  A value above the midpoint flips the
     * latch on and forwards; a repeat is a no-op edge and is dropped. */
    send_value (node, 1.0);
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (switch_on (node));

    send_value (node, 1.0);          /* already on -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    send_value (node, 0.0);          /* on -> off  -> forward */
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_FALSE  (switch_on (node));

    send_value (node, 0.0);          /* already off -> dropped */
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

static void
test_midpoint_counts_as_off (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* The threshold is value > 0.5, so exactly 0.5 reads as "off" and,
     * matching the default off latch, is dropped. */
    send_value (node, 0.5);
    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE  (switch_on (node));

    send_value (node, 0.6);          /* now over the line -> on */
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (switch_on (node));

    g_object_unref (node);
}

static void
test_passthrough_without_value (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A message with no usable "value" cannot drive the latch, but may
     * carry annotations the user expects to flow through, so it is
     * forwarded unchanged with the latch left untouched. */
    pn_message_set_string (msg, "output", "note");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (switch_on (node));
    PN_CHECK_CMPSTR (pn_test_str (msg, "output"), ==, "note");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_integer_value_drives_latch (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* An integer "value" (JSON int, not double) drives the latch just
     * like a double — read_value_member accepts both numeric types. */
    pn_message_set_int (msg, "value", 1);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK        (switch_on (node));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_non_numeric_value_passes_through (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A "value" that is present but not numeric cannot drive the latch:
     * it is treated like a message with no usable value, so it passes
     * through unchanged and the latch stays in its default off position. */
    pn_message_set_string (msg, "value", "on");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_FALSE  (switch_on (node));
    PN_CHECK_CMPSTR (pn_test_str (msg, "value"), ==, "on");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_announces_state_once_on_startup (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* A switch announces its latch state once shortly after construction
     * so a freshly-loaded worksheet lets downstream nodes learn the
     * position without a click.  The announce is a one-shot main-loop
     * idle scheduled at construction; nothing has fired yet. */
    g_object_set (node, "on", TRUE, NULL);   /* set silently */
    PN_CHECK_CMPINT (emits, ==, 0);

    /* Pump the default context until the idle fires. */
    while (emits == 0 && g_main_context_iteration (NULL, TRUE))
        ;
    PN_CHECK_CMPINT (emits, ==, 1);

    /* One-shot: it does not recur like a periodic data source. */
    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

static void
test_announce_opt_out_is_silent (void)
{
    guint   emits;
    PnNode *node = make_node (&emits);

    /* Opting out cancels the pending announce so no startup emit fires
     * -- the behaviour PnTasmotaSwitch relies on (it opts out from its
     * init, before the base schedules the shot) so opening a worksheet
     * never commands a physical relay.  Here the call lands just after
     * construction, exercising the late-cancel branch of the setter. */
    pn_switch_set_announce_on_startup (PN_SWITCH (node), FALSE);

    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-switch");
    pn_test_add ("latch_drops_noops",      test_latches_and_drops_noops);
    pn_test_add ("midpoint_is_off",        test_midpoint_counts_as_off);
    pn_test_add ("passthrough_no_value",   test_passthrough_without_value);
    pn_test_add ("integer_drives_latch",   test_integer_value_drives_latch);
    pn_test_add ("non_numeric_passes",     test_non_numeric_value_passes_through);
    pn_test_add ("startup_announce",       test_announces_state_once_on_startup);
    pn_test_add ("startup_opt_out",        test_announce_opt_out_is_silent);
    return pn_test_run ();
}
