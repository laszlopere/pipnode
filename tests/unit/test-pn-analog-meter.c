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

/* Unit tests for PnAnalogMeter.  The needle position is on-screen state
 * with no headless getter, so these tests cover the contracts that hold
 * without a display: it is a pure sink (it never forwards a message),
 * its scale/label properties round-trip, and feeding it a non-finite or
 * unconfigured value is harmless. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-analog-meter.h"

static PnNode *
make_node (guint *out_emits)
{
    PnNode *node = PN_NODE (pn_analog_meter_new ());

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
test_is_a_sink (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* Point the meter at data.value, then feed it: the needle moves but
     * nothing is forwarded. */
    g_object_set (node, "key", "value", NULL);
    pn_message_set_double (msg, "value", 10.0);
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_properties_round_trip (void)
{
    PnNode  *node = PN_NODE (pn_analog_meter_new ());
    gdouble  lo = 0.0, hi = 0.0;
    gint     mode = -1;
    gchar   *key = NULL;
    gchar   *unit = NULL;

    g_object_set (node,
                  "min-value", -5.0,
                  "max-value", 42.0,
                  "mode",      PN_ANALOG_METER_MODE_DC,
                  "key",       "data.x",
                  "unit",      "A",
                  NULL);

    g_object_get (node,
                  "min-value", &lo,
                  "max-value", &hi,
                  "mode",      &mode,
                  "key",       &key,
                  "unit",      &unit,
                  NULL);

    PN_CHECK_NEAR   (lo, -5.0, 1e-9);
    PN_CHECK_NEAR   (hi, 42.0, 1e-9);
    PN_CHECK_CMPINT (mode, ==, PN_ANALOG_METER_MODE_DC);
    PN_CHECK_CMPSTR (key,  ==, "data.x");
    PN_CHECK_CMPSTR (unit, ==, "A");

    g_free (key);
    g_free (unit);
    g_object_unref (node);
}

static void
test_non_finite_value_is_ignored (void)
{
    guint          emits;
    PnNode        *node = make_node (&emits);
    PnAnalogMeter *m    = PN_ANALOG_METER (node);

    /* NaN / infinity must be dropped rather than poisoning the needle
     * animation; the call simply returns and emits nothing. */
    pn_analog_meter_set_value (m, NAN);
    pn_analog_meter_set_value (m, INFINITY);
    pn_analog_meter_set_value (m, 1.0);          /* a finite one is fine */

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

static void
test_unconfigured_receive_is_safe (void)
{
    guint      emits;
    PnNode    *node = make_node (&emits);         /* no key configured */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* With no key set there is nothing to read; receiving must be a
     * harmless no-op rather than a crash. */
    pn_message_set_double (msg, "value", 3.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-analog-meter");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("properties_round_trip",  test_properties_round_trip);
    pn_test_add ("non_finite_ignored",     test_non_finite_value_is_ignored);
    pn_test_add ("unconfigured_safe",      test_unconfigured_receive_is_safe);
    return pn_test_run ();
}
