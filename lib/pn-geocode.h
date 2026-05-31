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

#ifndef PN_GEOCODE_H
#define PN_GEOCODE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  pn-geocode                                                         */
/*                                                                     */
/*  Shared city-name -> coordinates lookup against Open-Meteo's free,  */
/*  key-less (global) geocoding endpoint.  Pulled out of #PnWeather so  */
/*  any node that needs to turn a place name into a latitude/longitude */
/*  (Weather, Astronomical, ...) resolves it the same way.  The HTTP    */
/*  transport is curl spawned synchronously, matching the rest of the  */
/*  #PnHttp family; TODO #38.1 will swap curl for an in-process client  */
/*  here in one place.                                                  */
/* ------------------------------------------------------------------ */

/* Outcome of a geocoding lookup.  On success @latitude/@longitude are
 * finite and @name/@country carry the canonical place (either may be
 * %NULL); on failure @error holds a human-readable reason.  Clear with
 * pn_geocode_result_clear(). */
typedef struct
{
    gboolean  resolved;
    gdouble   latitude;
    gdouble   longitude;
    gchar    *name;       /* canonical resolved name; may be NULL  */
    gchar    *country;    /* resolved country;        may be NULL  */
    gchar    *timezone;   /* IANA tz name, e.g. "Europe/Berlin";   */
                          /* may be NULL                           */
    gchar    *error;      /* failure reason when !resolved         */
} PnGeocodeResult;

/**
 * pn_geocode_parse:
 * @root:  the parsed Open-Meteo geocoding response object (may be %NULL)
 * @city:  the queried place name, used only to phrase a not-found error
 * @out:   (out caller-allocates): filled with the first hit or an error
 *
 * Pure function (no I/O): maps an already-parsed geocoding response body
 * to a #PnGeocodeResult.  An absent/empty "results" array is Open-Meteo's
 * "place not found" answer.  Returns @out->resolved.
 */
gboolean pn_geocode_parse (JsonObject      *root,
                           const gchar     *city,
                           PnGeocodeResult *out);

/**
 * pn_geocode_resolve_sync:
 * @city:            place name to resolve (non-empty)
 * @timeout_seconds: curl --max-time for the request
 * @out:             (out caller-allocates): filled with the result
 *
 * Blocking lookup: spawns curl against the Open-Meteo geocoding endpoint
 * for @city, then defers to pn_geocode_parse().  Safe to call from a
 * worker thread (does no GTK / main-loop work).  Returns @out->resolved.
 */
gboolean pn_geocode_resolve_sync (const gchar     *city,
                                  guint            timeout_seconds,
                                  PnGeocodeResult *out);

/* Frees the heap members of @out and zeroes it; safe on a zeroed struct. */
void pn_geocode_result_clear (PnGeocodeResult *out);

G_END_DECLS

#endif /* PN_GEOCODE_H */
