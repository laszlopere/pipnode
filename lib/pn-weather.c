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
/*  Polls current weather for a named place from Open-Meteo, a free,   */
/*  key-less API.  Open-Meteo's forecast endpoint is keyed on          */
/*  latitude/longitude, so a city name first has to be resolved to     */
/*  coordinates through the separate geocoding endpoint.  That is two   */
/*  HTTP requests, which does not fit #PnHttp's one-curl-per-tick       */
/*  build_command/emit_message contract, so the trigger is overridden: */
/*  it geocodes (caching the result until the city changes) and then    */
/*  delegates to #PnHttp's own trigger for the forecast fetch, reusing  */
/*  the curl spawn loop via build_command()/emit_message().            */
/*                                                                     */
/*  Emitted message (data bag):                                        */
/*    success      - whether a current reading was obtained            */
/*    value        - current temperature in °C (the canonical value)   */
/*    temperature  - same as value, explicitly named                   */
/*    humidity     - relative humidity, %                              */
/*    wind_speed   - wind speed at 10 m, km/h                          */
/*    weather_code - WMO weather code (integer)                        */
/*    description  - human label for the weather code                  */
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

#include <json-glib/json-glib.h>
#include <math.h>

/* fa-cloud U+F0C2 — a plain cloud reads as "weather". */
#define PN_WEATHER_ICON            "\xef\x83\x82"

#define PN_WEATHER_FORECAST_URL    "https://api.open-meteo.com/v1/forecast"
#define PN_WEATHER_GEOCODE_URL     "https://geocoding-api.open-meteo.com/v1/search"

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

    /* Cached geocoding result.  @geo_key is the city string the cached
     * coordinates belong to; when it differs from @city (or @have_coords
     * is FALSE) the next tick re-geocodes. */
    gchar   *geo_key;
    gboolean have_coords;
    gdouble  latitude;
    gdouble  longitude;
    gchar   *resolved_name; /* canonical place name from the geocoder    */
    gchar   *country;
};

G_DEFINE_TYPE (PnWeather, pn_weather, PN_TYPE_HTTP)

enum {
    PROP_0,
    PROP_CITY,
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
static const gchar *
weather_code_description (gint code)
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

/* Build a curl command that GETs @url and prints the body.  Shared
 * shape for both the geocoding and (via build_command) forecast calls. */
static gchar *
weather_curl_command (const gchar *url, guint timeout)
{
    gchar *quoted = g_shell_quote (url);
    gchar *cmd    = g_strdup_printf (
            "curl --silent --show-error --location "
            "--max-time %u "
            "-H 'Accept: application/json' "
            "%s",
            timeout, quoted);

    g_free (quoted);
    return cmd;
}

/* ------------------------------------------------------------------ */
/*  Geocoding (worker thread)                                          */
/* ------------------------------------------------------------------ */

/* Resolve @city to coordinates via Open-Meteo's geocoding endpoint and
 * cache the result.  On success the cache is keyed on @city so a steady
 * city is geocoded only once; on failure the cache is invalidated so a
 * later tick retries (a transient outage or a typo being corrected
 * should both recover without re-opening the node). */
static void
weather_geocode (PnWeather *self, const gchar *city)
{
    gchar      *escaped = g_uri_escape_string (city, NULL, FALSE);
    gchar      *url     = g_strdup_printf (
            "%s?name=%s&count=1&language=en&format=json",
            PN_WEATHER_GEOCODE_URL, escaped);
    gchar      *cmd     = weather_curl_command (url, 10u);
    gchar      *out     = NULL;
    gchar      *err     = NULL;
    gint        status  = 0;
    GError     *error   = NULL;
    gboolean    spawned;
    gboolean    resolved = FALSE;
    gdouble     lat = 0.0, lon = 0.0;
    gchar      *name = NULL;
    gchar      *country = NULL;

    spawned = g_spawn_command_line_sync (cmd, &out, &err, &status, &error);

    if (spawned && g_spawn_check_wait_status (status, NULL) &&
        out != NULL && *out != '\0')
    {
        JsonParser *parser = json_parser_new ();

        if (json_parser_load_from_data (parser, out, -1, NULL))
        {
            JsonNode *root = json_parser_get_root (parser);

            if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            {
                JsonObject *obj = json_node_get_object (root);

                if (json_object_has_member (obj, "results"))
                {
                    JsonNode *rn = json_object_get_member (obj, "results");

                    if (JSON_NODE_HOLDS_ARRAY (rn) &&
                        json_array_get_length (json_node_get_array (rn)) > 0)
                    {
                        JsonObject *hit = json_array_get_object_element (
                                json_node_get_array (rn), 0);

                        lat = obj_number (hit, "latitude",  (gdouble) NAN);
                        lon = obj_number (hit, "longitude", (gdouble) NAN);

                        if (isfinite (lat) && isfinite (lon))
                        {
                            resolved = TRUE;
                            if (json_object_has_member (hit, "name"))
                                name = g_strdup (json_object_get_string_member (
                                                     hit, "name"));
                            if (json_object_has_member (hit, "country"))
                                country = g_strdup (
                                        json_object_get_string_member (
                                                hit, "country"));
                        }
                    }
                }
            }
        }
        g_object_unref (parser);
    }
    else if (!spawned)
    {
        g_warning ("pn-weather: failed to spawn geocoding curl: %s",
                   error ? error->message : "(unknown)");
    }

    g_mutex_lock (&self->mutex);
    g_free (self->resolved_name);
    g_free (self->country);
    g_free (self->geo_key);
    if (resolved)
    {
        self->have_coords   = TRUE;
        self->latitude      = lat;
        self->longitude     = lon;
        self->resolved_name = name ? name : g_strdup (city);
        self->country       = country;        /* may be NULL */
        self->geo_key       = g_strdup (city);
        name = NULL;
        country = NULL;
    }
    else
    {
        self->have_coords   = FALSE;
        self->resolved_name = NULL;
        self->country       = NULL;
        self->geo_key       = NULL;           /* force a retry next tick */
    }
    g_mutex_unlock (&self->mutex);

    g_clear_error (&error);
    g_free (name);
    g_free (country);
    g_free (out);
    g_free (err);
    g_free (cmd);
    g_free (url);
    g_free (escaped);
}

/* Ensure cached coordinates match the configured city, geocoding when
 * needed.  Returns %TRUE when coordinates are available. */
static gboolean
weather_ensure_coords (PnWeather *self, const gchar *city)
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

