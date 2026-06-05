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

/* Unit tests for PnWeather's pure parse seam.  The node fetches the
 * Open-Meteo API over the network, which is not reproducible headless,
 * so the testable contract is pn_weather_parse_current() — extracting
 * the current temperature/humidity/wind/code (and any provider error
 * "reason") from an already-parsed response object — plus the WMO code
 * label map.  Canned JSON bodies stand in for the fetch.  These pin the
 * logic half the headless/core split (TODO #23) keeps loadable without
 * GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-weather.h"

#include <json-glib/json-glib.h>

/* Parse @text and return its root object, keeping the owning parser in
 * *@keep (the caller unrefs it once done reading the object). */
static JsonObject *
root_object (const gchar *text, JsonParser **keep)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;

    g_assert (json_parser_load_from_data (parser, text, -1, NULL));
    root  = json_parser_get_root (parser);
    *keep = parser;
    return (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
           ? json_node_get_object (root) : NULL;
}

static void
test_parse_full_reading (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"current\":{"
        "\"temperature_2m\":21.5,"
        "\"relative_humidity_2m\":63,"
        "\"wind_speed_10m\":12.4,"
        "\"weather_code\":3}}", &keep);

    PN_CHECK (pn_weather_parse_current (o, &c));
    PN_CHECK_NEAR  (c.temperature, 21.5, 1e-9);
    PN_CHECK (c.has_humidity);
    PN_CHECK_NEAR  (c.humidity, 63.0, 1e-9);
    PN_CHECK (c.has_wind);
    PN_CHECK_NEAR  (c.wind_speed, 12.4, 1e-9);
    PN_CHECK (c.has_code);
    PN_CHECK_CMPINT (c.weather_code, ==, 3);
    PN_CHECK (c.reason == NULL);

    g_free (c.reason);
    g_object_unref (keep);
}

static void
test_parse_temperature_only (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"current\":{\"temperature_2m\":-4}}", &keep);

    /* Temperature alone is a usable reading; the optional fields stay
     * un-flagged so the caller omits them. */
    PN_CHECK (pn_weather_parse_current (o, &c));
    PN_CHECK_NEAR (c.temperature, -4.0, 1e-9);
    PN_CHECK_FALSE (c.has_humidity);
    PN_CHECK_FALSE (c.has_wind);
    PN_CHECK_FALSE (c.has_code);

    g_free (c.reason);
    g_object_unref (keep);
}

static void
test_parse_error_reason (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"error\":true,\"reason\":\"No matching location found\"}", &keep);

    /* An error body yields no reading but surfaces the provider reason. */
    PN_CHECK_FALSE (pn_weather_parse_current (o, &c));
    PN_CHECK_FALSE (c.ok);
    PN_CHECK_CMPSTR (c.reason, ==, "No matching location found");

    g_free (c.reason);
    g_object_unref (keep);
}

static void
test_parse_current_without_temperature (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"current\":{\"relative_humidity_2m\":50}}", &keep);

    /* No temperature -> not a usable reading, nothing flagged. */
    PN_CHECK_FALSE (pn_weather_parse_current (o, &c));
    PN_CHECK_FALSE (c.has_humidity);

    g_free (c.reason);
    g_object_unref (keep);
}

static void
test_parse_no_current_and_null (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object ("{\"latitude\":51.5}", &keep);

    PN_CHECK_FALSE (pn_weather_parse_current (o, &c));
    PN_CHECK (c.reason == NULL);
    g_object_unref (keep);

    /* A NULL root (un-parseable body) is a safe no-op. */
    PN_CHECK_FALSE (pn_weather_parse_current (NULL, &c));

    g_free (c.reason);
}

static void
test_code_description (void)
{
    PN_CHECK_CMPSTR (pn_weather_code_description (0),  ==, "Clear sky");
    PN_CHECK_CMPSTR (pn_weather_code_description (3),  ==, "Overcast");
    PN_CHECK_CMPSTR (pn_weather_code_description (95), ==, "Thunderstorm");
    /* Codes Open-Meteo never emits fall through to the generic label. */
    PN_CHECK_CMPSTR (pn_weather_code_description (9999), ==, "Unknown conditions");
}

/* ---- Bright Sky parse seam ------------------------------------------ */

