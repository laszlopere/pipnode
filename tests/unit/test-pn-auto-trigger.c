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

/* Unit tests for the PnAutoTrigger synchronous test seam.  A tiny
 * test-only subclass whose trigger() emits a canned message lets us
 * verify, without touching any real IO node, that:
 *   - "autostart" = FALSE yields a quiescent instance (no worker thread);
 *   - pn_auto_trigger_run_once_sync() runs trigger() once on the calling
 *     thread and delivers the message synchronously via "message";
 *   - period clamping still holds.
 * No filesystem, network, or GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-auto-trigger.h"
#include "pn-node.h"

/* ---- test-only PnAutoTrigger subclass ---- */

#define TEST_TYPE_TRIGGER (test_trigger_get_type ())
G_DECLARE_FINAL_TYPE (TestTrigger, test_trigger, TEST, TRIGGER, PnAutoTrigger)

struct _TestTrigger
{
    PnAutoTrigger parent_instance;
    guint         trigger_calls;
};

G_DEFINE_TYPE (TestTrigger, test_trigger, PN_TYPE_AUTO_TRIGGER)

static void
test_trigger_trigger (PnAutoTrigger *trigger)
{
    TestTrigger *self = TEST_TRIGGER (trigger);
    PnMessage   *msg  = pn_message_new (PN_NODE (self), NULL);

    self->trigger_calls++;
    pn_message_set_double (msg, "value", 42.0);
    pn_message_set_string (msg, "output", "tick");

    pn_auto_trigger_emit_on_main (trigger, msg);   /* transfer */
}

static void
test_trigger_class_init (TestTriggerClass *klass)
{
    PN_AUTO_TRIGGER_CLASS (klass)->trigger = test_trigger_trigger;
}

static void
test_trigger_init (TestTrigger *self)
{
    self->trigger_calls = 0;
}

/* ---- emission capture ---- */

typedef struct
{
    guint      count;
    PnMessage *last;
} Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;
    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

/* ---- tests ---- */

static void
test_run_once_sync_emits (void)
{
    Capture      cap  = { 0 };
    TestTrigger *node = g_object_new (TEST_TYPE_TRIGGER,
                                      "autostart", FALSE, NULL);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));

    /* trigger() ran exactly once and its message arrived synchronously,
     * before run_once_sync returned — no main loop was spun. */
    PN_CHECK_CMPINT (node->trigger_calls, ==, 1);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 42.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "tick");

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_run_once_sync_repeats (void)
{
    Capture      cap  = { 0 };
    TestTrigger *node = g_object_new (TEST_TYPE_TRIGGER,
                                      "autostart", FALSE, NULL);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));

    PN_CHECK_CMPINT (node->trigger_calls, ==, 3);
    PN_CHECK_CMPINT (cap.count, ==, 3);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_autostart_false_is_quiescent (void)
{
    Capture      cap       = { 0 };
    gboolean     autostart = TRUE;
    TestTrigger *node      = g_object_new (TEST_TYPE_TRIGGER,
                                           "autostart", FALSE, NULL);

    g_signal_connect (node, "message", G_CALLBACK (on_emit), &cap);

    g_object_get (node, "autostart", &autostart, NULL);
    PN_CHECK_FALSE (autostart);

    /* With no worker thread, nothing fires on its own.  Drain any
     * pending main-context sources to catch a stray emission; the
     * worker (were it running) would not fire within this window
     * anyway, so this guards against an accidental construct-time or
     * one-shot emission rather than the full periodic schedule. */
    g_usleep (20 * 1000);
    while (g_main_context_iteration (NULL, FALSE))
        ;

    PN_CHECK_CMPINT (node->trigger_calls, ==, 0);
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_period_clamping (void)
{
    PnAutoTrigger *node = g_object_new (TEST_TYPE_TRIGGER,
                                        "autostart", FALSE, NULL);

    pn_auto_trigger_set_period (node, 0);
    PN_CHECK_CMPINT (pn_auto_trigger_get_period (node), ==, 1);     /* min */

    pn_auto_trigger_set_period (node, 999999);
    PN_CHECK_CMPINT (pn_auto_trigger_get_period (node), ==, 3600);  /* max */

    pn_auto_trigger_set_period (node, 42);
    PN_CHECK_CMPINT (pn_auto_trigger_get_period (node), ==, 42);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-auto-trigger");
    pn_test_add ("run_once_sync_emits",   test_run_once_sync_emits);
    pn_test_add ("run_once_sync_repeats", test_run_once_sync_repeats);
    pn_test_add ("autostart_quiescent",   test_autostart_false_is_quiescent);
    pn_test_add ("period_clamping",       test_period_clamping);
    return pn_test_run ();
}
