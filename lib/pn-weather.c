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

/* ------------------------------------------------------------------ */
/*  PnWeather                                                          */
/*                                                                     */
/*  Polls current weather for a named place from one of three free,    */
/*  key-less providers, chosen with the "provider" property:           */
/*    - Open-Meteo (default) — global coverage.                        */
/*    - Bright Sky           — DWD open data; Germany / central Europe. */
/*    - MET Norway           — global coverage; Norwegian Met Institute. */
/*  Both forecast endpoints are keyed on latitude/longitude, so a city  */
/*  name first has to be resolved to coordinates through Open-Meteo's   */
/*  (global) geocoding endpoint.  That is two HTTP requests, which does  */
/*  not fit #PnHttp's one-curl-per-tick build_command/emit_message      */
/*  contract, so the trigger is overridden: it geocodes (caching the    */
/*  result until the city changes) and then delegates to #PnHttp's own  */
/*  trigger for the forecast fetch, reusing the curl spawn loop via     */
/*  build_command()/emit_message().                                     */
/*                                                                     */
/*  Emitted message (data bag):                                        */
/*    success      - whether a current reading was obtained            */
/*    value        - current temperature in °C (the canonical value)   */
/*    temperature  - same as value, explicitly named                   */
/*    humidity     - relative humidity, %                              */
/*    wind_speed   - wind speed at 10 m, km/h                          */
/*    weather_code - WMO weather code (integer; Open-Meteo only)        */
/*    description  - human label for the current conditions            */
/*    city/country - the resolved place                                */
/*    latitude/longitude - the resolved coordinates                    */
/*    output       - one-line human summary                            */
/*    raw          - the provider's full JSON response verbatim, so a   */
/*                   downstream Query / JMESPath / JSON Path node can    */
/*                   read any field not promoted to a member above      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-weather.h"
#include "pn-message.h"
#include "pn-geocode.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

/* fa-cloud U+F0C2 — a plain cloud reads as "weather". */
#define PN_WEATHER_ICON            "\xef\x83\x82"

#define PN_WEATHER_FORECAST_URL    "https://api.open-meteo.com/v1/forecast"
#define PN_WEATHER_BRIGHTSKY_URL   "https://api.brightsky.dev/current_weather"
#define PN_WEATHER_MET_NO_URL \
    "https://api.met.no/weatherapi/locationforecast/2.0/compact"

/* MET Norway rejects requests with a default or empty User-Agent and asks
 * callers to identify the application with contact info, so every met.no
 * fetch carries this descriptive UA.  (Open-Meteo and Bright Sky ignore
 * it, so it is sent only for met.no.)  Built from the package identity so
 * it tracks the version automatically. */
#define PN_WEATHER_MET_NO_USER_AGENT \
    PACKAGE_NAME "/" PACKAGE_VERSION \
    " (https://github.com/laszlopere/pipnode; " PACKAGE_BUGREPORT ")"

/* The "provider" property, registered as an enum so the settings dialog
 * renders it as a combobox and it serialises by nick. */
#define PN_TYPE_WEATHER_PROVIDER   (pn_weather_provider_get_type ())

static GType
pn_weather_provider_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_WEATHER_PROVIDER_OPEN_METEO, "PN_WEATHER_PROVIDER_OPEN_METEO",
              "Open-Meteo (global)"                                            },
            { PN_WEATHER_PROVIDER_BRIGHT_SKY, "PN_WEATHER_PROVIDER_BRIGHT_SKY",
              "Bright Sky (Germany / central Europe)"                          },
            { PN_WEATHER_PROVIDER_MET_NO, "PN_WEATHER_PROVIDER_MET_NO",
              "MET Norway (global)"                                            },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnWeatherProvider", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

/* The "current" block we ask Open-Meteo for.  A handful of these are
 * promoted to named message members in pn_weather_emit_message(); the
 * rest ride along untouched in the "raw" passthrough, so a generous
 * request makes that passthrough worth navigating with a downstream
 * Query / JMESPath / JSON Path node. */
#define PN_WEATHER_CURRENT_VARS \
    "temperature_2m,relative_humidity_2m,apparent_temperature,is_day," \
    "precipitation,rain,showers,snowfall,weather_code,cloud_cover," \
    "pressure_msl,surface_pressure,wind_speed_10m,wind_direction_10m," \
    "wind_gusts_10m"

/* Weather moves slowly and Open-Meteo updates roughly hourly, so a
 * 10-minute cadence is plenty and stays well clear of the free tier's
 * rate limits. */
#define PN_WEATHER_DEFAULT_PERIOD  600u

struct _PnWeather
{
    PnHttp parent_instance;

    /* @mutex guards every field below: @city is written by the main
     * thread (property setter) and read by the worker; the cached
     * geocoding result is written and read by the worker. */
    GMutex   mutex;

    gchar   *city;          /* the configured place name (property)     */
    PnWeatherProvider provider; /* which forecast provider (property)   */

    /* Cached geocoding result.  @geo_key is the city string the cached
     * coordinates belong to; when it differs from @city (or @have_coords
     * is FALSE) the next tick re-geocodes. */
    gchar   *geo_key;
    gboolean have_coords;
    gdouble  latitude;
    gdouble  longitude;
    gchar   *resolved_name; /* canonical place name from the geocoder    */
    gchar   *country;

    /* When the last geocoding attempt failed, a human-readable reason
     * for that failure; NULL while coordinates are usable.  The trigger
     * surfaces this so the user learns whether the lookup was rate-
     * limited, broken by a transport error, or really "no such place",
     * rather than every failure flattening to one generic message. */
    gchar   *geo_error;
};

