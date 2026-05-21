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

/* Unit tests for PnAutoInjector: a self-firing source that emits its
 * configured value / success / output on every timer tick.  The timer
 * is driven synchronously here via pn_auto_trigger_kick() rather than
 * waited out (see pntimer.h). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pntimer.h"
#include "pn-auto-injector.h"

static void
test_default_payload (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_INJECTOR, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);

    PN_CHECK_CMPINT (d.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str  (d.last, "output"), ==,
                     "AutoInjector activated.");
    PN_CHECK_NEAR   (pn_test_num  (d.last, "value"), 0.0, 1e-9);
    PN_CHECK        (pn_test_bool (d.last, "success"));

    pn_timer_destroy (node, &d);
}

static void
test_custom_payload (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_INJECTOR, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    g_object_set (node,
                  "output",  "tick",
                  "value",   12.5,
                  "success", FALSE,
                  NULL);
    pn_timer_kick_wait (node, &d);

    PN_CHECK_CMPINT (d.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str  (d.last, "output"), ==, "tick");
    PN_CHECK_NEAR   (pn_test_num  (d.last, "value"), 12.5, 1e-9);
    PN_CHECK_FALSE  (pn_test_bool (d.last, "success"));

    pn_timer_destroy (node, &d);
}

static void
test_fires_repeatedly (void)
{
    PnNode       *node = g_object_new (PN_TYPE_AUTO_INJECTOR, NULL);
    PnTimerDriver d;

    /* Each kick is an independent tick, so the same node keeps emitting
     * its payload for as long as the timer runs. */
    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);
    pn_timer_kick_wait (node, &d);
    pn_timer_kick_wait (node, &d);

    PN_CHECK_CMPINT (d.count, ==, 3);

    pn_timer_destroy (node, &d);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-auto-injector");
    pn_test_add ("default_payload",        test_default_payload);
    pn_test_add ("custom_payload",         test_custom_payload);
    pn_test_add ("fires_repeatedly",       test_fires_repeatedly);
    return pn_test_run ();
}
