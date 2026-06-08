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

/* Unit tests for PnWeatherReport.  The cairo/Pango card painter lives in
 * the gui tier, so the headless-testable contract is the receive seam:
 * the node is a pure sink that deep-copies the incoming data bag into its
 * own snapshot (peek_data), and it mirrors the current conditions onto the
 * header glyph — sun/moon for clear sky honouring is_day, cloud/rain/snow/
 * storm for the WMO code, and the stable cloud fallback when the reading
 * carries no usable code or reports failure.  The display-unit / gradient /
 * colour properties and the fixed card geometry round-trip too. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-weather-report.h"

#include <json-glib/json-glib.h>

/* FontAwesome 4.7 glyphs the receive path selects, mirrored from
 * pn-weather-report.c as UTF-8 byte strings. */
#define WR_ICON       "\xef\x83\x82"  /* fa-cloud       U+F0C2 (fallback) */
#define ICON_SUN      "\xef\x86\x85"  /* fa-sun-o       U+F185 */
#define ICON_MOON     "\xef\x86\x86"  /* fa-moon-o      U+F186 */
#define ICON_CLOUD    "\xef\x83\x82"  /* fa-cloud       U+F0C2 */
#define ICON_UMBRELLA "\xef\x83\xa9"  /* fa-umbrella    U+F0E9 */
#define ICON_SNOW     "\xef\x8b\x9c"  /* fa-snowflake-o U+F2DC */
#define ICON_BOLT     "\xef\x83\xa7"  /* fa-bolt        U+F0E7 */

/* Build a message whose data bag is the bare object @json (deserialize
 * folds a bare object — no "data" member — wholesale into the data bag),
 * which is exactly the shape pn_weather_report_receive() consumes. */
static PnMessage *
message_from_data (const gchar *json)
{
    PnMessage *m = pn_message_deserialize (json, NULL);
    g_assert (m != NULL);
    return m;
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_weather_report_new ());
    PnMessage *msg   = message_from_data (
        "{\"success\":true,\"weather_code\":0,\"temperature\":18.0}");

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* A reading repaints the card but never forwards a message. */
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE (pn_node_get_has_output (node));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_peek_data_snapshot (void)
{
    PnNode          *node = PN_NODE (pn_weather_report_new ());
    PnWeatherReport *wr   = PN_WEATHER_REPORT (node);
    PnMessage       *msg;
    JsonObject      *snap;

    /* No reading yet: the snapshot is NULL. */
    PN_CHECK (pn_weather_report_peek_data (wr) == NULL);

    msg = message_from_data (
        "{\"success\":true,\"weather_code\":3,\"city\":\"Pecs\","
        "\"temperature\":21.5}");
    pn_node_receive_message (node, msg);

    snap = pn_weather_report_peek_data (wr);
    PN_CHECK (snap != NULL);
    PN_CHECK (json_object_has_member (snap, "city"));
    PN_CHECK_CMPSTR (json_object_get_string_member (snap, "city"), ==, "Pecs");

    /* The node holds its own ref on the reading, so the snapshot stays
     * readable after the delivering message is dropped (the contract the
     * painter relies on between weather refreshes). */
    g_object_unref (msg);
    snap = pn_weather_report_peek_data (wr);
    PN_CHECK (snap != NULL);
    PN_CHECK_CMPSTR (json_object_get_string_member (snap, "city"), ==, "Pecs");

    /* A second reading replaces the first wholesale. */
    msg = message_from_data ("{\"success\":true,\"city\":\"Wien\"}");
    pn_node_receive_message (node, msg);
    snap = pn_weather_report_peek_data (wr);
    PN_CHECK_CMPSTR (json_object_get_string_member (snap, "city"), ==, "Wien");
    PN_CHECK_FALSE (json_object_has_member (snap, "weather_code"));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_icon_clear_day_night (void)
{
    PnNode    *node = PN_NODE (pn_weather_report_new ());
    PnMessage *day;
    PnMessage *night;

    /* Clear sky (code 0/1) shows the sun by day and the moon by night;
     * is_day rides on the raw Open-Meteo passthrough at raw/current. */
    day = message_from_data (
        "{\"success\":true,\"weather_code\":0,"
        "\"raw\":{\"current\":{\"is_day\":true}}}");
    pn_node_receive_message (node, day);
    PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, ICON_SUN);
    g_object_unref (day);

    night = message_from_data (
        "{\"success\":true,\"weather_code\":1,"
        "\"raw\":{\"current\":{\"is_day\":false}}}");
    pn_node_receive_message (node, night);
    PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, ICON_MOON);
    g_object_unref (night);

    /* is_day defaults to true when the raw passthrough is absent. */
    {
        PnMessage *bare = message_from_data (
            "{\"success\":true,\"weather_code\":0}");
        pn_node_receive_message (node, bare);
        PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, ICON_SUN);
        g_object_unref (bare);
    }

    g_object_unref (node);
}