G_DEFINE_TYPE (PnWeather, pn_weather, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_CITY,
    PROP_PROVIDER,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static gchar *
weather_dup_city_locked (PnWeather *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->city);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static PnWeatherProvider
weather_get_provider_locked (PnWeather *self)
{
    PnWeatherProvider p;

    g_mutex_lock (&self->mutex);
    p = self->provider;
    g_mutex_unlock (&self->mutex);

    return p;
}

/* Borrow a string member that may be absent or JSON null; %NULL when not
 * a present string.  The result points into @obj's parser and is valid
 * only while it lives. */
static const gchar *
obj_string (JsonObject *obj, const gchar *key)
{
    if (obj != NULL && json_object_has_member (obj, key))
    {
        JsonNode *n = json_object_get_member (obj, key);
        if (JSON_NODE_HOLDS_VALUE (n) &&
            json_node_get_value_type (n) == G_TYPE_STRING)
            return json_node_get_string (n);
    }
    return NULL;
}

/* Read a JSON number that may be encoded as either a double or an
 * integer (Open-Meteo reports humidity / weather_code as integers but
 * temperature / wind speed as doubles).  Returns @def when the member
 * is absent or not a number. */
static gdouble
node_number (JsonNode *n, gdouble def)
{
    GType t;

    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return def;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double (n);
    if (t == G_TYPE_INT64)
        return (gdouble) json_node_get_int (n);
    return def;
}

static gdouble
obj_number (JsonObject *obj, const gchar *key, gdouble def)
{
    return obj != NULL && json_object_has_member (obj, key)
           ? node_number (json_object_get_member (obj, key), def)
           : def;
}

/* Map a WMO weather code (as Open-Meteo reports it) to a short English
 * label.  Unknown codes fall through to a generic string. */
const gchar *
pn_weather_code_description (gint code)
{
    switch (code)
    {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Depositing rime fog";
    case 51: return "Light drizzle";
    case 53: return "Moderate drizzle";
    case 55: return "Dense drizzle";
    case 56: return "Light freezing drizzle";
    case 57: return "Dense freezing drizzle";
    case 61: return "Slight rain";
    case 63: return "Moderate rain";
    case 65: return "Heavy rain";
    case 66: return "Light freezing rain";
    case 67: return "Heavy freezing rain";
    case 71: return "Slight snowfall";
    case 73: return "Moderate snowfall";
    case 75: return "Heavy snowfall";
    case 77: return "Snow grains";
    case 80: return "Slight rain showers";
    case 81: return "Moderate rain showers";
    case 82: return "Violent rain showers";
    case 85: return "Slight snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: return "Thunderstorm with slight hail";
    case 99: return "Thunderstorm with heavy hail";
    default: return "Unknown conditions";
    }
}

/* Extract the "current" reading (and any provider error "reason") from a
 * parsed Open-Meteo response object.  Pure: no I/O, no node state — the
 * caller supplies the already-parsed root and frees @out->reason. */
gboolean
pn_weather_parse_current (JsonObject *root, PnWeatherCurrent *out)
{
    JsonNode *cn;

    g_return_val_if_fail (out != NULL, FALSE);

    if (root == NULL)
        return FALSE;

    /* Open-Meteo reports problems as {"error":true,"reason":...}. */
    if (json_object_has_member (root, "reason"))
    {
        JsonNode *rn = json_object_get_member (root, "reason");
        if (JSON_NODE_HOLDS_VALUE (rn) &&
            json_node_get_value_type (rn) == G_TYPE_STRING)
            out->reason = g_strdup (json_node_get_string (rn));
    }

    if (!json_object_has_member (root, "current"))
        return out->ok;

    cn = json_object_get_member (root, "current");
    if (JSON_NODE_HOLDS_OBJECT (cn))
    {
        JsonObject *cur  = json_node_get_object (cn);
        gdouble     temp = obj_number (cur, "temperature_2m",       (gdouble) NAN);
        gdouble     hum  = obj_number (cur, "relative_humidity_2m", (gdouble) NAN);
        gdouble     wind = obj_number (cur, "wind_speed_10m",       (gdouble) NAN);
        gdouble     code = obj_number (cur, "weather_code",         (gdouble) NAN);

        /* A finite temperature is the marker of a usable reading. */
        if (isfinite (temp))
        {
            out->ok          = TRUE;
            out->temperature = temp;
            if (isfinite (hum))  { out->has_humidity = TRUE; out->humidity     = hum;  }
            if (isfinite (wind)) { out->has_wind     = TRUE; out->wind_speed   = wind; }
            if (isfinite (code)) { out->has_code     = TRUE; out->weather_code = (gint) code; }
        }
    }

    return out->ok;
}

/* Map a Bright Sky "icon" (the richer of its two condition fields) to a
 * short English label, preferring the sky-state icon and falling back to
 * the precipitation-type "condition" and then a generic string.  Bright
 * Sky reports no WMO code, so this is how the conditions text is derived.
 * Both arguments may be %NULL. */
const gchar *
pn_weather_bright_sky_description (const gchar *icon, const gchar *condition)
{
    if (icon != NULL)
    {
        if (!g_strcmp0 (icon, "clear-day") ||
            !g_strcmp0 (icon, "clear-night"))         return "Clear sky";
        if (!g_strcmp0 (icon, "partly-cloudy-day") ||
            !g_strcmp0 (icon, "partly-cloudy-night")) return "Partly cloudy";
        if (!g_strcmp0 (icon, "cloudy"))              return "Cloudy";
        if (!g_strcmp0 (icon, "fog"))                 return "Fog";
        if (!g_strcmp0 (icon, "wind"))                return "Windy";
        if (!g_strcmp0 (icon, "rain"))                return "Rain";
        if (!g_strcmp0 (icon, "sleet"))               return "Sleet";
        if (!g_strcmp0 (icon, "snow"))                return "Snow";
        if (!g_strcmp0 (icon, "hail"))                return "Hail";
        if (!g_strcmp0 (icon, "thunderstorm"))        return "Thunderstorm";
    }
    if (condition != NULL)
    {
        if (!g_strcmp0 (condition, "dry"))            return "Clear";
        if (!g_strcmp0 (condition, "fog"))            return "Fog";
        if (!g_strcmp0 (condition, "rain"))           return "Rain";
        if (!g_strcmp0 (condition, "sleet"))          return "Sleet";
        if (!g_strcmp0 (condition, "snow"))           return "Snow";
        if (!g_strcmp0 (condition, "hail"))           return "Hail";
        if (!g_strcmp0 (condition, "thunderstorm"))   return "Thunderstorm";
    }
    return "Unknown conditions";
}

/* The Bright Sky counterpart of pn_weather_parse_current().  Bright Sky's
 * /current_weather wraps the reading in a "weather" object and reports
 * units that already match ours (°C, %RH, km/h), so the only shape
 * differences are the member names and that conditions arrive as the
 * "icon"/"condition" strings rather than a WMO code.  Pure: no I/O, no
 * node state. */
gboolean
pn_weather_parse_bright_sky_current (JsonObject *root, PnWeatherCurrent *out)
{
    JsonNode *wn;

    g_return_val_if_fail (out != NULL, FALSE);

    if (root == NULL)
        return FALSE;

    /* Bright Sky reports problems as {"detail":"..."} (e.g. the HTTP 404
     * "No sources match your criteria" returned outside its coverage). */
    {
        const gchar *detail = obj_string (root, "detail");
        if (detail != NULL)
            out->reason = g_strdup (detail);
    }

    if (!json_object_has_member (root, "weather"))
        return out->ok;

    wn = json_object_get_member (root, "weather");
    if (JSON_NODE_HOLDS_OBJECT (wn))
    {
        JsonObject *w    = json_node_get_object (wn);
        gdouble     temp = obj_number (w, "temperature",       (gdouble) NAN);
        gdouble     hum  = obj_number (w, "relative_humidity", (gdouble) NAN);
        /* Bright Sky exposes 10/30/60-minute wind averages; the 10-minute
         * mean is the closest to "current". */
        gdouble     wind = obj_number (w, "wind_speed_10",     (gdouble) NAN);

        /* A finite temperature is the marker of a usable reading. */
        if (isfinite (temp))
        {
            out->ok          = TRUE;
            out->temperature = temp;
            if (isfinite (hum))  { out->has_humidity = TRUE; out->humidity   = hum;  }
            if (isfinite (wind)) { out->has_wind     = TRUE; out->wind_speed = wind; }
            out->description = g_strdup (pn_weather_bright_sky_description (
                    obj_string (w, "icon"), obj_string (w, "condition")));
        }
    }

    return out->ok;
}

/* Borrow the symbol_code from one MET Norway forecast window (e.g.
 * "next_1_hours") of a timeseries @data block: data.<window>.summary
 * .symbol_code.  Returns %NULL when the window or the chain is absent.
 * The result points into the parser and is valid only while it lives. */
static const gchar *
met_no_symbol_code (JsonObject *data, const gchar *window)
{
    JsonNode   *node;
    JsonObject *win, *summary;

    if (data == NULL || !json_object_has_member (data, window))
        return NULL;
    node = json_object_get_member (data, window);
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    win = json_node_get_object (node);

    if (!json_object_has_member (win, "summary"))
        return NULL;
    node = json_object_get_member (win, "summary");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return NULL;
    summary = json_node_get_object (node);

    return obj_string (summary, "symbol_code");
}

/* Map a MET Norway symbol_code to a short English label.  met.no encodes
 * the sky/precipitation state and an optional day/night/polartwilight
 * variant in one token (e.g. "partlycloudy_day", "heavyrainshowers_night",
 * "lightsleetandthunder").  The variant suffix is irrelevant to the text
 * label, and the precipitation tokens share stable substrings, so this
 * matches on those substrings (most specific first) rather than
 * enumerating the ~100 combinations.  @symbol_code may be %NULL. */
const gchar *
pn_weather_met_no_description (const gchar *symbol_code)
{
    if (symbol_code == NULL || *symbol_code == '\0')
        return "Unknown conditions";

    /* Thunder is reported as an "...andthunder" suffix on a precipitation
     * token, so it has to be checked before the precipitation substrings. */
    if (strstr (symbol_code, "thunder") != NULL)  return "Thunderstorm";

    /* "sleet" must precede "rain"/"snow": it never co-occurs with them in a
     * single code, but keep the most-specific precipitation types first. */
    if (strstr (symbol_code, "sleet") != NULL)
        return strstr (symbol_code, "showers") ? "Sleet showers" : "Sleet";
    if (strstr (symbol_code, "snow") != NULL)
        return strstr (symbol_code, "showers") ? "Snow showers"  : "Snow";
    if (strstr (symbol_code, "rain") != NULL)
        return strstr (symbol_code, "showers") ? "Rain showers"  : "Rain";

    if (strstr (symbol_code, "fog") != NULL)          return "Fog";
    if (g_str_has_prefix (symbol_code, "partlycloudy")) return "Partly cloudy";
    if (g_str_has_prefix (symbol_code, "cloudy"))     return "Cloudy";
    if (g_str_has_prefix (symbol_code, "fair"))       return "Fair";
    if (g_str_has_prefix (symbol_code, "clearsky"))   return "Clear sky";

    return "Unknown conditions";
}

/* The MET Norway counterpart of pn_weather_parse_current().  The
 * Locationforecast /compact body nests the readings under
 * properties.timeseries[0].data: the "instant" block has the temperature,
 * humidity, wind and pressure, while the conditions label rides on the
 * "next_1_hours" (or, near the forecast tail, "next_6_hours") summary's
 * symbol_code.  met.no reports wind in m/s, so it is converted to km/h to
 * match the metric contract the other providers already satisfy.  Pure:
 * no I/O, no node state. */
gboolean
pn_weather_parse_met_no_current (JsonObject *root, PnWeatherCurrent *out)
{
    JsonObject *props, *first, *data, *instant, *details;
    JsonArray  *series;
    JsonNode   *node;
    gdouble     temp, hum, wind;

    g_return_val_if_fail (out != NULL, FALSE);

    if (root == NULL || !json_object_has_member (root, "properties"))
        return out->ok;

    node = json_object_get_member (root, "properties");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return out->ok;
    props = json_node_get_object (node);

    if (!json_object_has_member (props, "timeseries"))
        return out->ok;
    node = json_object_get_member (props, "timeseries");
    if (!JSON_NODE_HOLDS_ARRAY (node))
        return out->ok;
    series = json_node_get_array (node);
    if (json_array_get_length (series) == 0)
        return out->ok;

    node = json_array_get_element (series, 0);
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return out->ok;
    first = json_node_get_object (node);

    if (!json_object_has_member (first, "data"))
        return out->ok;
    node = json_object_get_member (first, "data");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return out->ok;
    data = json_node_get_object (node);

    if (!json_object_has_member (data, "instant"))
        return out->ok;
    node = json_object_get_member (data, "instant");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return out->ok;
    instant = json_node_get_object (node);

    if (!json_object_has_member (instant, "details"))
        return out->ok;
    node = json_object_get_member (instant, "details");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return out->ok;
    details = json_node_get_object (node);

    temp = obj_number (details, "air_temperature",           (gdouble) NAN);
    hum  = obj_number (details, "relative_humidity",         (gdouble) NAN);
    wind = obj_number (details, "wind_speed",                (gdouble) NAN);

    /* A finite temperature is the marker of a usable reading. */
    if (isfinite (temp))
    {
        const gchar *symbol;

        out->ok          = TRUE;
        out->temperature = temp;
        if (isfinite (hum))  { out->has_humidity = TRUE; out->humidity = hum; }
        /* m/s -> km/h, the unit every other provider already reports. */
        if (isfinite (wind))
        {
            out->has_wind   = TRUE;
            out->wind_speed = wind * 3.6;
        }

        /* The conditions symbol lives on a forecast window, not the
         * instant: prefer next_1_hours and fall back to next_6_hours,
         * which is all that is present near the end of the timeseries. */
        symbol = met_no_symbol_code (data, "next_1_hours");
        if (symbol == NULL)
            symbol = met_no_symbol_code (data, "next_6_hours");
        out->description = g_strdup (pn_weather_met_no_description (symbol));
    }

    return out->ok;
}

/* Trim a MET Norway /compact response down to the slice the Weather node
 * actually consumes.  The provider always returns a full ~10-day forecast
 * (~85 timeseries entries, ~37 KB) — there is no query parameter to ask for
 * less — but pn_weather_parse_met_no_current() only ever reads
 * timeseries[0], the nowcast for the current hour.  The other ~84 entries
 * are a genuine future forecast that nothing downstream reads today, so
 * copying them verbatim into the message's "raw" member bloats every
 * message.  Return a deep copy of @root with properties.timeseries
 * truncated to its first entry; meta and geometry are small and kept whole.
 *
 * TODO(forecast): when the Weather node grows real forecast support across
 * ALL providers (Open-Meteo / Bright Sky / met.no), gate this trim behind a
 * boolean "forecast" property.  When that property is set, skip the trim and
 * pass the full timeseries through so a downstream Query/forecast node can
 * mine the 10-day outlook met.no already delivers free in every response. */
JsonNode *
pn_weather_met_no_trim_raw (JsonNode *root)
{
    JsonObject *src, *src_props, *out, *out_props;
    JsonArray  *series, *trimmed;
    JsonNode   *node, *out_node;
    GList      *keys, *l;

    if (!JSON_NODE_HOLDS_OBJECT (root))
        return json_node_copy (root);
    src = json_node_get_object (root);

    /* Anything without a properties.timeseries to trim is already small;
     * hand back a plain copy unchanged. */
    if (!json_object_has_member (src, "properties"))
        return json_node_copy (root);
    node = json_object_get_member (src, "properties");
    if (!JSON_NODE_HOLDS_OBJECT (node))
        return json_node_copy (root);
    src_props = json_node_get_object (node);

    if (!json_object_has_member (src_props, "timeseries"))
        return json_node_copy (root);
    node = json_object_get_member (src_props, "timeseries");
    if (!JSON_NODE_HOLDS_ARRAY (node))
        return json_node_copy (root);
    series = json_node_get_array (node);
    if (json_array_get_length (series) <= 1)
        return json_node_copy (root);

    /* json_node_copy() is shallow for object/array nodes — it shares the
     * held JsonObject/JsonArray rather than duplicating it — so we must not
     * copy @root and then edit its timeseries: that would mutate the
     * caller's parsed tree.  Instead rebuild only the two containers we
     * change (the root object and its "properties"), carrying every other
     * member across by value-copy.  Those copies stay shared by reference,
     * which is safe because we never mutate them and the new tree holds its
     * own references. */
    out       = json_object_new ();
    out_props = json_object_new ();

    keys = json_object_get_members (src_props);
    for (l = keys; l != NULL; l = l->next)
    {
        const gchar *k = l->data;
        if (g_strcmp0 (k, "timeseries") == 0)
            continue;
        json_object_set_member (
                out_props, k,
                json_node_copy (json_object_get_member (src_props, k)));
    }
    g_list_free (keys);

    /* timeseries keeps only the first entry — the current-hour nowcast. */
    trimmed = json_array_new ();
    json_array_add_element (
            trimmed, json_node_copy (json_array_get_element (series, 0)));
    json_object_set_array_member (out_props, "timeseries", trimmed);

    keys = json_object_get_members (src);
    for (l = keys; l != NULL; l = l->next)
    {
        const gchar *k = l->data;
        if (g_strcmp0 (k, "properties") == 0)
            continue;
        json_object_set_member (
                out, k, json_node_copy (json_object_get_member (src, k)));
    }
    g_list_free (keys);

    json_object_set_object_member (out, "properties", out_props);

    out_node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (out_node, out);
    return out_node;
}

/* Build a curl command that GETs @url and prints the body followed by
 * the HTTP status sentinel that pn_http_split_body_and_status() looks
 * for.  Shared shape for both the geocoding and (via build_command)
 * forecast calls — curl's exit code, the response body, and the HTTP
 * status are all needed to produce a specific error message when a
 * request fails. */
static gchar *
weather_curl_command (const gchar *url, guint timeout, const gchar *user_agent)
{
    gchar *quoted   = g_shell_quote (url);
    gchar *ua_opt   = NULL;
    gchar *cmd;

    /* MET Norway rejects requests without a descriptive User-Agent; the
     * other providers ignore it, so the header is only added when a
     * provider asks for one. */
    if (user_agent != NULL)
    {
        gchar *quoted_ua = g_shell_quote (user_agent);
        ua_opt = g_strdup_printf ("--user-agent %s ", quoted_ua);
        g_free (quoted_ua);
    }

    cmd = g_strdup_printf (
            "curl --silent --show-error --location "
            "--max-time %u "
            "--write-out '%s%%{http_code}' "
            "-H 'Accept: application/json' "
            "%s%s",
            timeout, PN_HTTP_STATUS_SENTINEL,
            ua_opt ? ua_opt : "", quoted);

    g_free (ua_opt);
    g_free (quoted);
    return cmd;
}

/* First non-empty line of @text, trimmed; %NULL when @text is %NULL or
 * has nothing printable.  curl's --show-error writes a one-line message
 * to stderr on transport failure ("curl: (6) Could not resolve host"
 * etc.); that line is what makes a network error legible to the user.
 * Caller frees the result. */
static gchar *
weather_first_meaningful_line (const gchar *text)
{
    const gchar *p;

    if (text == NULL)
        return NULL;

    for (p = text; *p != '\0'; )
    {
        const gchar *eol = strchr (p, '\n');
        gsize        len = eol ? (gsize) (eol - p) : strlen (p);
        gchar       *line;
        gchar       *trimmed;

        line = g_strndup (p, len);
        trimmed = g_strstrip (line);
        if (*trimmed != '\0')
        {
            gchar *copy = g_strdup (trimmed);
            g_free (line);
            return copy;
        }
        g_free (line);
        if (!eol)
            break;
        p = eol + 1;
    }
    return NULL;
}

/* Parse @body as a provider JSON error envelope and steal its
 * explanation string; returns %NULL when the body is empty, not JSON, or
 * carries no such string.  Caller frees the result.  Used to surface the
 * provider's own words alongside the HTTP status — Open-Meteo puts the
 * rate-limit text and "No matching location found" in "reason", while
 * Bright Sky uses "detail" (e.g. "No sources match your criteria"). */
static gchar *
weather_extract_reason (const gchar *body)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    gchar      *reason = NULL;

    if (body == NULL || *body == '\0')
        return NULL;

    parser = json_parser_new ();
    if (json_parser_load_from_data (parser, body, -1, NULL))
    {
        root = json_parser_get_root (parser);
        if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        {
            const gchar *s;

            obj = json_node_get_object (root);
            s = obj_string (obj, "reason");
            if (s == NULL)
                s = obj_string (obj, "detail");
            if (s != NULL)
                reason = g_strdup (s);
        }
    }
    g_object_unref (parser);
    return reason;
}