    if (!weather_ensure_coords (self, city))
    {
        /* The place could not be resolved: surface a failure message so
         * a downstream Debug / Text View tells the user, rather than
         * going silent. */
        PnMessage *msg     = pn_message_new (PN_NODE (self), NULL);
        gchar     *summary = g_strdup_printf (
                "Could not find a location named \"%s\".", city);

        pn_message_set_string  (msg, "city",    city);
        pn_message_set_boolean (msg, "success", FALSE);
        pn_message_set_string  (msg, "output",  summary);
        pn_auto_trigger_emit_on_main (trigger, msg);

        g_free (summary);
        g_free (city);
        return;
    }

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
    PnWeather *self = PN_WEATHER (http);
    gchar     *base = pn_http_dup_url (http);
    gchar      latbuf[G_ASCII_DTOSTR_BUF_SIZE];
    gchar      lonbuf[G_ASCII_DTOSTR_BUF_SIZE];
    gdouble    lat, lon;
    gchar     *full;
    gchar     *cmd;

    g_mutex_lock (&self->mutex);
    lat = self->latitude;
    lon = self->longitude;
    g_mutex_unlock (&self->mutex);

    /* Locale-independent formatting: the API wants a '.' decimal point
     * regardless of the host's LC_NUMERIC. */
    g_ascii_dtostr (latbuf, sizeof latbuf, lat);
    g_ascii_dtostr (lonbuf, sizeof lonbuf, lon);

    /* timezone=auto makes Open-Meteo resolve the location's own timezone
     * and stamp current.time in that local time; without it the API
     * defaults to GMT, so the report's clock reads UTC. */
    full = g_strdup_printf (
            "%s?latitude=%s&longitude=%s&current=%s&timezone=auto",
            base ? base : PN_WEATHER_FORECAST_URL,
            latbuf, lonbuf, PN_WEATHER_CURRENT_VARS);
    cmd = weather_curl_command (full, timeout);

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
    const gchar *body    = stdout_text;
    gboolean     fetched;
    gboolean     ok      = FALSE;
    gchar       *reason  = NULL;
    PnMessage   *msg;
    gchar       *name, *country;
    gdouble      lat, lon;

    (void) stderr_text;

    /* Snapshot the resolved place for labelling the message. */
    g_mutex_lock (&self->mutex);
    name    = g_strdup (self->resolved_name);
    country = g_strdup (self->country);
    lat     = self->latitude;
    lon     = self->longitude;
    g_mutex_unlock (&self->mutex);

