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

/* Unit tests for PnAutoRandom: a self-firing source that emits a random
 * sample drawn on [min, max] each tick.  The exact value is not
 * predictable, so the tests assert the invariants the sampler
 * guarantees: every draw is clamped into the configured range, a
 * zero-width range collapses to its single value, and the static
 * payload members (output / success) ride along unchanged. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pntimer.h"
#include "pn-auto-random.h"

static void
test_default_range_and_payload (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_RANDOM, NULL);
    PnTimerDriver d;
    gdouble       v;

    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);

    PN_CHECK_CMPINT (d.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str  (d.last, "output"), ==, "AutoRandom sample.");
    PN_CHECK        (pn_test_bool (d.last, "success"));

    /* Default range is [0, 1]. */
    v = pn_test_num (d.last, "value");
    PN_CHECK (v >= 0.0 && v <= 1.0);

    pn_timer_destroy (node, &d);
}

static void
test_value_stays_within_custom_range (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_RANDOM, NULL);
    PnTimerDriver d;
    guint         i;

    pn_timer_attach (node, &d);
    g_object_set (node, "min", 10.0, "max", 20.0, NULL);

    /* Sample several times: every draw must land inside [10, 20]
     * regardless of which one the RNG produces. */
    for (i = 0; i < 8; i++)
    {
        gdouble v;
        pn_timer_kick_wait (node, &d);
        v = pn_test_num (d.last, "value");
        PN_CHECK (v >= 10.0 && v <= 20.0);
    }

    pn_timer_destroy (node, &d);
}

static void
test_zero_width_range_is_constant (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_RANDOM, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    g_object_set (node, "min", 7.0, "max", 7.0, NULL);
    pn_timer_kick_wait (node, &d);

    /* With min == max the only possible sample is that value. */
    PN_CHECK_CMPINT (d.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (d.last, "value"), 7.0, 1e-9);

    pn_timer_destroy (node, &d);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-auto-random");
    pn_test_add ("default_range",          test_default_range_and_payload);
    pn_test_add ("within_custom_range",    test_value_stays_within_custom_range);
    pn_test_add ("zero_width_constant",    test_zero_width_range_is_constant);
    return pn_test_run ();
}