/* Build a one-line human description of why an HTTP request failed,
 * given everything we know about the curl run.  @what is "Weather
 * fetch" or "Location lookup" so the same shape covers both endpoints.
 * Returns a newly-allocated string the caller frees.  Falls through to
 * a generic "request failed" when no field has anything useful. */
static gchar *
weather_format_http_error (const gchar      *what,
                           PnWeatherProvider provider,
                           gboolean          spawned,
                           gint              exit_status,
                           const gchar      *stderr_text,
                           gint              http_status,
                           const gchar      *body)
{
    gchar *reason;
    gchar *err_line;

    if (!spawned)
        return g_strdup_printf ("%s could not start (curl not run).", what);

    if (!g_spawn_check_wait_status (exit_status, NULL))
    {
        err_line = weather_first_meaningful_line (stderr_text);
        if (err_line != NULL)
        {
            gchar *out = g_strdup_printf ("%s failed: %s", what, err_line);
            g_free (err_line);
            return out;
        }
        return g_strdup_printf ("%s failed (curl exit status %d).",
                                what, exit_status);
    }

    /* Bright Sky answers HTTP 404 with "No sources match your criteria"
     * for any location it has no nearby station for — which, for a
     * geocoder that resolves the whole globe, mostly means "outside
     * Bright Sky's coverage".  Translate that into an actionable hint
     * rather than echoing the raw status. */
    if (provider == PN_WEATHER_PROVIDER_BRIGHT_SKY && http_status == 404)
        return g_strdup_printf (
                "%s: Bright Sky has no weather station near this location. "
                "It covers Germany and parts of central Europe \xe2\x80\x94 "
                "switch the provider to Open-Meteo for worldwide coverage.",
                what);

    reason = weather_extract_reason (body);

    if (http_status >= 400)
    {
        gchar *out;
        if (reason != NULL)
            out = g_strdup_printf ("%s failed (HTTP %d): %s",
                                   what, http_status, reason);
        else if (http_status == 429)
            out = g_strdup_printf (
                    "%s rate-limited by the weather provider (HTTP 429). "
                    "Increase the period and try again later.", what);
        else
            out = g_strdup_printf ("%s failed (HTTP %d).",
                                   what, http_status);
        g_free (reason);
        return out;
    }

    if (reason != NULL)
    {
        gchar *out = g_strdup_printf ("%s failed: %s", what, reason);
        g_free (reason);
        return out;
    }

    if (body == NULL || *body == '\0')
        return g_strdup_printf ("%s returned an empty response.", what);

    if (http_status > 0)
        return g_strdup_printf (
                "%s returned an unexpected response (HTTP %d).",
                what, http_status);
    return g_strdup_printf ("%s returned an unexpected response.", what);
}

