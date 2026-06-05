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

/* Unit tests for PnSunPath's message-intake contract, read back through
 * the GTK-free snapshot seam (pn_sun_path_get_snapshot).  The cairo scene
 * itself has no headless getter, but the snapshot carries exactly the
 * decision the painter makes — "waiting" vs "no position" vs "draw the
 * Sun" — so the intake rules are pinned here:
 *
 *   - a card with no reading yet stays in the "waiting" state;
 *   - a successful astronomical reading is taken in full;
 *   - a *successful* message with no Sun position (e.g. a Weather report
 *     wired in by mistake) is ignored rather than parking the Sun at
 *     (0, 0) — the bug this test guards against;
 *   - an explicit failure (success = FALSE) still drives "no position";
 *   - it is a pure sink: it never forwards a message. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-sun-path.h"

static PnSunPath *
make_node (guint *out_emits)
{
    PnSunPath *self = pn_sun_path_new ();

    *out_emits = 0;
    g_signal_connect (self, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return self;
}

/* A freshly dropped card has no reading: the painter shows "waiting". */
static void
test_starts_waiting (void)
{
    guint             emits;
    PnSunPath        *self = make_node (&emits);
    PnSunPathSnapshot s;

    pn_sun_path_get_snapshot (self, &s);
    PN_CHECK_FALSE (s.have_data);

    g_object_unref (self);
}

/* A full astronomical reading is taken in: have_data + success set, the
 * position recorded, and the day's arc recomputed from the coordinates. */
static void
test_accepts_astronomical_reading (void)
{
    guint             emits;
    PnSunPath        *self = make_node (&emits);
    PnMessage        *msg  = pn_message_new (NULL, NULL);
    PnSunPathSnapshot s;

    pn_message_set_boolean (msg, "success",      TRUE);
    pn_message_set_boolean (msg, "sun_up",       TRUE);
    pn_message_set_double  (msg, "sun_altitude", 28.0);
    pn_message_set_double  (msg, "sun_azimuth",  85.8);
    pn_message_set_double  (msg, "latitude",     46.08);
    pn_message_set_double  (msg, "longitude",    18.23);
    pn_node_receive_message (PN_NODE (self), msg);

    pn_sun_path_get_snapshot (self, &s);
    PN_CHECK       (s.have_data);
    PN_CHECK       (s.success);
    PN_CHECK       (s.sun_up);
    PN_CHECK_NEAR  (s.sun_altitude, 28.0, 1e-9);
    PN_CHECK_NEAR  (s.sun_azimuth,  85.8, 1e-9);
    PN_CHECK_CMPINT (s.n_path, >, 0);          /* arc was computed */
    PN_CHECK_CMPINT (emits, ==, 0);            /* a sink: nothing forwarded */

    g_object_unref (msg);
    g_object_unref (self);
}

/* The reported bug: a Weather report (success = TRUE, no Sun fields) wired
 * into a Sun Path card must be ignored, not taken as a reading at (0, 0).
 * The card stays in the "waiting" state. */
static void
test_ignores_non_astronomical_message (void)
{
    guint             emits;
    PnSunPath        *self = make_node (&emits);
    PnMessage        *msg  = pn_message_new (NULL, NULL);
    PnSunPathSnapshot s;

    /* A Weather-shaped bag: successful, carries a place and a temperature,
     * but no Sun position. */
    pn_message_set_boolean (msg, "success",     TRUE);
    pn_message_set_double  (msg, "value",       20.0);
    pn_message_set_string  (msg, "city",        "Pécs");
    pn_node_receive_message (PN_NODE (self), msg);

    pn_sun_path_get_snapshot (self, &s);
    PN_CHECK_FALSE  (s.have_data);             /* still waiting, not (0,0) */
    PN_CHECK_NEAR   (s.sun_altitude, 0.0, 1e-9);
    PN_CHECK_NEAR   (s.sun_azimuth,  0.0, 1e-9);
    PN_CHECK_CMPINT (s.n_path, ==, 0);

    g_object_unref (msg);
    g_object_unref (self);
}

/* An explicit failure (unknown city) still passes through: have_data set,
 * success FALSE, so the painter shows the "no position" notice with the
 * error text rather than the "waiting" placeholder. */
static void
test_accepts_failure_reading (void)
{
    guint             emits;
    PnSunPath        *self = make_node (&emits);
    PnMessage        *msg  = pn_message_new (NULL, NULL);
    PnSunPathSnapshot s;

    pn_message_set_boolean (msg, "success", FALSE);
    pn_message_set_string  (msg, "city",    "Nowhereville");
    pn_message_set_string  (msg, "output",  "Could not look up the location.");
    pn_node_receive_message (PN_NODE (self), msg);

    pn_sun_path_get_snapshot (self, &s);
    PN_CHECK       (s.have_data);
    PN_CHECK_FALSE (s.success);
    PN_CHECK_CMPINT (s.n_path, ==, 0);         /* no stale arc under the notice */

    g_object_unref (msg);
    g_object_unref (self);
}

/* A stray Weather message arriving after a good reading must not clobber
 * the live Sun position the card is already showing. */
static void
test_non_astronomical_does_not_clobber (void)
{
    guint             emits;
    PnSunPath        *self = make_node (&emits);
    PnMessage        *good = pn_message_new (NULL, NULL);
    PnMessage        *weather = pn_message_new (NULL, NULL);
    PnSunPathSnapshot s;

    pn_message_set_boolean (good, "success",      TRUE);
    pn_message_set_boolean (good, "sun_up",       TRUE);
    pn_message_set_double  (good, "sun_altitude", 28.0);
    pn_message_set_double  (good, "sun_azimuth",  85.8);
    pn_message_set_double  (good, "latitude",     46.08);
    pn_message_set_double  (good, "longitude",    18.23);
    pn_node_receive_message (PN_NODE (self), good);

    pn_message_set_boolean (weather, "success", TRUE);
    pn_message_set_double  (weather, "value",   20.0);
    pn_node_receive_message (PN_NODE (self), weather);

    pn_sun_path_get_snapshot (self, &s);
    PN_CHECK      (s.have_data);
    PN_CHECK      (s.success);
    PN_CHECK_NEAR (s.sun_altitude, 28.0, 1e-9);
    PN_CHECK_NEAR (s.sun_azimuth,  85.8, 1e-9);

    g_object_unref (good);
    g_object_unref (weather);
    g_object_unref (self);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-sun-path");
    pn_test_add ("starts_waiting",          test_starts_waiting);
    pn_test_add ("accepts_astronomical",    test_accepts_astronomical_reading);
    pn_test_add ("ignores_non_astro",       test_ignores_non_astronomical_message);
    pn_test_add ("accepts_failure",         test_accepts_failure_reading);
    pn_test_add ("non_astro_no_clobber",    test_non_astronomical_does_not_clobber);
    return pn_test_run ();
}
