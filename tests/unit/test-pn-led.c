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

/* Unit tests for PnLed.  The visible "lit" state is driven by a hold
 * timer and only matters on screen, so the headless tests cover the
 * contract that holds without a display: it is a pure sink (it never
 * forwards a message), its hold period is floored so a one-shot flash
 * stays visible, its colour round-trips through the property, and the
 * Mode property defaults to Flash and round-trips its two values. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-led.h"

static void
test_is_a_sink (void)
{
    guint      emits;
    PnNode    *node = PN_NODE (pn_led_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Receiving lights the LED but produces no downstream message. */
    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_hold_ms_floor_and_default (void)
{
    PnNode         *node = PN_NODE (pn_led_new ());
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;
    guint           hold = 0;

    /* The 100 ms floor (so a one-shot flash stays visible on a 60 Hz
     * display) is the property's minimum, with a 250 ms default. */
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), "hold-ms");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum,       ==, 100u);
    PN_CHECK_CMPINT (uspec->default_value, ==, 250u);

    /* A value above the floor round-trips unchanged. */
    g_object_set (node, "hold-ms", 4000u, NULL);
    g_object_get (node, "hold-ms", &hold, NULL);
    PN_CHECK_CMPINT (hold, ==, 4000u);

    g_object_unref (node);
}

static void
test_color_round_trips (void)
{
    PnNode  *node = PN_NODE (pn_led_new ());
    PnColor  in   = { 0.10, 0.20, 0.30, 1.0 };
    PnColor *out  = NULL;

    g_object_set (node, "color", &in, NULL);
    g_object_get (node, "color", &out, NULL);

    PN_CHECK (out != NULL && pn_color_equal (out, &in));

    if (out != NULL)
        pn_color_free (out);
    g_object_unref (node);
}

static void
test_mode_default_and_round_trip (void)
{
    PnNode    *node = PN_NODE (pn_led_new ());
    PnLedMode  mode = PN_LED_MODE_STEADY;

    /* A fresh LED defaults to Flash -- the historical behaviour. */
    g_object_get (node, "mode", &mode, NULL);
    PN_CHECK_CMPINT (mode, ==, PN_LED_MODE_FLASH);

    /* Both values round-trip through the property. */
    g_object_set (node, "mode", PN_LED_MODE_STEADY, NULL);
    g_object_get (node, "mode", &mode, NULL);
    PN_CHECK_CMPINT (mode, ==, PN_LED_MODE_STEADY);

    g_object_set (node, "mode", PN_LED_MODE_FLASH, NULL);
    g_object_get (node, "mode", &mode, NULL);
    PN_CHECK_CMPINT (mode, ==, PN_LED_MODE_FLASH);

    g_object_unref (node);
}

static void
test_steady_is_still_a_sink (void)
{
    guint      emits;
    PnNode    *node = PN_NODE (pn_led_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* Steady mode is a sink too: latching to data.value never forwards
     * a downstream message. */
    g_object_set (node, "mode", PN_LED_MODE_STEADY, NULL);

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    pn_message_set_double (msg, "value", 0.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-led");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("hold_ms_floor_default",  test_hold_ms_floor_and_default);
    pn_test_add ("color_round_trips",      test_color_round_trips);
    pn_test_add ("mode_default_round_trip", test_mode_default_and_round_trip);
    pn_test_add ("steady_is_still_a_sink", test_steady_is_still_a_sink);
    return pn_test_run ();
}