static void
test_bright_sky_full_reading (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"weather\":{"
        "\"temperature\":21.9,"
        "\"relative_humidity\":46,"
        "\"wind_speed_10\":21.6,"
        "\"condition\":\"dry\","
        "\"icon\":\"partly-cloudy-day\"}}", &keep);

    PN_CHECK (pn_weather_parse_bright_sky_current (o, &c));
    PN_CHECK_NEAR  (c.temperature, 21.9, 1e-9);
    PN_CHECK (c.has_humidity);
    PN_CHECK_NEAR  (c.humidity, 46.0, 1e-9);
    PN_CHECK (c.has_wind);
    PN_CHECK_NEAR  (c.wind_speed, 21.6, 1e-9);
    /* Bright Sky carries no WMO code; the label rides on description. */
    PN_CHECK_FALSE (c.has_code);
    PN_CHECK_CMPSTR (c.description, ==, "Partly cloudy");
    PN_CHECK (c.reason == NULL);

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_bright_sky_temperature_only (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"weather\":{\"temperature\":-4,\"icon\":null,"
        "\"condition\":null}}", &keep);

    /* Temperature alone is usable; null icon/condition fall through to
     * the generic label and the optional fields stay un-flagged. */
    PN_CHECK (pn_weather_parse_bright_sky_current (o, &c));
    PN_CHECK_NEAR (c.temperature, -4.0, 1e-9);
    PN_CHECK_FALSE (c.has_humidity);
    PN_CHECK_FALSE (c.has_wind);
    PN_CHECK_CMPSTR (c.description, ==, "Unknown conditions");

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_bright_sky_detail_error (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"detail\":\"No sources match your criteria\"}", &keep);

    /* Out-of-coverage answer: no reading, but the "detail" string is
     * surfaced as the reason (mirrors Open-Meteo's "reason"). */
    PN_CHECK_FALSE (pn_weather_parse_bright_sky_current (o, &c));
    PN_CHECK_FALSE (c.ok);
    PN_CHECK_CMPSTR (c.reason, ==, "No sources match your criteria");
    PN_CHECK (c.description == NULL);

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_bright_sky_no_weather_and_null (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object ("{\"sources\":[]}", &keep);

    PN_CHECK_FALSE (pn_weather_parse_bright_sky_current (o, &c));
    PN_CHECK (c.reason == NULL);
    g_object_unref (keep);

    /* A NULL root (un-parseable body) is a safe no-op. */
    PN_CHECK_FALSE (pn_weather_parse_bright_sky_current (NULL, &c));

    g_free (c.reason);
    g_free (c.description);
}

static void
test_bright_sky_description (void)
{
    /* Icon wins when present. */
    PN_CHECK_CMPSTR (pn_weather_bright_sky_description ("clear-night", "dry"),
                     ==, "Clear sky");
    PN_CHECK_CMPSTR (pn_weather_bright_sky_description ("thunderstorm", NULL),
                     ==, "Thunderstorm");
    /* Falls back to condition when icon is absent. */
    PN_CHECK_CMPSTR (pn_weather_bright_sky_description (NULL, "rain"),
                     ==, "Rain");
    /* Both absent / unknown -> generic label. */
    PN_CHECK_CMPSTR (pn_weather_bright_sky_description (NULL, NULL),
                     ==, "Unknown conditions");
    PN_CHECK_CMPSTR (pn_weather_bright_sky_description ("moonbeam", NULL),
                     ==, "Unknown conditions");
}

/* ---- MET Norway parse seam ------------------------------------------ */

