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

/* Unit tests for PnInject: on activation, emit a message carrying the
 * configured text on data.output and the configured flag on
 * data.success.  An empty text leaves the node unconfigured and it
 * refuses to fire. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-inject.h"

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

static PnInject *
make_node (Capture *cap)
{
    PnInject *node = pn_inject_new ();

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

static void
test_fires_default_payload (void)
{
    Capture   cap;
    PnInject *node = make_node (&cap);

    /* A freshly-constructed node ships with a friendly default text and
     * success == TRUE, so it is immediately fireable. */
    pn_inject_fire (node);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "output"), ==,
                     "Injector activated.");
    PN_CHECK        (pn_test_bool (cap.last, "success"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_fires_custom_text_and_flag (void)
{
    Capture   cap;
    PnInject *node = make_node (&cap);

    g_object_set (node, "text", "boom", "success", FALSE, NULL);
    pn_inject_fire (node);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str  (cap.last, "output"), ==, "boom");
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_empty_text_refuses_to_fire (void)
{
    Capture   cap;
    PnInject *node = make_node (&cap);

    g_object_set (node, "text", "", NULL);
    pn_inject_fire (node);

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-inject");
    pn_test_add ("fires_default",          test_fires_default_payload);
    pn_test_add ("fires_custom",           test_fires_custom_text_and_flag);
    pn_test_add ("empty_refuses",          test_empty_text_refuses_to_fire);
    return pn_test_run ();
}
