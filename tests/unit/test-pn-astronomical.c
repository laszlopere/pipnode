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

/* Unit tests for PnAstronomical's pure solar-position seam,
 * pn_astronomical_compute().  The node's city -> coordinates step does
 * network I/O and is exercised live in the app; the maths below is the
 * part with a deterministic contract, so it is pinned here against
 * known mid-latitude and polar cases with fixed inputs. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-astronomical.h"

/* Berlin, to a few decimals. */
#define BERLIN_LAT  52.5200
#define BERLIN_LON  13.4050

static gint64
utc (gint year, gint month, gint day, gint hour, gint minute, gint second)
{
    GDateTime *dt = g_date_time_new_utc (year, month, day,
                                         hour, minute, second);
    gint64     t  = g_date_time_to_unix (dt);

    g_date_time_unref (dt);
    return t;
}

/* Summer solstice midday over Berlin: Sun high, a long day, the events
 * ordered sunrise < noon < sunset. */
static void
test_berlin_summer_noon (void)
{
    PnAstronomicalResult r = { 0 };

    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON,
                             utc (2024, 6, 21, 12, 0, 0), &r);

    PN_CHECK       (r.sun_up);
    PN_CHECK       (r.altitude > 50.0);
    PN_CHECK       (r.azimuth >= 0.0 && r.azimuth < 360.0);
    PN_CHECK       (r.azimuth > 150.0 && r.azimuth < 220.0);  /* ~south */
    PN_CHECK       (r.has_sunrise && r.has_sunset);
    PN_CHECK       (r.day_length > 16.0 && r.day_length < 17.5);
    PN_CHECK       (r.sunrise < r.solar_noon);
    PN_CHECK       (r.solar_noon < r.sunset);
}

/* Winter solstice, small hours over Berlin: Sun below the horizon and a
 * short day. */
static void
test_berlin_winter_night (void)
{
    PnAstronomicalResult r = { 0 };

    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON,
                             utc (2024, 12, 21, 2, 0, 0), &r);

    PN_CHECK_FALSE (r.sun_up);
    PN_CHECK       (r.altitude < 0.0);
    PN_CHECK       (r.has_sunrise && r.has_sunset);
    PN_CHECK       (r.day_length > 7.0 && r.day_length < 9.0);
}

/* High Arctic at the summer solstice: the Sun never sets. */
static void
test_polar_day (void)
{
    PnAstronomicalResult r = { 0 };

    pn_astronomical_compute (80.0, 0.0,
                             utc (2024, 6, 21, 12, 0, 0), &r);

    PN_CHECK       (r.sun_up);
    PN_CHECK       (r.altitude > 0.0);
    PN_CHECK_FALSE (r.has_sunrise);
    PN_CHECK_FALSE (r.has_sunset);
    PN_CHECK_NEAR  (r.day_length, 24.0, 1e-9);
}

/* High Arctic at the winter solstice: the Sun never rises. */
static void
test_polar_night (void)
{
    PnAstronomicalResult r = { 0 };

    pn_astronomical_compute (80.0, 0.0,
                             utc (2024, 12, 21, 12, 0, 0), &r);

    PN_CHECK_FALSE (r.sun_up);
    PN_CHECK       (r.altitude < 0.0);
    PN_CHECK_FALSE (r.has_sunrise);
    PN_CHECK_FALSE (r.has_sunset);
    PN_CHECK_NEAR  (r.day_length, 0.0, 1e-9);
}

/* Solar noon is, by construction, when the Sun is highest: the altitude
 * sampled at the reported solar_noon must beat the altitude an hour to
 * either side. */
static void
test_solar_noon_is_peak (void)
{
    PnAstronomicalResult noon = { 0 }, before = { 0 }, after = { 0 };
    gint64               t = utc (2024, 3, 20, 9, 0, 0);  /* equinox-ish */

    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON, t, &noon);
    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON,
                             (gint64) noon.solar_noon, &noon);
    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON,
                             (gint64) noon.solar_noon - 3600, &before);
    pn_astronomical_compute (BERLIN_LAT, BERLIN_LON,
                             (gint64) noon.solar_noon + 3600, &after);

    PN_CHECK (noon.altitude > before.altitude);
    PN_CHECK (noon.altitude > after.altitude);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-astronomical");
    pn_test_add ("berlin_summer_noon",  test_berlin_summer_noon);
    pn_test_add ("berlin_winter_night", test_berlin_winter_night);
    pn_test_add ("polar_day",           test_polar_day);
    pn_test_add ("polar_night",         test_polar_night);
    pn_test_add ("solar_noon_is_peak",  test_solar_noon_is_peak);
    return pn_test_run ();
}
