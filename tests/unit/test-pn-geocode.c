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

/* Unit tests for the shared geocoder's pure parse seam,
 * pn_geocode_parse() — the network half (pn_geocode_resolve_sync) does
 * I/O and is exercised live in the app instead. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-geocode.h"

/* Parse @text and return its root object, keeping the owning parser in
 * *@keep so the caller can unref it after inspecting the result. */
static JsonObject *
root_object (const gchar *text, JsonParser **keep)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;

    g_assert (json_parser_load_from_data (parser, text, -1, NULL));
    root  = json_parser_get_root (parser);
    *keep = parser;
    return JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

static void
test_parse_first_hit (void)
{
    JsonParser      *keep = NULL;
    PnGeocodeResult  r    = { 0 };
    JsonObject      *o    = root_object (
            "{\"results\":[{\"name\":\"Berlin\",\"country\":\"Germany\","
            "\"timezone\":\"Europe/Berlin\","
            "\"latitude\":52.52437,\"longitude\":13.41053},"
            "{\"name\":\"Berlin\",\"country\":\"United States\","
            "\"latitude\":44.46,\"longitude\":-71.18}]}", &keep);

    PN_CHECK       (pn_geocode_parse (o, "Berlin", &r));
    PN_CHECK       (r.resolved);
    PN_CHECK_NEAR  (r.latitude,  52.52437, 1e-6);
    PN_CHECK_NEAR  (r.longitude, 13.41053, 1e-6);
    PN_CHECK_CMPSTR (r.name,     ==, "Berlin");
    PN_CHECK_CMPSTR (r.country,  ==, "Germany");
    PN_CHECK_CMPSTR (r.timezone, ==, "Europe/Berlin");
    PN_CHECK       (r.error == NULL);

    pn_geocode_result_clear (&r);
    g_object_unref (keep);
}

static void
test_parse_no_results_member (void)
{
    JsonParser      *keep = NULL;
    PnGeocodeResult  r    = { 0 };
    JsonObject      *o    = root_object ("{\"generationtime_ms\":0.21}", &keep);

    /* Open-Meteo's "place not found" answer is an absent results array. */
    PN_CHECK_FALSE (pn_geocode_parse (o, "Nowhereville", &r));
    PN_CHECK_FALSE (r.resolved);
    PN_CHECK       (r.error != NULL);

    pn_geocode_result_clear (&r);
    g_object_unref (keep);
}

static void
test_parse_empty_results (void)
{
    JsonParser      *keep = NULL;
    PnGeocodeResult  r    = { 0 };
    JsonObject      *o    = root_object ("{\"results\":[]}", &keep);

    PN_CHECK_FALSE (pn_geocode_parse (o, "Atlantis", &r));
    PN_CHECK       (r.error != NULL);

    pn_geocode_result_clear (&r);
    g_object_unref (keep);
}

static void
test_parse_hit_without_coords (void)
{
    JsonParser      *keep = NULL;
    PnGeocodeResult  r    = { 0 };
    JsonObject      *o    = root_object (
            "{\"results\":[{\"name\":\"Ghost\"}]}", &keep);

    PN_CHECK_FALSE (pn_geocode_parse (o, "Ghost", &r));
    PN_CHECK       (r.error != NULL);

    pn_geocode_result_clear (&r);
    g_object_unref (keep);
}

static void
test_parse_null_root (void)
{
    PnGeocodeResult r = { 0 };

    /* A NULL root (un-parseable body) is a safe failure, not a crash. */
    PN_CHECK_FALSE (pn_geocode_parse (NULL, "X", &r));
    PN_CHECK       (r.error != NULL);

    pn_geocode_result_clear (&r);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-geocode");
    pn_test_add ("parse_first_hit",         test_parse_first_hit);
    pn_test_add ("parse_no_results_member", test_parse_no_results_member);
    pn_test_add ("parse_empty_results",     test_parse_empty_results);
    pn_test_add ("parse_hit_without_coords", test_parse_hit_without_coords);
    pn_test_add ("parse_null_root",         test_parse_null_root);
    return pn_test_run ();
}