static void
test_icon_by_code (void)
{
    /* The WMO code maps onto the headline glyph: overcast -> cloud,
     * drizzle/rain/showers -> umbrella, snow -> snowflake, storm -> bolt. */
    const struct { const gchar *code; const gchar *glyph; } cases[] = {
        { "3",  ICON_CLOUD    },  /* overcast            */
        { "45", ICON_CLOUD    },  /* fog                 */
        { "51", ICON_UMBRELLA },  /* drizzle             */
        { "63", ICON_UMBRELLA },  /* rain                */
        { "80", ICON_UMBRELLA },  /* rain showers        */
        { "71", ICON_SNOW     },  /* snow fall           */
        { "85", ICON_SNOW     },  /* snow showers        */
        { "95", ICON_BOLT     },  /* thunderstorm        */
    };

    for (guint i = 0; i < G_N_ELEMENTS (cases); ++i)
    {
        PnNode *node = PN_NODE (pn_weather_report_new ());
        gchar  *json = g_strdup_printf (
            "{\"success\":true,\"weather_code\":%s}", cases[i].code);
        PnMessage *msg = message_from_data (json);

        pn_node_receive_message (node, msg);
        PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, cases[i].glyph);

        g_object_unref (msg);
        g_free (json);
        g_object_unref (node);
    }
}

static void
test_icon_fallback (void)
{
    PnNode    *node = PN_NODE (pn_weather_report_new ());
    PnMessage *no_code;
    PnMessage *failed;

    /* A reading with no weather_code falls back to the stable cloud. */
    no_code = message_from_data (
        "{\"success\":true,\"city\":\"Nowhere\"}");
    pn_node_receive_message (node, no_code);
    PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, WR_ICON);
    g_object_unref (no_code);

    /* success=false suppresses the conditions glyph even when a code is
     * present, reverting to the fallback cloud. */
    failed = message_from_data (
        "{\"success\":false,\"weather_code\":95,"
        "\"output\":\"No matching location found\"}");
    pn_node_receive_message (node, failed);
    PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, WR_ICON);
    g_object_unref (failed);

    g_object_unref (node);
}