    /* This subclass builds the curl command itself and deliberately does
     * not append the HTTP-status sentinel that #PnHttp's default fetch
     * relies on, so the raw stdout is simply the JSON body.  Open-Meteo
     * answers HTTP 200 with the reading on success and a
     * {"error":true,"reason":...} body on failure, so the parse below —
     * not the HTTP status — decides success. */
    fetched = spawned && g_spawn_check_wait_status (exit_status, NULL);

    msg = pn_message_new (node, NULL);
    if (name != NULL)
        pn_message_set_string (msg, "city", name);
    if (country != NULL)
        pn_message_set_string (msg, "country", country);
    pn_message_set_double (msg, "latitude",  lat);
    pn_message_set_double (msg, "longitude", lon);

    if (fetched && body != NULL && *body != '\0')
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
             * promote to a named member. */
            if (o != NULL)
                pn_message_set_member (msg, "raw", json_node_copy (root));

            /* Open-Meteo reports problems as {"error":true,"reason":...}. */
            if (o != NULL && json_object_has_member (o, "reason"))
            {
                JsonNode *rn = json_object_get_member (o, "reason");
                if (JSON_NODE_HOLDS_VALUE (rn) &&
                    json_node_get_value_type (rn) == G_TYPE_STRING)
                    reason = g_strdup (json_node_get_string (rn));
            }

            if (o != NULL && json_object_has_member (o, "current"))
            {
                JsonNode *cn = json_object_get_member (o, "current");

                if (JSON_NODE_HOLDS_OBJECT (cn))
                {
                    JsonObject *cur  = json_node_get_object (cn);
                    gdouble     temp = obj_number (cur, "temperature_2m",
                                                   (gdouble) NAN);
                    gdouble     hum  = obj_number (cur, "relative_humidity_2m",
                                                   (gdouble) NAN);
                    gdouble     wind = obj_number (cur, "wind_speed_10m",
                                                   (gdouble) NAN);
                    gdouble     code = obj_number (cur, "weather_code",
                                                   (gdouble) NAN);

                    if (isfinite (temp))
                    {
                        GString *summary = g_string_new (NULL);

                        ok = TRUE;
                        pn_message_set_double (msg, "value",       temp);
                        pn_message_set_double (msg, "temperature", temp);
                        if (isfinite (hum))
                            pn_message_set_double (msg, "humidity", hum);
                        if (isfinite (wind))
                            pn_message_set_double (msg, "wind_speed", wind);

                        if (name != NULL)
                            g_string_append (summary, name);
                        if (country != NULL && *country != '\0')
                            g_string_append_printf (summary, ", %s", country);
                        if (summary->len > 0)
                            g_string_append (summary, ": ");
                        g_string_append_printf (summary, "%.1f \xc2\xb0""C",
                                                temp);

                        if (isfinite (code))
                        {
                            const gchar *desc =
                                    weather_code_description ((gint) code);
                            pn_message_set_int    (msg, "weather_code",
                                                   (gint) code);
                            pn_message_set_string (msg, "description", desc);
                            g_string_append_printf (summary, ", %s", desc);
                        }
                        if (isfinite (hum))
                            g_string_append_printf (summary, ", %.0f%% RH",
                                                    hum);
                        if (isfinite (wind))
                            g_string_append_printf (summary,
                                                    ", wind %.1f km/h", wind);

                        pn_message_set_string (msg, "output", summary->str);
                        g_string_free (summary, TRUE);
                    }
                }
            }
        }
        g_object_unref (parser);
    }

    pn_message_set_boolean (msg, "success", ok);
    if (!ok)
    {
        gchar *summary = (reason != NULL)
                ? g_strdup_printf ("Weather request failed: %s", reason)
                : g_strdup_printf ("Failed to read weather for %s.",
                                   name ? name : "the configured location");
        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (PN_AUTO_TRIGGER (self), msg);

    g_free (reason);
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
             * so the next tick re-geocodes. */
            self->have_coords = FALSE;
            g_clear_pointer (&self->geo_key, g_free);
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
    node_class->color        = (GdkRGBA){ 0.30, 0.60, 0.85, 1.0 };
    node_class->category     = "Network";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;
    http_class->normal_icon  = PN_WEATHER_ICON;
    http_class->normal_color = (GdkRGBA){ 0.30, 0.60, 0.85, 1.0 };

    props[PROP_CITY] = g_param_spec_string (
            "city", "City",
            "Place name to fetch weather for (e.g. \"Berlin\" or "
            "\"Berlin, US\").  Resolved to coordinates via Open-Meteo's "
            "geocoding service before each forecast fetch.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_weather_init (PnWeather *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  sky  = { 0.30, 0.60, 0.85, 1.0 };

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
