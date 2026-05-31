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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-geocode.h"
#include "pn-http.h"

#include <math.h>

#define PN_GEOCODE_URL  "https://geocoding-api.open-meteo.com/v1/search"

/* ------------------------------------------------------------------ */
/*  JSON helpers (local; the same shape #PnWeather uses)               */
/* ------------------------------------------------------------------ */

static const gchar *
obj_string (JsonObject *obj, const gchar *key)
{
    JsonNode *n;

    if (obj == NULL || !json_object_has_member (obj, key))
        return NULL;
    n = json_object_get_member (obj, key);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

static gdouble
obj_number (JsonObject *obj, const gchar *key, gdouble def)
{
    JsonNode *n;
    GType     t;

    if (obj == NULL || !json_object_has_member (obj, key))
        return def;
    n = json_object_get_member (obj, key);
    if (!JSON_NODE_HOLDS_VALUE (n))
        return def;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        return json_node_get_double (n);
    if (t == G_TYPE_INT64)
        return (gdouble) json_node_get_int (n);
    return def;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void
pn_geocode_result_clear (PnGeocodeResult *out)
{
    if (out == NULL)
        return;
    g_clear_pointer (&out->name, g_free);
    g_clear_pointer (&out->country, g_free);
    g_clear_pointer (&out->timezone, g_free);
    g_clear_pointer (&out->error, g_free);
    out->resolved  = FALSE;
    out->latitude  = 0.0;
    out->longitude = 0.0;
}

gboolean
pn_geocode_parse (JsonObject      *root,
                  const gchar     *city,
                  PnGeocodeResult *out)
{
    JsonNode   *rn;
    JsonArray  *results;
    JsonObject *hit;
    gdouble     lat, lon;

    g_return_val_if_fail (out != NULL, FALSE);

    out->resolved = FALSE;

    if (root == NULL || !json_object_has_member (root, "results"))
    {
        /* Open-Meteo answers an unknown place with HTTP 200 and an
         * absent (or empty) results array, not an error envelope. */
        out->error = g_strdup_printf (
                "Open-Meteo could not find a location named \"%s\".",
                city ? city : "");
        return FALSE;
    }

    rn = json_object_get_member (root, "results");
    if (!JSON_NODE_HOLDS_ARRAY (rn) ||
        json_array_get_length (json_node_get_array (rn)) == 0)
    {
        out->error = g_strdup_printf (
                "Open-Meteo could not find a location named \"%s\".",
                city ? city : "");
        return FALSE;
    }

    results = json_node_get_array (rn);
    hit     = json_array_get_object_element (results, 0);
    lat     = obj_number (hit, "latitude",  (gdouble) NAN);
    lon     = obj_number (hit, "longitude", (gdouble) NAN);

    if (!isfinite (lat) || !isfinite (lon))
    {
        out->error = g_strdup_printf (
                "Open-Meteo found a location for \"%s\" but it had no "
                "coordinates.", city ? city : "");
        return FALSE;
    }

    out->resolved  = TRUE;
    out->latitude  = lat;
    out->longitude = lon;
    out->name      = g_strdup (obj_string (hit, "name"));
    out->country   = g_strdup (obj_string (hit, "country"));
    out->timezone  = g_strdup (obj_string (hit, "timezone"));
    return TRUE;
}

/* Build a curl command that GETs @url and appends the HTTP-status
 * sentinel #PnHttp splits back off, so a request failure produces a
 * specific message.  Same shape as the #PnHttp family's own commands. */
static gchar *
geocode_curl_command (const gchar *url, guint timeout)
{
    gchar *quoted = g_shell_quote (url);
    gchar *cmd    = g_strdup_printf (
            "curl --silent --show-error --location "
            "--max-time %u "
            "--write-out '%s%%{http_code}' "
            "-H 'Accept: application/json' "
            "%s",
            timeout, PN_HTTP_STATUS_SENTINEL, quoted);

    g_free (quoted);
    return cmd;
}

/* First non-empty trimmed line of @text, or %NULL.  curl --show-error
 * writes a one-line cause to stderr on transport failure. */
static gchar *
first_line (const gchar *text)
{
    gchar **lines;
    gchar  *out = NULL;
    guint   i;

    if (text == NULL)
        return NULL;

    lines = g_strsplit (text, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        gchar *trimmed = g_strstrip (lines[i]);
        if (*trimmed != '\0')
        {
            out = g_strdup (trimmed);
            break;
        }
    }
    g_strfreev (lines);
    return out;
}

gboolean
pn_geocode_resolve_sync (const gchar     *city,
                         guint            timeout_seconds,
                         PnGeocodeResult *out)
{
    gchar    *escaped, *url, *cmd;
    gchar    *stdout_text = NULL, *stderr_text = NULL;
    gchar    *body = NULL;
    gint      exit_status = 0, http_status = 0;
    GError   *error = NULL;
    gboolean  spawned;

    g_return_val_if_fail (out != NULL, FALSE);
    g_return_val_if_fail (city != NULL && *city != '\0', FALSE);

    out->resolved = FALSE;

    escaped = g_uri_escape_string (city, NULL, FALSE);
    url     = g_strdup_printf ("%s?name=%s&count=1&language=en&format=json",
                               PN_GEOCODE_URL, escaped);
    cmd     = geocode_curl_command (url, timeout_seconds);

    spawned = g_spawn_command_line_sync (cmd, &stdout_text, &stderr_text,
                                         &exit_status, &error);

    if (!spawned)
    {
        out->error = error != NULL
                ? g_strdup_printf (
                        "Location lookup could not start curl: %s",
                        error->message)
                : g_strdup ("Location lookup could not start curl.");
    }
    else
    {
        pn_http_split_body_and_status (stdout_text, &body, &http_status);

        if (g_spawn_check_wait_status (exit_status, NULL) &&
            (http_status == 0 || (http_status >= 200 && http_status < 300)) &&
            body != NULL && *body != '\0')
        {
            JsonParser *parser = json_parser_new ();

            if (json_parser_load_from_data (parser, body, -1, NULL))
            {
                JsonNode   *root = json_parser_get_root (parser);
                JsonObject *o    = (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
                                   ? json_node_get_object (root) : NULL;
                pn_geocode_parse (o, city, out);
            }
            else
            {
                out->error = g_strdup (
                        "Location lookup returned a non-JSON response.");
            }
            g_object_unref (parser);
        }
        else if (http_status == 429)
        {
            out->error = g_strdup (
                    "Location lookup was rate-limited by the geocoding "
                    "service (HTTP 429).  Try a longer poll period.");
        }
        else if (http_status > 0)
        {
            out->error = g_strdup_printf (
                    "Location lookup failed (HTTP %d).", http_status);
        }
        else
        {
            gchar *cause = first_line (stderr_text);
            out->error = cause != NULL
                    ? g_strdup_printf ("Location lookup failed: %s", cause)
                    : g_strdup ("Location lookup could not reach the "
                                "geocoding service.");
            g_free (cause);
        }
    }

    g_clear_error (&error);
    g_free (body);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (cmd);
    g_free (url);
    g_free (escaped);

    return out->resolved;
}
