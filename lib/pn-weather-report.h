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
#include <json-glib/json-glib.h>

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

/* ------------------------------------------------------------------ */
/*  Display-unit + gradient enums (shared)                             */
/*                                                                     */
/*  The Weather node always reports metric; these select the unit the   */
/*  card converts into.  Published here because both the core (the      */
/*  property pspecs) and the gui-tier painter (which converts the       */
/*  reading) need the enum values.  The GType registrations stay        */
/*  private to the core .c.                                             */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_WR_TEMP_CELSIUS,
    PN_WR_TEMP_FAHRENHEIT,
    PN_WR_TEMP_KELVIN,
} PnWrTempUnit;

typedef enum
{
    PN_WR_WIND_KMH,
    PN_WR_WIND_MS,
    PN_WR_WIND_MPH,
    PN_WR_WIND_KNOTS,
} PnWrWindUnit;

typedef enum
{
    PN_WR_PRESS_HPA,
    PN_WR_PRESS_KPA,
    PN_WR_PRESS_INHG,
    PN_WR_PRESS_MMHG,
} PnWrPressUnit;

typedef enum
{
    PN_WR_GRADIENT_NONE,
    PN_WR_GRADIENT_VERTICAL,
    PN_WR_GRADIENT_HORIZONTAL,
    PN_WR_GRADIENT_DIAGONAL,
} PnWrGradient;

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo/Pango card painter lives in the gui-tier file            */
/*  pn-weather-report-gui.c, but it has to read the same reading        */
/*  snapshot + display config the core fills.  The colours are #PnColor */
/*  (layout-identical to GdkRGBA); the reading is a borrowed            */
/*  #JsonObject* (NULL until the first message), valid for the duration */
/*  of one paint call.                                                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    gboolean      show_details;
    PnWrTempUnit  temp_unit;
    PnWrWindUnit  wind_unit;
    PnWrPressUnit press_unit;

    PnColor       font_color;
    PnColor       secondary_color;
    PnColor       bg_color;
    PnColor       bg_color2;
    PnWrGradient  bg_gradient;
} PnWeatherReportPaintState;

/**
 * pn_weather_report_get_paint_state:
 * @self: weather-report instance
 * @out:  (out): caller-provided snapshot filled with the current display
 *        configuration.
 */
void pn_weather_report_get_paint_state (PnWeatherReport           *self,
                                        PnWeatherReportPaintState *out);

/* The latest reading (borrowed): a deep-copied #JsonObject, or %NULL
 * before the first message.  Owned by the node. */
JsonObject *pn_weather_report_peek_data (PnWeatherReport *self);

G_END_DECLS

#endif /* PN_WEATHER_REPORT_H */
