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

#ifndef PN_WEATHER_REPORT_H
#define PN_WEATHER_REPORT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWeatherReport                                                    */
/*                                                                     */
/*  Sink node that renders the message emitted by a #PnWeather node as  */
/*  a compact, mostly-monochrome weather card.  Like #PnGraph, the      */
/*  header is drawn in the standard Node-RED style and a white plot     */
/*  rectangle of the same dimensions sits below it with a small gap;    */
/*  instead of a PLplot graph the rectangle is painted with cairo +     */
/*  Pango — FontAwesome glyphs and plain text — to lay out the place,   */
/*  the current conditions, the temperature, and a row of detail tiles  */
/*  (humidity, wind, pressure, cloud cover).                            */
/*                                                                     */
/*  It reads the named members the Weather node promotes (city,         */
/*  country, temperature, humidity, wind_speed, weather_code,           */
/*  description, success, output) and dips into the raw Open-Meteo       */
/*  passthrough at data/raw/current for the extra fields                 */
/*  (apparent_temperature, is_day, cloud_cover, pressure_msl,           */
/*  precipitation, wind_direction_10m, time) it surfaces.               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WEATHER_REPORT (pn_weather_report_get_type ())

G_DECLARE_FINAL_TYPE (PnWeatherReport, pn_weather_report,
                      PN, WEATHER_REPORT, PnNode)

PnWeatherReport *pn_weather_report_new (void);

G_END_DECLS

#endif /* PN_WEATHER_REPORT_H */
