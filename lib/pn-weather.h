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

#ifndef PN_WEATHER_H
#define PN_WEATHER_H

#include "pn-http.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* A source node that polls current weather for a named place.  It is a
 * #PnHttp subclass so it reuses the curl-based fetch loop and the
 * auto-trigger period, but it resolves the city name to coordinates with
 * one extra (cached) geocoding request before each forecast fetch.
 *
 * Three free, key-less forecast providers are selectable via the
 * "provider" property:
 *   - Open-Meteo (https://open-meteo.com) — global coverage; the default.
 *   - Bright Sky (https://brightsky.dev)  — Germany and parts of central
 *     Europe only, served from DWD open data.
 *   - MET Norway (https://api.met.no)     — global coverage, served by the
 *     Norwegian Meteorological Institute's Locationforecast API.
 * The city is geocoded through Open-Meteo's geocoding endpoint for all
 * three (it is global); only the forecast fetch differs by provider.  The
 * network transport is curl-via-PnHttp for now and will be ported to
 * in-process HTTP later (see the "pure C" project note). */

#define PN_TYPE_WEATHER (pn_weather_get_type ())
G_DECLARE_FINAL_TYPE (PnWeather, pn_weather, PN, WEATHER, PnHttp)

/* Which forecast provider the node fetches from.  Serialised by enum
 * nick (see lib/pn-flow.c), so the nicks are part of the file format. */
typedef enum
{
    PN_WEATHER_PROVIDER_OPEN_METEO,   /* global; default                   */
    PN_WEATHER_PROVIDER_BRIGHT_SKY,   /* DWD; Germany / central Europe     */
    PN_WEATHER_PROVIDER_MET_NO,       /* MET Norway; global                */
} PnWeatherProvider;

PnWeather *pn_weather_new (void);

/* ------------------------------------------------------------------ */
/*  Pure parse seam (no I/O, no GTK)                                    */
/*                                                                     */
/*  The values extracted from one forecast response, normalised across */
/*  providers (metric units throughout).  These helpers are exposed     */
/*  (non-static) purely so the headless unit tests can drive the JSON   */
/*  parse on canned bodies without a network fetch; the node remains    */
/*  the only production caller.                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    gboolean  ok;             /* a finite current temperature was found */
    gdouble   temperature;    /* °C; valid iff @ok                       */
    gboolean  has_humidity;
    gdouble   humidity;       /* %RH                                     */
    gboolean  has_wind;
    gdouble   wind_speed;     /* km/h                                    */
    gboolean  has_code;
    gint      weather_code;   /* WMO code (Open-Meteo only)              */
    gchar    *description;    /* conditions label, or NULL; owned.  Set  */
                              /* by providers that report no WMO code    */
                              /* (Bright Sky); for Open-Meteo the caller */
                              /* derives it from @weather_code instead.  */
    gchar    *reason;         /* provider error reason, or NULL; owned   */
} PnWeatherCurrent;

/* Extract the "current" reading (and any provider "reason" error) from
 * a parsed Open-Meteo response object @root (may be %NULL).  Fills
 * @out (zero it first) and returns @out->ok.  The caller frees
 * @out->reason and @out->description. */
gboolean     pn_weather_parse_current   (JsonObject       *root,
                                         PnWeatherCurrent *out);

/* The Bright Sky counterpart: extract the current reading from a parsed
 * Bright Sky /current_weather response object @root (may be %NULL).
 * Fills @out (zero it first) with metric values and a conditions
 * @description (Bright Sky reports no WMO code), and returns @out->ok.
 * A Bright Sky error body ({"detail":...}) is surfaced in @out->reason.
 * The caller frees @out->reason and @out->description. */
gboolean     pn_weather_parse_bright_sky_current (JsonObject       *root,
                                                  PnWeatherCurrent *out);

/* The MET Norway counterpart: extract the current reading from a parsed
 * Locationforecast /compact response object @root (may be %NULL).  The
 * "current" reading is the first entry of properties.timeseries; its
 * instant block carries the temperature, humidity and wind, and the
 * conditions @description is derived from the next_1_hours symbol_code.
 * met.no reports wind in m/s, which this normalises to km/h to match the
 * other providers.  Fills @out (zero it first) and returns @out->ok.  The
 * caller frees @out->reason and @out->description. */
gboolean     pn_weather_parse_met_no_current (JsonObject       *root,
                                              PnWeatherCurrent *out);

/* Trim a MET Norway /compact response @root (a JSON node) down to the slice
 * the node consumes: a deep copy with properties.timeseries truncated to its
 * first entry (the current-hour nowcast), with meta/geometry kept intact.
 * met.no always returns a full ~10-day forecast that nothing downstream
 * reads, so the message's "raw" member carries this trimmed copy rather than
 * the whole ~37 KB response.  Returns a newly allocated node the caller
 * owns; never %NULL for a valid @root. */
JsonNode    *pn_weather_met_no_trim_raw (JsonNode *root);

/* Map a WMO weather code (as Open-Meteo reports it) to a short English
 * label; unknown codes yield a generic string. */
const gchar *pn_weather_code_description (gint code);

/* Map a Bright Sky @icon (e.g. "partly-cloudy-day") to a short English
 * label, falling back to @condition (e.g. "rain") and then a generic
 * string; both arguments may be %NULL.  Returns a static string. */
const gchar *pn_weather_bright_sky_description (const gchar *icon,
                                                const gchar *condition);

/* Map a MET Norway @symbol_code (e.g. "partlycloudy_day", "lightrain",
 * "heavysnowshowers_night") to a short English label.  The day/night/
 * polartwilight variant suffix is ignored and the base condition is
 * matched; unknown codes yield a generic string.  @symbol_code may be
 * %NULL.  Returns a static string. */
const gchar *pn_weather_met_no_description (const gchar *symbol_code);

G_END_DECLS

#endif /* PN_WEATHER_H */
