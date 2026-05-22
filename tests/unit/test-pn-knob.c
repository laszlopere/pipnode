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

/* Unit tests for PnKnob: a rotary value source.  pn_knob_set_value()
 * clamps to [min, max] and updates the value silently; pn_knob_scroll()
 * moves the value by a fixed fraction of the range per detent and emits
 * a message carrying the new data.value.  Scrolling against an end stop
 * (or across a zero-width range) emits nothing. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-knob.h"

/* One scroll detent moves the value by (max - min) / 24. */
#define KNOB_TICKS  24.0

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

static PnKnob *
make_node (Capture *cap)
{
    PnKnob *node = pn_knob_new ();

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

static void
test_set_value_clamps_without_emitting (void)
{
    Capture cap;
    PnKnob *node = make_node (&cap);     /* default range [0, 1] */

    pn_knob_set_value (node, 2.0);       /* above max -> clamps to 1 */
    PN_CHECK_NEAR (pn_knob_get_value (node), 1.0, 1e-9);

    pn_knob_set_value (node, -3.0);      /* below min -> clamps to 0 */
    PN_CHECK_NEAR (pn_knob_get_value (node), 0.0, 1e-9);

    /* set_value is the programmatic path and never emits; only a user
     * scroll does. */
    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_scroll_steps_and_emits (void)
{
    Capture cap;
    PnKnob *node = make_node (&cap);     /* default range [0, 1], value 0 */

    /* Wheel up (dy < 0) raises the value by one detent and emits the
     * new value. */
    pn_knob_scroll (node, -1.0);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_knob_get_value (node),         1.0 / KNOB_TICKS, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"),  1.0 / KNOB_TICKS, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_scroll_at_end_stop_is_silent (void)
{
    Capture cap;
    PnKnob *node = make_node (&cap);

    /* A single big notch saturates the value at max (a full range of
     * travel), emitting once. */
    pn_knob_scroll (node, -KNOB_TICKS);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_knob_get_value (node), 1.0, 1e-9);

    /* Already parked at the end stop: another up-scroll changes nothing
     * and emits nothing. */
    pn_knob_scroll (node, -1.0);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_knob_get_value (node), 1.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_announces_value_once_on_startup (void)
{
    Capture cap;
    PnKnob *node = make_node (&cap);     /* default range [0, 1], value 0 */

    /* A knob announces its current position once shortly after it is
     * constructed, so a freshly-loaded worksheet lets downstream nodes
     * learn the knob's state without a manual turn.  The announce is a
     * one-shot main-loop idle scheduled at construction; set_value is
     * silent, so nothing has fired yet. */
    pn_knob_set_value (node, 0.5);
    PN_CHECK_CMPINT (cap.count, ==, 0);

    /* Pump the default context until the idle fires. */
    while (cap.count == 0 && g_main_context_iteration (NULL, TRUE))
        ;

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 0.5, 1e-9);

    /* One-shot: no recurring emit like a periodic data source would. */
    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (cap.count, ==, 1);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_zero_width_range_does_not_rotate (void)
{
    Capture cap;
    PnKnob *node = make_node (&cap);

    g_object_set (node, "min", 5.0, "max", 5.0, NULL);
    pn_knob_scroll (node, -1.0);

    PN_CHECK_CMPINT (cap.count, ==, 0);
    PN_CHECK_NEAR   (pn_knob_get_value (node), 5.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-knob");
    pn_test_add ("set_value_clamps",       test_set_value_clamps_without_emitting);
    pn_test_add ("scroll_steps_emits",     test_scroll_steps_and_emits);
    pn_test_add ("scroll_end_stop_silent", test_scroll_at_end_stop_is_silent);
    pn_test_add ("startup_announce",       test_announces_value_once_on_startup);
    pn_test_add ("zero_width_no_rotate",   test_zero_width_range_does_not_rotate);
    return pn_test_run ();
}
