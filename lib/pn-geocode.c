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

#include <libsoup/soup.h>
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

gboolean
pn_geocode_resolve_sync (const gchar     *city,
                         guint            timeout_seconds,
                         PnGeocodeResult *out)
{
    gchar       *escaped, *url;
    SoupSession *session;
    SoupMessage *msg;
    GBytes      *bytes;
    GError      *error = NULL;

    g_return_val_if_fail (out != NULL, FALSE);
    g_return_val_if_fail (city != NULL && *city != '\0', FALSE);

    out->resolved = FALSE;

    escaped = g_uri_escape_string (city, NULL, FALSE);
    url     = g_strdup_printf ("%s?name=%s&count=1&language=en&format=json",
                               PN_GEOCODE_URL, escaped);

    msg = soup_message_new (SOUP_METHOD_GET, url);
    if (msg == NULL)
    {
        out->error = g_strdup ("Location lookup built an invalid URL.");
        g_free (url);
        g_free (escaped);
        return FALSE;
    }
    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "Accept", "application/json");

    /* Bound socket I/O so an unreachable geocoder cannot hang the
     * caller's worker thread; libsoup follows redirects by default. */
    session = soup_session_new ();
    soup_session_set_timeout (session, timeout_seconds);

    bytes = soup_session_send_and_read (session, msg, NULL, &error);

    if (bytes == NULL)
    {
        out->error = (error != NULL && error->message != NULL)
                ? g_strdup_printf ("Location lookup failed: %s",
                                   error->message)
                : g_strdup ("Location lookup could not reach the "
                            "geocoding service.");
    }
    else
    {
        gsize        len    = 0;
        const gchar *data   = g_bytes_get_data (bytes, &len);
        gint         status = (gint) soup_message_get_status (msg);

        if ((status == 0 || (status >= 200 && status < 300)) &&
            data != NULL && len > 0)
        {
            JsonParser *parser = json_parser_new ();

            if (json_parser_load_from_data (parser, data, (gssize) len, NULL))
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
        else if (status == 429)
        {
            out->error = g_strdup (
                    "Location lookup was rate-limited by the geocoding "
                    "service (HTTP 429).  Try a longer poll period.");
        }
        else if (status > 0)
        {
            out->error = g_strdup_printf (
                    "Location lookup failed (HTTP %d).", status);
        }
        else
        {
            out->error = g_strdup ("Location lookup could not reach the "
                                   "geocoding service.");
        }
        g_bytes_unref (bytes);
    }

    g_clear_error (&error);
    g_object_unref (msg);
    g_object_unref (session);
    g_free (url);
    g_free (escaped);

    return out->resolved;
}