/* ------------------------------------------------------------------ */
/*  Geocoding (worker thread)                                          */
/* ------------------------------------------------------------------ */

/* Resolve @city to coordinates via Open-Meteo's geocoding endpoint and
 * cache the result.  On success the cache is keyed on @city so a steady
 * city is geocoded only once; on failure the cache is invalidated and a
 * human-readable reason is stashed on the node so the trigger can
 * forward it to a downstream Debug / Text View (transient outages,
 * rate-limits, and real "no such place" should all read differently).
 * A later tick retries the failed lookup. */
static void
weather_geocode (PnWeather *self, const gchar *city)
{
    /* Geocoding is always Open-Meteo's (global) endpoint, regardless of
     * the selected forecast provider; pn_geocode does the curl + parse. */
    PnGeocodeResult res = { 0 };

    pn_geocode_resolve_sync (city, 10u, &res);

    g_mutex_lock (&self->mutex);
    g_free (self->resolved_name);
    g_free (self->country);
    g_free (self->geo_key);
    g_clear_pointer (&self->geo_error, g_free);
    if (res.resolved)
    {
        self->have_coords   = TRUE;
        self->latitude      = res.latitude;
        self->longitude     = res.longitude;
        self->resolved_name = res.name ? res.name : g_strdup (city);
        self->country       = res.country;    /* may be NULL */
        self->geo_key       = g_strdup (city);
        res.name    = NULL;                    /* ownership transferred */
        res.country = NULL;
    }
    else
    {
        self->have_coords   = FALSE;
        self->resolved_name = NULL;
        self->country       = NULL;
        self->geo_key       = NULL;           /* force a retry next tick */
        self->geo_error     = res.error;      /* may be NULL */
        res.error = NULL;
    }
    g_mutex_unlock (&self->mutex);

    pn_geocode_result_clear (&res);
}

