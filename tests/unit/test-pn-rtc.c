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

/* Unit tests for PnRtc (Clock): a self-firing source that emits the
 * current local time broken into fields on every tick.  The exact
 * instant is not predictable, so the tests assert the message shape and
 * that each field sits in its valid calendar range. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pntimer.h"
#include "pn-rtc.h"

static void
test_emits_time_fields (void)
{
    PnNode       *node = g_object_new (PN_TYPE_RTC, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);

    PN_CHECK_CMPINT (d.count, ==, 1);

    /* Every documented member is present. */
    PN_CHECK (pn_test_has (d.last, "epoch"));
    PN_CHECK (pn_test_has (d.last, "year"));
    PN_CHECK (pn_test_has (d.last, "month"));
    PN_CHECK (pn_test_has (d.last, "dom"));
    PN_CHECK (pn_test_has (d.last, "hour"));
    PN_CHECK (pn_test_has (d.last, "minute"));
    PN_CHECK (pn_test_has (d.last, "second"));

    pn_timer_destroy (node, &d);
}

static void
test_fields_in_valid_ranges (void)
{
    PnNode       *node = g_object_new (PN_TYPE_RTC, NULL);
    PnTimerDriver d;

    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);

    /* Calendar bounds.  The year check is a loose sanity floor — the
     * clock reads the system's wall time, which is well past 2024. */
    PN_CHECK (pn_test_num (d.last, "year")   >= 2024.0);
    PN_CHECK (pn_test_num (d.last, "month")  >= 1.0 &&
              pn_test_num (d.last, "month")  <= 12.0);
    PN_CHECK (pn_test_num (d.last, "dom")    >= 1.0 &&
              pn_test_num (d.last, "dom")    <= 31.0);
    PN_CHECK (pn_test_num (d.last, "hour")   >= 0.0 &&
              pn_test_num (d.last, "hour")   <= 23.0);
    PN_CHECK (pn_test_num (d.last, "minute") >= 0.0 &&
              pn_test_num (d.last, "minute") <= 59.0);
    PN_CHECK (pn_test_num (d.last, "second") >= 0.0 &&
              pn_test_num (d.last, "second") <= 60.0);   /* allow leap */
    PN_CHECK (pn_test_num (d.last, "epoch")  > 0.0);

    pn_timer_destroy (node, &d);
}

static void
test_convenience_members (void)
{
    PnNode       *node = g_object_new (PN_TYPE_RTC, NULL);
    PnTimerDriver d;
    const gchar  *output;

    pn_timer_attach (node, &d);
    pn_timer_kick_wait (node, &d);

    /* success lets an Edge chain off the tick; output is a ready-made
     * sentence for a TTS sink. */
    PN_CHECK (pn_test_bool (d.last, "success"));
    output = pn_test_str (d.last, "output");
    PN_CHECK (output != NULL && g_str_has_prefix (output, "The time is "));

    /* The spoken sentence and the numeric fields come from a single
     * timestamp snapshot, so the "HH:MM:SS." it renders must match the
     * hour/minute/second members exactly — no rollover between them. */
    if (output != NULL)
    {
        gchar *expected = g_strdup_printf (
                "The time is %02d:%02d:%02d.",
                (int) pn_test_num (d.last, "hour"),
                (int) pn_test_num (d.last, "minute"),
                (int) pn_test_num (d.last, "second"));
        PN_CHECK_CMPSTR (output, ==, expected);
        g_free (expected);
    }

    pn_timer_destroy (node, &d);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-rtc");
    pn_test_add ("emits_time_fields",      test_emits_time_fields);
    pn_test_add ("fields_in_range",        test_fields_in_valid_ranges);
    pn_test_add ("convenience_members",    test_convenience_members);
    return pn_test_run ();
}
