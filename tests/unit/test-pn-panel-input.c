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

/* Unit tests for PnPanelInput: a source the panel engine drives via
 * pn_panel_input_send() (a "value" message) and pn_panel_input_send_event()
 * (a mouse event, "<event>/<button>" topic plus button identity).  It
 * announces its current value once on startup (a one-shot idle, like
 * PnSwitch/PnKnob) and emits on every send even when the value repeats. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-panel-input.h"

/* Capture the most recently emitted message so a test can assert on the
 * payload the node built for itself. */
static void
capture_last (PnNode *node, PnMessage *message, gpointer user_data)
{
    PnMessage **slot = user_data;
    (void) node;
    g_clear_object (slot);
    *slot = g_object_ref (message);
}

/* Pump the default context until @emits reaches @want (or no more work),
 * used to let the deferred startup-announce idle fire. */
static void
drain_until (guint *emits, guint want)
{
    while (*emits < want && g_main_context_iteration (NULL, TRUE))
        ;
}

static void
test_send_emits_value (void)
{
    guint       emits = 0;
    PnMessage  *last  = NULL;
    PnNode     *node  = g_object_new (PN_TYPE_PANEL_INPUT, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    g_signal_connect (node, "message",
                      G_CALLBACK (capture_last), &last);

    /* Let the startup announce of the default value (0) fire first. */
    drain_until (&emits, 1);
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (last, "value"), 0.0, 1e-9);

    /* A send emits the new value with success=true. */
    pn_panel_input_send (PN_PANEL_INPUT (node), 42.0);
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (last, "value"), 42.0, 1e-9);
    PN_CHECK        (pn_test_bool (last, "success"));

    g_clear_object (&last);
    g_object_unref (node);
}

static void
test_startup_announces_loaded_value (void)
{
    guint       emits = 0;
    PnMessage  *last  = NULL;
    PnNode     *node  = g_object_new (PN_TYPE_PANEL_INPUT, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    g_signal_connect (node, "message",
                      G_CALLBACK (capture_last), &last);

    /* The loader applies the saved property after construction; the
     * one-shot reads it when it fires, so the announce carries 7. */
    g_object_set (node, "value", 7.0, NULL);
    PN_CHECK_CMPINT (emits, ==, 0);

    drain_until (&emits, 1);
    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (last, "value"), 7.0, 1e-9);

    /* One-shot: it does not recur. */
    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (emits, ==, 1);

    g_clear_object (&last);
    g_object_unref (node);
}

static void
test_repeated_send_emits_each_time (void)
{
    guint   emits = 0;
    PnNode *node  = g_object_new (PN_TYPE_PANEL_INPUT, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    drain_until (&emits, 1);          /* startup announce */

    /* A panel button must register every click, so the same value
     * emitted twice produces two messages (unlike a deduping latch). */
    pn_panel_input_send (PN_PANEL_INPUT (node), 1.0);
    pn_panel_input_send (PN_PANEL_INPUT (node), 1.0);
    PN_CHECK_CMPINT (emits, ==, 3);

    g_object_unref (node);
}

static void
test_send_updates_property (void)
{
    guint    emits = 0;
    PnNode  *node  = g_object_new (PN_TYPE_PANEL_INPUT, NULL);
    gdouble  v     = -1.0;

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    drain_until (&emits, 1);

    /* send() records the value so it persists (and is re-announced on a
     * future load). */
    pn_panel_input_send (PN_PANEL_INPUT (node), 3.5);
    g_object_get (node, "value", &v, NULL);
    PN_CHECK_NEAR (v, 3.5, 1e-9);

    g_object_unref (node);
}

static void
test_send_event_topic_and_members (void)
{
    guint       emits = 0;
    PnMessage  *last  = NULL;
    gdouble     v     = -1.0;
    PnNode     *node  = g_object_new (PN_TYPE_PANEL_INPUT, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    g_signal_connect (node, "message",
                      G_CALLBACK (capture_last), &last);
    drain_until (&emits, 1);                       /* startup announce */

    /* A left-button click: topic "click/left", the button number on the
     * output "value", the human-readable summary on "output", and readable
     * button / event names. */
    pn_panel_input_send_event (PN_PANEL_INPUT (node), "click", 1);
    PN_CHECK_CMPINT (emits, ==, 2);
    PN_CHECK_CMPSTR (pn_message_get_topic (last), ==, "click/left");
    PN_CHECK_NEAR   (pn_test_num (last, "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (last, "button"), ==, "left");
    PN_CHECK_CMPSTR (pn_test_str (last, "event"),  ==, "click");
    PN_CHECK_CMPSTR (pn_test_str (last, "output"), ==,
                     "Applet left mouse button clicked.");
    PN_CHECK        (pn_test_bool (last, "success"));

    /* A right-button click names the other button. */
    pn_panel_input_send_event (PN_PANEL_INPUT (node), "click", 3);
    PN_CHECK_CMPINT (emits, ==, 3);
    PN_CHECK_CMPSTR (pn_message_get_topic (last), ==, "click/right");
    PN_CHECK_NEAR   (pn_test_num (last, "value"), 3.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (last, "button"), ==, "right");
    PN_CHECK_CMPSTR (pn_test_str (last, "output"), ==,
                     "Applet right mouse button clicked.");

    /* The event carries the button without disturbing the configured
     * "value" property (still the startup default 0). */
    g_object_get (node, "value", &v, NULL);
    PN_CHECK_NEAR (v, 0.0, 1e-9);

    g_clear_object (&last);
    g_object_unref (node);
}

static void
test_ports (void)
{
    PnNode *node = g_object_new (PN_TYPE_PANEL_INPUT, NULL);

    /* A source: output only. */
    PN_CHECK       (pn_node_get_has_output (node));
    PN_CHECK_FALSE (pn_node_get_has_input (node));

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-panel-input");
    pn_test_add ("send_emits_value",     test_send_emits_value);
    pn_test_add ("startup_announce",     test_startup_announces_loaded_value);
    pn_test_add ("repeated_send",        test_repeated_send_emits_each_time);
    pn_test_add ("send_updates_value",   test_send_updates_property);
    pn_test_add ("send_event",           test_send_event_topic_and_members);
    pn_test_add ("ports",                test_ports);
    return pn_test_run ();
}
