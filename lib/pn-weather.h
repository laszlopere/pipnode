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

G_BEGIN_DECLS

/* A source node that polls current weather for a named place from the
 * Open-Meteo API (https://open-meteo.com), which is free and needs no
 * API key.  It is a #PnHttp subclass so it reuses the curl-based fetch
 * loop and the auto-trigger period, but it resolves the city name to
 * coordinates with one extra (cached) geocoding request before each
 * forecast fetch.  This is the first of what may become several weather
 * sources; the network transport is curl-via-PnHttp for now and will be
 * ported to in-process HTTP later (see the "pure C" project note). */

#define PN_TYPE_WEATHER (pn_weather_get_type ())
G_DECLARE_FINAL_TYPE (PnWeather, pn_weather, PN, WEATHER, PnHttp)

PnWeather *pn_weather_new (void);

G_END_DECLS

#endif /* PN_WEATHER_H */