static void
test_empty_data_is_safe (void)
{
    PnNode    *node = PN_NODE (pn_weather_report_new ());
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* An empty data bag carries no code and no success flag (defaults to
     * true): the node stores the empty snapshot and paints the fallback
     * cloud without crashing. */
    pn_node_receive_message (node, msg);
    PN_CHECK (pn_weather_report_peek_data (PN_WEATHER_REPORT (node)) != NULL);
    PN_CHECK_CMPSTR (pn_node_get_icon (node), ==, WR_ICON);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_unit_props_default_round_trip (void)
{
    PnNode        *node  = PN_NODE (pn_weather_report_new ());
    PnWrTempUnit   temp  = PN_WR_TEMP_KELVIN;
    PnWrWindUnit   wind  = PN_WR_WIND_KNOTS;
    PnWrPressUnit  press = PN_WR_PRESS_MMHG;
    PnWrGradient   grad  = PN_WR_GRADIENT_DIAGONAL;
    gboolean       det   = FALSE;

    /* Metric defaults mirror what the Weather node reports. */
    g_object_get (node,
                  "temperature-unit", &temp,
                  "wind-unit",        &wind,
                  "pressure-unit",    &press,
                  "background-gradient", &grad,
                  "show-details",     &det,
                  NULL);
    PN_CHECK_CMPINT (temp,  ==, PN_WR_TEMP_CELSIUS);
    PN_CHECK_CMPINT (wind,  ==, PN_WR_WIND_KMH);
    PN_CHECK_CMPINT (press, ==, PN_WR_PRESS_HPA);
    PN_CHECK_CMPINT (grad,  ==, PN_WR_GRADIENT_NONE);
    PN_CHECK (det == TRUE);

    /* Each enum round-trips through its property. */
    g_object_set (node,
                  "temperature-unit",    PN_WR_TEMP_FAHRENHEIT,
                  "wind-unit",           PN_WR_WIND_MPH,
                  "pressure-unit",       PN_WR_PRESS_INHG,
                  "background-gradient", PN_WR_GRADIENT_VERTICAL,
                  "show-details",        FALSE,
                  NULL);
    g_object_get (node,
                  "temperature-unit", &temp,
                  "wind-unit",        &wind,
                  "pressure-unit",    &press,
                  "background-gradient", &grad,
                  "show-details",     &det,
                  NULL);
    PN_CHECK_CMPINT (temp,  ==, PN_WR_TEMP_FAHRENHEIT);
    PN_CHECK_CMPINT (wind,  ==, PN_WR_WIND_MPH);
    PN_CHECK_CMPINT (press, ==, PN_WR_PRESS_INHG);
    PN_CHECK_CMPINT (grad,  ==, PN_WR_GRADIENT_VERTICAL);
    PN_CHECK (det == FALSE);

    g_object_unref (node);
}

static void
test_color_props_round_trip (void)
{
    PnNode  *node = PN_NODE (pn_weather_report_new ());
    PnColor  ink  = { 0.10, 0.20, 0.30, 1.0 };
    PnColor *out  = NULL;

    g_object_set (node, "font-color", &ink, NULL);
    g_object_get (node, "font-color", &out, NULL);
    PN_CHECK (out != NULL && pn_color_equal (out, &ink));
    g_clear_pointer (&out, pn_color_free);

    {
        PnColor bg = { 0.40, 0.50, 0.60, 0.8 };
        g_object_set (node, "background-color2", &bg, NULL);
        g_object_get (node, "background-color2", &out, NULL);
        PN_CHECK (out != NULL && pn_color_equal (out, &bg));
        g_clear_pointer (&out, pn_color_free);
    }

    g_object_unref (node);
}

static void
test_fixed_geometry (void)
{
    PnNode *node = PN_NODE (pn_weather_report_new ());
    double  w = 0, h = 0;

    /* The card re-uses the graph footprint: 280 wide, 40 header + 4 gap +
     * 173 card = 217 tall, independent of the reading. */
    pn_node_get_size (node, &w, &h);
    PN_CHECK_NEAR (w, 280.0, 1e-9);
    PN_CHECK_NEAR (h, 217.0, 1e-9);
    PN_CHECK_NEAR (pn_node_get_header_height (node), 40.0, 1e-9);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-weather-report");
    pn_test_add ("is_a_sink",              test_is_a_sink);
    pn_test_add ("peek_data_snapshot",     test_peek_data_snapshot);
    pn_test_add ("icon_clear_day_night",   test_icon_clear_day_night);
    pn_test_add ("icon_by_code",           test_icon_by_code);
    pn_test_add ("icon_fallback",          test_icon_fallback);
    pn_test_add ("empty_data_is_safe",     test_empty_data_is_safe);
    pn_test_add ("unit_props_round_trip",  test_unit_props_default_round_trip);
    pn_test_add ("color_props_round_trip", test_color_props_round_trip);
    pn_test_add ("fixed_geometry",         test_fixed_geometry);
    return pn_test_run ();
}