static void
test_met_no_full_reading (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    /* The /compact body nests the reading under
     * properties.timeseries[0].data; the conditions symbol rides on the
     * next_1_hours summary.  met.no reports wind in m/s. */
    JsonObject       *o = root_object (
        "{\"properties\":{\"timeseries\":[{"
        "\"time\":\"2026-06-05T12:00:00Z\","
        "\"data\":{"
        "\"instant\":{\"details\":{"
        "\"air_temperature\":18.4,"
        "\"relative_humidity\":71.2,"
        "\"wind_speed\":3.0,"
        "\"air_pressure_at_sea_level\":1012.3}},"
        "\"next_1_hours\":{\"summary\":{\"symbol_code\":\"partlycloudy_day\"}}"
        "}}]}}", &keep);

    PN_CHECK (pn_weather_parse_met_no_current (o, &c));
    PN_CHECK_NEAR  (c.temperature, 18.4, 1e-9);
    PN_CHECK (c.has_humidity);
    PN_CHECK_NEAR  (c.humidity, 71.2, 1e-9);
    /* 3.0 m/s -> 10.8 km/h, normalised to match the other providers. */
    PN_CHECK (c.has_wind);
    PN_CHECK_NEAR  (c.wind_speed, 10.8, 1e-9);
    /* met.no carries no WMO code; the label rides on description. */
    PN_CHECK_FALSE (c.has_code);
    PN_CHECK_CMPSTR (c.description, ==, "Partly cloudy");
    PN_CHECK (c.reason == NULL);

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_met_no_temperature_only (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    /* Temperature alone is usable; no forecast window means no symbol, so
     * the description falls through to the generic label and the optional
     * fields stay un-flagged. */
    JsonObject       *o = root_object (
        "{\"properties\":{\"timeseries\":[{"
        "\"data\":{\"instant\":{\"details\":{"
        "\"air_temperature\":-2.5}}}}]}}", &keep);

    PN_CHECK (pn_weather_parse_met_no_current (o, &c));
    PN_CHECK_NEAR (c.temperature, -2.5, 1e-9);
    PN_CHECK_FALSE (c.has_humidity);
    PN_CHECK_FALSE (c.has_wind);
    PN_CHECK_CMPSTR (c.description, ==, "Unknown conditions");

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_met_no_six_hour_fallback (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    /* Near the tail of the timeseries only next_6_hours is present; the
     * symbol is read from there when next_1_hours is absent. */
    JsonObject       *o = root_object (
        "{\"properties\":{\"timeseries\":[{"
        "\"data\":{"
        "\"instant\":{\"details\":{\"air_temperature\":9.0}},"
        "\"next_6_hours\":{\"summary\":{\"symbol_code\":\"heavyrain\"}}"
        "}}]}}", &keep);

    PN_CHECK (pn_weather_parse_met_no_current (o, &c));
    PN_CHECK_CMPSTR (c.description, ==, "Rain");

    g_free (c.reason);
    g_free (c.description);
    g_object_unref (keep);
}

static void
test_met_no_no_timeseries_and_null (void)
{
    JsonParser       *keep;
    PnWeatherCurrent  c = { 0 };
    JsonObject       *o = root_object (
        "{\"properties\":{\"timeseries\":[]}}", &keep);

    /* An empty timeseries yields no reading. */
    PN_CHECK_FALSE (pn_weather_parse_met_no_current (o, &c));
    PN_CHECK (c.reason == NULL);
    PN_CHECK (c.description == NULL);
    g_object_unref (keep);

    /* A NULL root (un-parseable body) is a safe no-op. */
    PN_CHECK_FALSE (pn_weather_parse_met_no_current (NULL, &c));

    g_free (c.reason);
    g_free (c.description);
}

static void
test_met_no_description (void)
{
    /* Day/night/polartwilight variant suffixes are ignored. */
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("clearsky_day"),
                     ==, "Clear sky");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("clearsky_polartwilight"),
                     ==, "Clear sky");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("fair_night"),
                     ==, "Fair");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("partlycloudy_day"),
                     ==, "Partly cloudy");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("cloudy"),
                     ==, "Cloudy");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("fog"),
                     ==, "Fog");
    /* Precipitation intensity prefixes collapse to the base type... */
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("lightrain"),
                     ==, "Rain");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("heavysnow"),
                     ==, "Snow");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("sleet"),
                     ==, "Sleet");
    /* ...while a "showers" variant keeps the showers wording. */
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("rainshowers_day"),
                     ==, "Rain showers");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("heavysnowshowers_night"),
                     ==, "Snow showers");
    /* "...andthunder" wins over the precipitation token it rides on. */
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("lightrainandthunder"),
                     ==, "Thunderstorm");
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("heavysleetshowersandthunder_day"),
                     ==, "Thunderstorm");
    /* Unknown / NULL -> generic label. */
    PN_CHECK_CMPSTR (pn_weather_met_no_description ("moonbeam"),
                     ==, "Unknown conditions");
    PN_CHECK_CMPSTR (pn_weather_met_no_description (NULL),
                     ==, "Unknown conditions");
}