/* Ensure cached coordinates match the configured city, geocoding when
 * needed.  Returns %TRUE when coordinates are available.  On failure
 * fills @out_error (caller frees) with the reason the most recent
 * lookup gave, so the trigger can emit something more specific than a
 * generic "not found". */
static gboolean
weather_ensure_coords (PnWeather *self, const gchar *city, gchar **out_error)
{
    gboolean need;
    gboolean ok;

    g_mutex_lock (&self->mutex);
    need = !self->have_coords || g_strcmp0 (self->geo_key, city) != 0;
    g_mutex_unlock (&self->mutex);

    if (need)
        weather_geocode (self, city);

    g_mutex_lock (&self->mutex);
    ok = self->have_coords;
    if (!ok && out_error != NULL)
        *out_error = g_strdup (self->geo_error);
    g_mutex_unlock (&self->mutex);

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Trigger (worker thread)                                            */
/* ------------------------------------------------------------------ */

static void
pn_weather_trigger (PnAutoTrigger *trigger)
{
    PnWeather          *self   = PN_WEATHER (trigger);
    PnAutoTriggerClass *parent = PN_AUTO_TRIGGER_CLASS (pn_weather_parent_class);
    gchar              *city   = weather_dup_city_locked (self);

    /* is_configured already gates an empty city, but the worker may be
     * mid-flight when the property is cleared, so re-check. */
    if (city == NULL || *city == '\0')
    {
        g_free (city);
        return;
    }

    gchar *geo_error = NULL;
    if (!weather_ensure_coords (self, city, &geo_error))
    {
        /* The place could not be resolved: surface the specific reason
         * (rate-limit, network error, real "not found", ...) so a
         * downstream Debug / Text View tells the user what actually
         * happened rather than every failure flattening to one message. */
        PnMessage *msg     = pn_message_new (PN_NODE (self), NULL);
        gchar     *summary = (geo_error != NULL && *geo_error != '\0')
                ? g_strdup (geo_error)
                : g_strdup_printf (
                        "Could not look up the location \"%s\".", city);

        pn_message_set_string  (msg, "city",    city);
        pn_message_set_boolean (msg, "success", FALSE);
        pn_message_set_string  (msg, "output",  summary);
        pn_auto_trigger_emit_on_main (trigger, msg);

        g_free (summary);
        g_free (geo_error);
        g_free (city);
        return;
    }
    g_free (geo_error);

    g_free (city);

    /* Coordinates are ready — let #PnHttp run the forecast fetch through
     * our build_command()/emit_message() overrides. */
    if (parent->trigger != NULL)
        parent->trigger (trigger);
}

/* ------------------------------------------------------------------ */
/*  PnHttp overrides                                                   */
/* ------------------------------------------------------------------ */

static gboolean
pn_weather_is_configured (PnHttp *http)
{
    PnWeather *self = PN_WEATHER (http);
    gchar     *city = weather_dup_city_locked (self);
    gchar     *url  = pn_http_dup_url (http);
    gboolean   ok   = city != NULL && *city != '\0' &&
                      url  != NULL && *url  != '\0';

    g_free (city);
    g_free (url);
    return ok;
}

static gchar *
pn_weather_build_command (PnHttp *http, guint timeout)
{
    PnWeather        *self     = PN_WEATHER (http);
    gchar            *base     = pn_http_dup_url (http);
    PnWeatherProvider provider = weather_get_provider_locked (self);
    gchar             latbuf[G_ASCII_DTOSTR_BUF_SIZE];
    gchar             lonbuf[G_ASCII_DTOSTR_BUF_SIZE];
    gdouble           lat, lon;
    const gchar      *user_agent = NULL;
    gchar            *full;
    gchar            *cmd;

    g_mutex_lock (&self->mutex);
    lat = self->latitude;
    lon = self->longitude;
    g_mutex_unlock (&self->mutex);

    /* Locale-independent formatting: the APIs want a '.' decimal point
     * regardless of the host's LC_NUMERIC. */
    g_ascii_dtostr (latbuf, sizeof latbuf, lat);
    g_ascii_dtostr (lonbuf, sizeof lonbuf, lon);

    if (provider == PN_WEATHER_PROVIDER_BRIGHT_SKY)
    {
        /* Bright Sky's /current_weather takes lat/lon and returns the
         * latest observation; no variable list or timezone knob. */
        full = g_strdup_printf (
                "%s?lat=%s&lon=%s",
                base ? base : PN_WEATHER_BRIGHTSKY_URL, latbuf, lonbuf);
    }
    else if (provider == PN_WEATHER_PROVIDER_MET_NO)
    {
        /* MET Norway's Locationforecast /compact takes lat/lon and returns
         * a timeseries whose first entry is the current reading.  It
         * requires the descriptive User-Agent below. */
        full = g_strdup_printf (
                "%s?lat=%s&lon=%s",
                base ? base : PN_WEATHER_MET_NO_URL, latbuf, lonbuf);
        user_agent = PN_WEATHER_MET_NO_USER_AGENT;
    }
    else
    {
        /* timezone=auto makes Open-Meteo resolve the location's own
         * timezone and stamp current.time in that local time; without it
         * the API defaults to GMT, so the report's clock reads UTC. */
        full = g_strdup_printf (
                "%s?latitude=%s&longitude=%s&current=%s&timezone=auto",
                base ? base : PN_WEATHER_FORECAST_URL,
                latbuf, lonbuf, PN_WEATHER_CURRENT_VARS);
    }
    cmd = weather_curl_command (full, timeout, user_agent);

    g_free (full);
    g_free (base);
    return cmd;
}

static void
pn_weather_emit_message (
        PnHttp      *http,
        gboolean     spawned,
        gint         exit_status,
        const gchar *stdout_text,
        const gchar *stderr_text)
{
    PnWeather   *self    = PN_WEATHER (http);
    PnNode      *node    = PN_NODE (self);
    gchar       *body    = NULL;
    gint         http_status = 0;
    gboolean     transport_ok;
    gboolean     ok      = FALSE;
    gchar       *failure = NULL;
    PnMessage   *msg;
    gchar       *name, *country;
    gdouble      lat, lon;
    PnWeatherProvider provider;

    /* Snapshot the resolved place + selected provider for the message. */
    g_mutex_lock (&self->mutex);
    name     = g_strdup (self->resolved_name);
    country  = g_strdup (self->country);
    lat      = self->latitude;
    lon      = self->longitude;
    provider = self->provider;
    g_mutex_unlock (&self->mutex);

    /* weather_curl_command() includes the same HTTP-status sentinel as
     * the base class's default build_command, so the body and status
     * can be split back apart here.  Open-Meteo answers HTTP 200 with
     * the reading on success and HTTP 4xx (typically 429 for rate
     * limits) with a {"error":true,"reason":...} body on failure, so
     * both the status and the parsed body need to feed the decision. */
    pn_http_split_body_and_status (stdout_text, &body, &http_status);
    transport_ok = spawned &&
                   g_spawn_check_wait_status (exit_status, NULL) &&
                   (http_status == 0 || (http_status >= 200 &&
                                         http_status < 300));

    msg = pn_message_new (node, NULL);
    if (name != NULL)
        pn_message_set_string (msg, "city", name);
    if (country != NULL)
        pn_message_set_string (msg, "country", country);
    pn_message_set_double (msg, "latitude",  lat);
    pn_message_set_double (msg, "longitude", lon);
    if (http_status > 0)
        pn_message_set_int (msg, "status", http_status);

    if (transport_ok && body != NULL && *body != '\0')
    {
        JsonParser *parser = json_parser_new ();

        if (json_parser_load_from_data (parser, body, -1, NULL))
        {
            JsonNode   *root = json_parser_get_root (parser);
            JsonObject *o    = (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
                               ? json_node_get_object (root) : NULL;

            /* Raw passthrough: hand the provider's whole response to
             * downstream nodes verbatim (a deep copy, since the parser
             * owns @root) so a Query / JMESPath / JSON Path node can read
             * any field — including an error body — that we do not
             * promote to a named member.  met.no is the exception: its
             * response carries a full ~10-day forecast we never consume, so
             * "raw" is trimmed to the current-hour entry we parse.  See
             * pn_weather_met_no_trim_raw() for the forecast TODO. */
            if (o != NULL)
            {
                JsonNode *raw =
                        (provider == PN_WEATHER_PROVIDER_MET_NO)
                                ? pn_weather_met_no_trim_raw (root)
                                : json_node_copy (root);
                pn_message_set_member (msg, "raw", raw);
            }

            /* Pull the reading + any provider error out of the parsed
             * body with the matching provider parser; the message/summary
             * building below stays here because it needs the node's
             * resolved place name. */
            PnWeatherCurrent cur = { 0 };

            if (provider == PN_WEATHER_PROVIDER_BRIGHT_SKY)
                pn_weather_parse_bright_sky_current (o, &cur);
            else if (provider == PN_WEATHER_PROVIDER_MET_NO)
                pn_weather_parse_met_no_current (o, &cur);
            else
                pn_weather_parse_current (o, &cur);

            if (cur.ok)
            {
                GString     *summary = g_string_new (NULL);
                /* Open-Meteo carries a WMO code we map to a label; Bright
                 * Sky already supplies the label in cur.description. */
                const gchar *desc =
                        cur.description != NULL ? cur.description
                        : cur.has_code ? pn_weather_code_description (
                                                 cur.weather_code)
                        : NULL;

                ok = TRUE;
                pn_message_set_double (msg, "value",       cur.temperature);
                pn_message_set_double (msg, "temperature", cur.temperature);
                if (cur.has_humidity)
                    pn_message_set_double (msg, "humidity", cur.humidity);
                if (cur.has_wind)
                    pn_message_set_double (msg, "wind_speed", cur.wind_speed);
                if (cur.has_code)
                    pn_message_set_int (msg, "weather_code", cur.weather_code);
                if (desc != NULL)
                    pn_message_set_string (msg, "description", desc);

                if (name != NULL)
                    g_string_append (summary, name);
                if (country != NULL && *country != '\0')
                    g_string_append_printf (summary, ", %s", country);
                if (summary->len > 0)
                    g_string_append (summary, ": ");
                g_string_append_printf (summary, "%.1f \xc2\xb0""C",
                                        cur.temperature);

                if (desc != NULL)
                    g_string_append_printf (summary, ", %s", desc);
                if (cur.has_humidity)
                    g_string_append_printf (summary, ", %.0f%% RH",
                                            cur.humidity);
                if (cur.has_wind)
                    g_string_append_printf (summary,
                                            ", wind %.1f km/h", cur.wind_speed);

                pn_message_set_string (msg, "output", summary->str);
                g_string_free (summary, TRUE);
            }
            else
            {
                /* HTTP 200, JSON parsed, but no usable reading.  The
                 * provider's error string (if any) explains why; otherwise
                 * the response shape was unexpected. */
                failure = (cur.reason != NULL)
                        ? g_strdup_printf (
                                "Weather fetch failed: %s", cur.reason)
                        : g_strdup_printf (
                                "Weather fetch returned no current "
                                "reading for %s.",
                                name ? name : "the configured location");
            }
            g_free (cur.reason);
            g_free (cur.description);
        }
        else
        {
            failure = g_strdup ("Weather fetch returned a non-JSON response.");
        }
        g_object_unref (parser);
    }
    else if (failure == NULL)
    {
        failure = weather_format_http_error (
                "Weather fetch", provider,
                spawned, exit_status, stderr_text, http_status, body);
    }

    pn_message_set_boolean (msg, "success", ok);
    if (!ok)
    {
        if (failure == NULL)
            failure = g_strdup_printf ("Failed to read weather for %s.",
                                       name ? name : "the configured location");
        pn_message_set_string (msg, "output", failure);
    }

    pn_auto_trigger_emit_on_main (PN_AUTO_TRIGGER (self), msg);

    g_free (failure);
    g_free (body);
    g_free (name);
    g_free (country);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_weather_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnWeather *self = PN_WEATHER (object);

    switch (prop_id)
    {
    case PROP_CITY:
        g_value_take_string (value, weather_dup_city_locked (self));
        break;
    case PROP_PROVIDER:
        g_value_set_enum (value, weather_get_provider_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_weather_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnWeather *self = PN_WEATHER (object);
    PnHttp    *http = PN_HTTP (self);

    switch (prop_id)
    {
    case PROP_CITY:
    {
        const gchar *new_city = g_value_get_string (value);
        gboolean     changed;

        g_mutex_lock (&self->mutex);
        changed = g_strcmp0 (self->city, new_city) != 0;
        if (changed)
        {
            g_free (self->city);
            self->city = g_strdup (new_city ? new_city : "");

            /* The cached coordinates belong to the old city; drop them
             * so the next tick re-geocodes.  The stale error message
             * (if any) also belongs to the old city. */
            self->have_coords = FALSE;
            g_clear_pointer (&self->geo_key,   g_free);
            g_clear_pointer (&self->geo_error, g_free);
        }
        g_mutex_unlock (&self->mutex);

        if (changed)
        {
            g_object_notify_by_pspec (object, props[PROP_CITY]);
            pn_http_apply_visual_state (
                    http, PN_HTTP_GET_CLASS (self)->is_configured (http));

            /* Show the new place's weather within seconds rather than
             * after a full period. */
            if (PN_HTTP_GET_CLASS (self)->is_configured (http))
                pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
        }
        break;
    }
    case PROP_PROVIDER:
    {
        PnWeatherProvider new_p = g_value_get_enum (value);
        gboolean          changed;

        g_mutex_lock (&self->mutex);
        changed = self->provider != new_p;
        if (changed)
            self->provider = new_p;
        g_mutex_unlock (&self->mutex);

        if (changed)
        {
            /* Track the choice in the (overridable) forecast endpoint so
             * the url field shows the active provider's default; the
             * geocoder is shared, so the cached coordinates stay valid
             * and only the forecast fetch changes. */
            const gchar *url = PN_WEATHER_FORECAST_URL;
            if (new_p == PN_WEATHER_PROVIDER_BRIGHT_SKY)
                url = PN_WEATHER_BRIGHTSKY_URL;
            else if (new_p == PN_WEATHER_PROVIDER_MET_NO)
                url = PN_WEATHER_MET_NO_URL;
            g_object_set (self, "url", url, NULL);
            g_object_notify_by_pspec (object, props[PROP_PROVIDER]);

            /* Refresh the reading from the new provider within seconds. */
            if (PN_HTTP_GET_CLASS (self)->is_configured (http))
                pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
        }
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_weather_finalize (GObject *object)
{
    PnWeather *self = PN_WEATHER (object);

    g_clear_pointer (&self->city, g_free);
    g_clear_pointer (&self->geo_key, g_free);
    g_clear_pointer (&self->resolved_name, g_free);
    g_clear_pointer (&self->country, g_free);
    g_clear_pointer (&self->geo_error, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_weather_parent_class)->finalize (object);
}

static void
pn_weather_class_init (PnWeatherClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnHttpClass        *http_class    = PN_HTTP_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_weather_get_property;
    object_class->set_property = pn_weather_set_property;
    object_class->finalize     = pn_weather_finalize;

    trigger_class->trigger     = pn_weather_trigger;

    http_class->is_configured  = pn_weather_is_configured;
    http_class->build_command  = pn_weather_build_command;
    http_class->emit_message   = pn_weather_emit_message;

    /* Visual identity. */
    node_class->palette_icon = PN_WEATHER_ICON;
    node_class->class_name   = "Weather";
    node_class->icon         = PN_WEATHER_ICON;
    node_class->color        = (PnColor){ 0.30, 0.60, 0.85, 1.0 };
    node_class->category     = "Network";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_WEATHER_ICON;
    http_class->normal_color = (PnColor){ 0.30, 0.60, 0.85, 1.0 };

    props[PROP_CITY] = g_param_spec_string (
            "city", "City",
            "Place name to fetch weather for (e.g. \"Berlin\" or "
            "\"Berlin, US\").  Resolved to coordinates via Open-Meteo's "
            "geocoding service before each forecast fetch.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PROVIDER] = g_param_spec_enum (
            "provider", "Provider",
            "Which free, key-less forecast source to fetch from.  "
            "Open-Meteo (the default) and MET Norway both have global "
            "coverage; Bright Sky serves DWD open data for Germany and "
            "parts of central Europe.  The city is geocoded through "
            "Open-Meteo for all three.",
            PN_TYPE_WEATHER_PROVIDER, PN_WEATHER_PROVIDER_OPEN_METEO,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_weather_init (PnWeather *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  sky  = { 0.30, 0.60, 0.85, 1.0 };

    g_mutex_init (&self->mutex);
    self->city = g_strdup ("");

    pn_node_set_class_name (node, "Weather");
    pn_node_set_icon       (node, PN_WEATHER_ICON);
    pn_node_set_color      (node, &sky);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* The forecast endpoint is exposed through the inherited "url"
     * property so an advanced user can point at a self-hosted Open-Meteo
     * instance; the geocoding endpoint stays fixed. */
    g_object_set (self, "url", PN_WEATHER_FORECAST_URL, NULL);
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_WEATHER_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWeather *
pn_weather_new (void)
{
    return g_object_new (PN_TYPE_WEATHER, NULL);
}
