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

#ifndef PN_ASTRONOMICAL_H
#define PN_ASTRONOMICAL_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAstronomical                                                     */
/*                                                                     */
/*  Auto-trigger source node that emits astronomical data for a named   */
/*  place, recomputed from the current time on every                    */
/*  #PnAutoTrigger:period tick.  The "city" is resolved to coordinates  */
/*  once via the shared pn-geocode lookup (Open-Meteo, HTTP, cached     */
/*  until the city changes); from there every figure is derived in C    */
/*  from the NOAA solar-position equations — no per-tick network.        */
/*                                                                     */
/*  The "datum" property (a combobox) selects which computed quantity   */
/*  drives the canonical data.value; whatever is selected, the message  */
/*  always carries the full set of available fields (sun_up,            */
/*  sun_altitude, sun_azimuth, sunrise, sunset, solar_noon, day_length, */
/*  plus the resolved latitude/longitude and city/country) so           */
/*  downstream nodes can read any of them.  The default datum is        */
/*  "The Sun is up" (1.0 when the Sun is above the horizon, 0.0 below;   */
/*  pick "The Sun is down" for the inverse).                            */
/* ------------------------------------------------------------------ */

/* Which computed quantity drives data.value.  Nicks are serialised to
 * the saved worksheet, so keep them stable. */
typedef enum
{
    PN_ASTRO_DATUM_SUN_IS_UP,     /* value 1.0 when the Sun is up; default  */
    PN_ASTRO_DATUM_SUN_IS_DOWN,   /* value 1.0 when the Sun is down         */
    PN_ASTRO_DATUM_SUN_ALTITUDE,  /* degrees above the horizon */
    PN_ASTRO_DATUM_SUN_AZIMUTH,   /* degrees clockwise from north */
    PN_ASTRO_DATUM_DAY_LENGTH,    /* hours of daylight today */
} PnAstronomicalDatum;

/* Result of pn_astronomical_compute().  Times are Unix epoch seconds
 * (UTC).  has_sunrise / has_sunset are FALSE in the polar day/night
 * cases where the Sun does not cross the horizon on the given day; the
 * matching time members are then undefined and the node omits them. */
typedef struct
{
    gboolean sun_up;       /* Sun above the (refraction-corrected) horizon */
    gdouble  altitude;     /* solar altitude,  degrees (negative = below)  */
    gdouble  azimuth;      /* solar azimuth,   degrees, 0 = N, clockwise   */
    gboolean has_sunrise;
    gdouble  sunrise;      /* Unix seconds (UTC) */
    gboolean has_sunset;
    gdouble  sunset;       /* Unix seconds (UTC) */
    gdouble  solar_noon;   /* Unix seconds (UTC) */
    gdouble  day_length;   /* hours (0 in polar night, 24 in polar day)    */
} PnAstronomicalResult;

/**
 * pn_astronomical_compute:
 * @latitude:  geographic latitude in degrees, +N
 * @longitude: geographic longitude in degrees, +E
 * @unix_time: the instant to evaluate, Unix epoch seconds (UTC)
 * @out:       (out): filled with the solar position + day events
 *
 * Pure function (no I/O, no node state): evaluates the Sun's position
 * for @latitude/@longitude at @unix_time using the NOAA solar-position
 * equations.  Factored out of the node so it can be tested with fixed
 * inputs.
 */
void pn_astronomical_compute (gdouble               latitude,
                              gdouble               longitude,
                              gint64                unix_time,
                              PnAstronomicalResult *out);

#define PN_TYPE_ASTRONOMICAL (pn_astronomical_get_type ())

G_DECLARE_FINAL_TYPE (PnAstronomical, pn_astronomical,
                      PN, ASTRONOMICAL, PnAutoTrigger)

PnAstronomical *pn_astronomical_new (void);

G_END_DECLS

#endif /* PN_ASTRONOMICAL_H */
