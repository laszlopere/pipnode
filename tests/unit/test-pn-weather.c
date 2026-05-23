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
    return pn_test_run ();
}