static void
test_met_no_trim_raw (void)
{
    /* met.no always ships a full multi-day forecast, but the node only
     * consumes timeseries[0].  The trim keeps that first entry plus the
     * small meta/geometry blocks and drops the rest, so the message's
     * "raw" member is the current-hour slice rather than the whole body. */
    JsonParser *parser = json_parser_new ();
    JsonNode   *root, *trimmed;
    JsonObject *o, *props, *first;
    JsonArray  *series;

    g_assert (json_parser_load_from_data (parser,
        "{\"type\":\"Feature\","
        "\"geometry\":{\"type\":\"Point\",\"coordinates\":[10.75,59.91,0]},"
        "\"properties\":{"
            "\"meta\":{\"updated_at\":\"2026-06-05T05:27:57Z\"},"
            "\"timeseries\":["
              "{\"time\":\"2026-06-05T06:00:00Z\","
                "\"data\":{\"instant\":{\"details\":{\"air_temperature\":12.3}}}},"
              "{\"time\":\"2026-06-05T07:00:00Z\","
                "\"data\":{\"instant\":{\"details\":{\"air_temperature\":13.0}}}},"
              "{\"time\":\"2026-06-05T08:00:00Z\","
                "\"data\":{\"instant\":{\"details\":{\"air_temperature\":14.0}}}}"
            "]}}", -1, NULL));
    root    = json_parser_get_root (parser);
    trimmed = pn_weather_met_no_trim_raw (root);

    PN_CHECK (trimmed != NULL);
    PN_CHECK (JSON_NODE_HOLDS_OBJECT (trimmed));
    o = json_node_get_object (trimmed);

    /* geometry and properties.meta survive the trim. */
    PN_CHECK (json_object_has_member (o, "geometry"));
    props = json_object_get_object_member (o, "properties");
    PN_CHECK (json_object_has_member (props, "meta"));

    /* timeseries is truncated to just the current-hour entry... */
    series = json_object_get_array_member (props, "timeseries");
    PN_CHECK_CMPINT (json_array_get_length (series), ==, 1);

    /* ...and the surviving entry is the original first one. */
    first = json_array_get_object_element (series, 0);
    PN_CHECK_CMPSTR (json_object_get_string_member (first, "time"),
                     ==, "2026-06-05T06:00:00Z");

    /* The trim is a deep copy: the original node still has all 3 entries. */
    PN_CHECK_CMPINT (
        json_array_get_length (
            json_object_get_array_member (
                json_object_get_object_member (
                    json_node_get_object (root), "properties"),
                "timeseries")),
        ==, 3);

    json_node_unref (trimmed);
    g_object_unref (parser);
}

static void
test_met_no_trim_raw_short (void)
{
    /* A response already at or below one entry is returned intact (the
     * trim is a no-op rather than an error), and a non-object node is
     * passed through unchanged. */
    JsonParser *parser = json_parser_new ();
    JsonNode   *root, *trimmed;
    JsonArray  *series;

    g_assert (json_parser_load_from_data (parser,
        "{\"properties\":{\"timeseries\":["
          "{\"time\":\"2026-06-05T06:00:00Z\"}]}}", -1, NULL));
    root    = json_parser_get_root (parser);
    trimmed = pn_weather_met_no_trim_raw (root);

    series = json_object_get_array_member (
        json_object_get_object_member (
            json_node_get_object (trimmed), "properties"),
        "timeseries");
    PN_CHECK_CMPINT (json_array_get_length (series), ==, 1);

    json_node_unref (trimmed);
    g_object_unref (parser);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-weather");
    pn_test_add ("parse_full_reading",       test_parse_full_reading);
    pn_test_add ("parse_temperature_only",   test_parse_temperature_only);
    pn_test_add ("parse_error_reason",       test_parse_error_reason);
    pn_test_add ("parse_current_no_temp",    test_parse_current_without_temperature);
    pn_test_add ("parse_no_current_and_null", test_parse_no_current_and_null);
    pn_test_add ("code_description",         test_code_description);
    pn_test_add ("bright_sky_full_reading",  test_bright_sky_full_reading);
    pn_test_add ("bright_sky_temp_only",     test_bright_sky_temperature_only);
    pn_test_add ("bright_sky_detail_error",  test_bright_sky_detail_error);
    pn_test_add ("bright_sky_no_weather",    test_bright_sky_no_weather_and_null);
    pn_test_add ("bright_sky_description",   test_bright_sky_description);
    pn_test_add ("met_no_full_reading",      test_met_no_full_reading);
    pn_test_add ("met_no_temp_only",         test_met_no_temperature_only);
    pn_test_add ("met_no_six_hour_fallback", test_met_no_six_hour_fallback);
    pn_test_add ("met_no_no_timeseries",     test_met_no_no_timeseries_and_null);
    pn_test_add ("met_no_description",       test_met_no_description);
    pn_test_add ("met_no_trim_raw",          test_met_no_trim_raw);
    pn_test_add ("met_no_trim_raw_short",    test_met_no_trim_raw_short);
    return pn_test_run ();
}
