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
/*  PnAstronomical                                                     */
/*                                                                     */
/*  See pn-astronomical.h for the node contract.  The Sun's position   */
/*  and the day's events are computed with the NOAA solar-position     */
/*  equations (the same maths behind NOAA's published spreadsheet),    */
/*  accurate to well under a minute for any year near the present —     */
/*  more than enough for a "is the Sun up?" signal.  Sunrise/sunset     */
/*  are reported for the UTC calendar day of the queried instant.      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-astronomical.h"
#include "pn-message.h"
#include "pn-geocode.h"

#include <math.h>

/* fa-sun-o U+F185 — the open sun glyph; FontAwesome 4.7. */
#define PN_ASTRONOMICAL_ICON      "\xef\x86\x85"

#define PN_ASTRONOMICAL_DEFAULT_PERIOD  60u

#define DEG2RAD (G_PI / 180.0)
#define RAD2DEG (180.0 / G_PI)

/* Refraction-corrected horizon: the Sun's centre sits 0.833° below the
 * true horizon when its upper limb touches it — the standard
 * sunrise/sunset altitude, so sun_up agrees with the reported times. */
#define SUN_HORIZON_DEG  (-0.833)

/* ------------------------------------------------------------------ */
/*  Pure solar computation                                             */
/* ------------------------------------------------------------------ */

void
pn_astronomical_compute (gdouble               lat_deg,
                         gdouble               lon_deg,
                         gint64                unix_time,
                         PnAstronomicalResult *out)
{
    gdouble jd, T, L0, M, e, Mrad, C, true_long, omega, lambda;
    gdouble seconds, eps0, eps, eps_rad, lambda_rad, decl, y, L0rad, eot;
    gdouble lat_rad, minutes_utc, tst, ha, ha_rad, cos_zen, zenith, sin_zen;
    gdouble altitude, azimuth, cosH, noon_min;
    gint64  secs_into_day, day_start;

    g_return_if_fail (out != NULL);

    /* Julian day / century for the instant. */
    jd = (gdouble) unix_time / 86400.0 + 2440587.5;
    T  = (jd - 2451545.0) / 36525.0;

    /* Geometric mean longitude (deg, 0..360) and anomaly of the Sun. */
    L0 = fmod (280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
    if (L0 < 0.0)
        L0 += 360.0;
    M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);

    Mrad = M * DEG2RAD;
    C = sin (Mrad)       * (1.914602 - T * (0.004817 + 0.000014 * T))
      + sin (2.0 * Mrad) * (0.019993 - 0.000101 * T)
      + sin (3.0 * Mrad) * 0.000289;

    true_long = L0 + C;
    omega     = 125.04 - 1934.136 * T;
    lambda    = true_long - 0.00569 - 0.00478 * sin (omega * DEG2RAD);

    /* Obliquity of the ecliptic, corrected for nutation. */
    seconds = 21.448 - T * (46.815 + T * (0.00059 - T * 0.001813));
    eps0    = 23.0 + (26.0 + seconds / 60.0) / 60.0;
    eps     = eps0 + 0.00256 * cos (omega * DEG2RAD);

    eps_rad    = eps * DEG2RAD;
    lambda_rad = lambda * DEG2RAD;

    /* Solar declination (radians). */
    decl = asin (sin (eps_rad) * sin (lambda_rad));

    /* Equation of time (minutes). */
    y     = tan (eps_rad / 2.0);
    y    *= y;
    L0rad = L0 * DEG2RAD;
    eot   = 4.0 * RAD2DEG * (
                  y * sin (2.0 * L0rad)
                - 2.0 * e * sin (Mrad)
                + 4.0 * e * y * sin (Mrad) * cos (2.0 * L0rad)
                - 0.5 * y * y * sin (4.0 * L0rad)
                - 1.25 * e * e * sin (2.0 * Mrad));

    lat_rad = lat_deg * DEG2RAD;

    /* Split the instant into UTC-day boundary + minutes past midnight. */
    secs_into_day = unix_time % 86400;
    if (secs_into_day < 0)
        secs_into_day += 86400;
    day_start   = unix_time - secs_into_day;
    minutes_utc = (gdouble) secs_into_day / 60.0;

    /* --- Current solar position. --- */
    tst = fmod (minutes_utc + eot + 4.0 * lon_deg, 1440.0);
    if (tst < 0.0)
        tst += 1440.0;
    ha     = tst / 4.0 - 180.0;          /* hour angle, degrees */
    ha_rad = ha * DEG2RAD;

    cos_zen = sin (lat_rad) * sin (decl)
            + cos (lat_rad) * cos (decl) * cos (ha_rad);
    cos_zen = CLAMP (cos_zen, -1.0, 1.0);
    zenith  = acos (cos_zen);            /* radians */
    altitude = 90.0 - zenith * RAD2DEG;

    sin_zen = sin (zenith);
    if (sin_zen < 1e-9)
    {
        azimuth = 0.0;                   /* Sun overhead; azimuth undefined */
    }
    else
    {
        gdouble cos_az = (sin (lat_rad) * cos (zenith) - sin (decl))
                       / (cos (lat_rad) * sin_zen);
        gdouble az;

        cos_az = CLAMP (cos_az, -1.0, 1.0);
        az     = acos (cos_az) * RAD2DEG;
        azimuth = (ha > 0.0) ? fmod (az + 180.0, 360.0)
                             : fmod (540.0 - az, 360.0);
    }

    out->altitude = altitude;
    out->azimuth  = azimuth;
    out->sun_up   = (altitude > SUN_HORIZON_DEG);

    /* --- Sunrise / sunset / solar noon / day length. --- */
    cosH = cos ((90.0 - SUN_HORIZON_DEG) * DEG2RAD)
                 / (cos (lat_rad) * cos (decl))
         - tan (lat_rad) * tan (decl);

    noon_min        = 720.0 - 4.0 * lon_deg - eot;   /* minutes past UTC midnight */
    out->solar_noon = (gdouble) day_start + noon_min * 60.0;

    if (cosH > 1.0)
    {
        /* The Sun never climbs to the horizon — polar night. */
        out->has_sunrise = FALSE;
        out->has_sunset  = FALSE;
        out->day_length  = 0.0;
    }
    else if (cosH < -1.0)
    {
        /* The Sun never dips to the horizon — polar day. */
        out->has_sunrise = FALSE;
        out->has_sunset  = FALSE;
        out->day_length  = 24.0;
    }
    else
    {
        gdouble H = acos (cosH) * RAD2DEG;   /* half-day arc, degrees */

        out->has_sunrise = TRUE;
        out->has_sunset  = TRUE;
        out->sunrise     = (gdouble) day_start + (noon_min - 4.0 * H) * 60.0;
        out->sunset      = (gdouble) day_start + (noon_min + 4.0 * H) * 60.0;
        out->day_length  = (8.0 * H) / 60.0; /* hours */
    }
}

/* ------------------------------------------------------------------ */
/*  Node                                                               */
/* ------------------------------------------------------------------ */

struct _PnAstronomical
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads these while the main thread may overwrite
     * them via property setters; @mutex serialises the two. */
    GMutex               mutex;
    gchar               *city;
    PnAstronomicalDatum  datum;

    /* Cached geocoding result, keyed on @geo_key (the city it was
     * resolved from).  When the city changes the next tick re-geocodes. */
    gboolean             have_coords;
    gdouble              latitude;
    gdouble              longitude;
    gchar               *resolved_name;
    gchar               *country;
    gchar               *timezone;    /* IANA tz of the resolved place */
    gchar               *geo_key;
    gchar               *geo_error;   /* reason the last lookup failed */
};

G_DEFINE_TYPE (PnAstronomical, pn_astronomical, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_CITY,
    PROP_DATUM,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* The "datum" enum, registered so the settings dialog renders it as a
 * combobox.  The nicks (third column) are the labels shown and the
 * strings serialised into the saved worksheet, so keep them stable. */
#define PN_TYPE_ASTRONOMICAL_DATUM (pn_astronomical_datum_get_type ())

static GType
pn_astronomical_datum_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_ASTRO_DATUM_SUN_IS_UP,    "PN_ASTRO_DATUM_SUN_IS_UP",
              "The Sun is up"           },
            { PN_ASTRO_DATUM_SUN_IS_DOWN,  "PN_ASTRO_DATUM_SUN_IS_DOWN",
              "The Sun is down"         },
            { PN_ASTRO_DATUM_SUN_ALTITUDE, "PN_ASTRO_DATUM_SUN_ALTITUDE",
              "Sun altitude (degrees)"  },
            { PN_ASTRO_DATUM_SUN_AZIMUTH,  "PN_ASTRO_DATUM_SUN_AZIMUTH",
              "Sun azimuth (degrees)"   },
            { PN_ASTRO_DATUM_DAY_LENGTH,   "PN_ASTRO_DATUM_DAY_LENGTH",
              "Day length (hours)"      },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnAstronomicalDatum", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

/* ------------------------------------------------------------------ */
/*  Geocoding (worker thread)                                          */
/* ------------------------------------------------------------------ */

/* Ensure cached coordinates match @city, geocoding through the shared
 * pn-geocode lookup when needed.  Returns %TRUE when coordinates are
 * available, filling @lat/@lon; otherwise fills @out_error (caller
 * frees) with the most recent reason. */
static gboolean
astro_ensure_coords (PnAstronomical *self, const gchar *city,
                     gdouble *lat, gdouble *lon, gchar **out_error)
{
    gboolean need, ok;

    g_mutex_lock (&self->mutex);
    need = !self->have_coords || g_strcmp0 (self->geo_key, city) != 0;
    g_mutex_unlock (&self->mutex);

    if (need)
    {
        PnGeocodeResult res = { 0 };

        pn_geocode_resolve_sync (city, 10u, &res);

        g_mutex_lock (&self->mutex);
        g_free (self->resolved_name);
        g_free (self->country);
        g_free (self->timezone);
        g_free (self->geo_key);
        g_clear_pointer (&self->geo_error, g_free);
        if (res.resolved)
        {
            self->have_coords   = TRUE;
            self->latitude      = res.latitude;
            self->longitude     = res.longitude;
            self->resolved_name = res.name ? res.name : g_strdup (city);
            self->country       = res.country;    /* may be NULL */
            self->timezone      = res.timezone;   /* may be NULL */
            self->geo_key       = g_strdup (city);
            res.name     = NULL;                  /* ownership transferred */
            res.country  = NULL;
            res.timezone = NULL;
        }
        else
        {
            self->have_coords   = FALSE;
            self->resolved_name = NULL;
            self->country       = NULL;
            self->timezone      = NULL;
            self->geo_key       = NULL;           /* force a retry next tick */
            self->geo_error     = res.error;      /* may be NULL */
            res.error = NULL;
        }
        g_mutex_unlock (&self->mutex);

        pn_geocode_result_clear (&res);
    }

    g_mutex_lock (&self->mutex);
    ok = self->have_coords;
    if (ok)
    {
        *lat = self->latitude;
        *lon = self->longitude;
    }
    else if (out_error != NULL)
    {
        *out_error = g_strdup (self->geo_error);
    }
    g_mutex_unlock (&self->mutex);

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Trigger (worker thread)                                            */
/* ------------------------------------------------------------------ */

/* ISO 8601 / RFC 3339 timestamp for a Unix-seconds instant, expressed
 * in @tz (e.g. "2026-06-21T04:43:00+02:00"); caller frees.  The "%:z"
 * offset gives the canonical "+hh:mm" form rather than the bare "+hh"
 * g_date_time_format_iso8601() emits. */
static gchar *
format_iso8601 (gdouble unix_seconds, GTimeZone *tz)
{
    GDateTime *utc   = g_date_time_new_from_unix_utc ((gint64) unix_seconds);
    GDateTime *local = utc ? g_date_time_to_timezone (utc, tz) : NULL;
    gchar     *s     = local
            ? g_date_time_format (local, "%Y-%m-%dT%H:%M:%S%:z")
            : g_strdup ("");

    if (utc != NULL)
        g_date_time_unref (utc);
    if (local != NULL)
        g_date_time_unref (local);
    return s;
}

static void
pn_astronomical_trigger (PnAutoTrigger *trigger)
{
    PnAstronomical      *self  = PN_ASTRONOMICAL (trigger);
    PnNode              *node  = PN_NODE (self);
    PnAstronomicalResult r     = { 0 };
    PnAstronomicalDatum  datum;
    PnMessage           *msg;
    GTimeZone           *tz;
    gchar               *city, *name, *country, *tzname, *err = NULL;
    gdouble              lat = 0.0, lon = 0.0, value;
    GString             *summary;

    g_mutex_lock (&self->mutex);
    city  = g_strdup (self->city);
    datum = self->datum;
    g_mutex_unlock (&self->mutex);

    if (city == NULL || *city == '\0')
    {
        g_free (city);
        return;   /* nothing configured yet — stay quiet, like Weather */
    }

    if (!astro_ensure_coords (self, city, &lat, &lon, &err))
    {
        msg = pn_message_new (node, NULL);
        summary = g_string_new (NULL);
        if (err != NULL && *err != '\0')
            g_string_append (summary, err);
        else
            g_string_append_printf (summary,
                    "Could not look up the location \"%s\".", city);

        pn_message_set_string  (msg, "city",    city);
        pn_message_set_boolean (msg, "success", FALSE);
        pn_message_set_string  (msg, "output",  summary->str);
        pn_auto_trigger_emit_on_main (trigger, msg);

        g_string_free (summary, TRUE);
        g_free (err);
        g_free (city);
        return;
    }
    g_free (err);
    g_free (city);

    g_mutex_lock (&self->mutex);
    name    = g_strdup (self->resolved_name);
    country = g_strdup (self->country);
    tzname  = g_strdup (self->timezone);
    g_mutex_unlock (&self->mutex);

    /* Times are reported in the location's own time zone (Open-Meteo
     * supplies it with the coordinates); fall back to UTC if it didn't. */
    tz = (tzname != NULL && *tzname != '\0')
            ? g_time_zone_new_identifier (tzname) : NULL;
    if (tz == NULL)
        tz = g_time_zone_new_utc ();

    pn_astronomical_compute (lat, lon,
                             g_get_real_time () / G_USEC_PER_SEC, &r);

    switch (datum)
    {
    case PN_ASTRO_DATUM_SUN_ALTITUDE: value = r.altitude;            break;
    case PN_ASTRO_DATUM_SUN_AZIMUTH:  value = r.azimuth;             break;
    case PN_ASTRO_DATUM_DAY_LENGTH:   value = r.day_length;          break;
    case PN_ASTRO_DATUM_SUN_IS_DOWN:  value = r.sun_up ? 0.0 : 1.0;  break;
    case PN_ASTRO_DATUM_SUN_IS_UP:
    default:                          value = r.sun_up ? 1.0 : 0.0;  break;
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_double  (msg, "value",        value);
    pn_message_set_boolean (msg, "success",      TRUE);
    pn_message_set_boolean (msg, "sun_up",       r.sun_up);
    pn_message_set_double  (msg, "sun_altitude", r.altitude);
    pn_message_set_double  (msg, "sun_azimuth",  r.azimuth);
    pn_message_set_double  (msg, "day_length",   r.day_length);

    /* Date/time fields as timezone-aware ISO 8601 strings. */
    {
        gchar *noon = format_iso8601 (r.solar_noon, tz);
        pn_message_set_string (msg, "solar_noon", noon);
        g_free (noon);
    }
    if (r.has_sunrise)
    {
        gchar *s = format_iso8601 (r.sunrise, tz);
        pn_message_set_string (msg, "sunrise", s);
        g_free (s);
    }
    if (r.has_sunset)
    {
        gchar *s = format_iso8601 (r.sunset, tz);
        pn_message_set_string (msg, "sunset", s);
        g_free (s);
    }

    pn_message_set_double  (msg, "latitude",  lat);
    pn_message_set_double  (msg, "longitude", lon);
    if (name != NULL)
        pn_message_set_string (msg, "city", name);
    if (country != NULL)
        pn_message_set_string (msg, "country", country);
    if (tzname != NULL)
        pn_message_set_string (msg, "timezone", tzname);

    /* A short human-readable one-liner for a Debug / Text View; the
     * numeric detail is available in the data fields above. */
    {
        gchar *line = g_strdup_printf ("At %s the Sun is %s",
                                       name != NULL ? name : "the location",
                                       r.sun_up ? "up" : "down");
        pn_message_set_string (msg, "output", line);
        g_free (line);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_time_zone_unref (tz);
    g_free (name);
    g_free (country);
    g_free (tzname);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_astronomical_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnAstronomical *self = PN_ASTRONOMICAL (object);

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_CITY:
        g_value_set_string (value, self->city);
        break;
    case PROP_DATUM:
        g_value_set_enum (value, self->datum);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }

    g_mutex_unlock (&self->mutex);
}

static void
pn_astronomical_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnAstronomical *self    = PN_ASTRONOMICAL (object);
    gboolean        changed = FALSE;

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_CITY:
        {
            const gchar *new_city = g_value_get_string (value);
            if (g_strcmp0 (self->city, new_city) != 0)
            {
                g_free (self->city);
                self->city = g_strdup (new_city);
                changed = TRUE;
            }
        }
        break;
    case PROP_DATUM:
        {
            PnAstronomicalDatum d = g_value_get_enum (value);
            if (self->datum != d)
            {
                self->datum = d;
                changed = TRUE;
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }

    g_mutex_unlock (&self->mutex);

    if (changed)
    {
        g_object_notify_by_pspec (object, props[prop_id]);
        /* Re-emit promptly: a new city re-geocodes (geo_key mismatch),
         * a new datum just remaps data.value. */
        pn_auto_trigger_kick (PN_AUTO_TRIGGER (self));
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_astronomical_finalize (GObject *object)
{
    PnAstronomical *self = PN_ASTRONOMICAL (object);

    g_clear_pointer (&self->city,          g_free);
    g_clear_pointer (&self->resolved_name, g_free);
    g_clear_pointer (&self->country,       g_free);
    g_clear_pointer (&self->timezone,      g_free);
    g_clear_pointer (&self->geo_key,       g_free);
    g_clear_pointer (&self->geo_error,     g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_astronomical_parent_class)->finalize (object);
}

static void
pn_astronomical_class_init (PnAstronomicalClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_astronomical_get_property;
    object_class->set_property = pn_astronomical_set_property;
    object_class->finalize     = pn_astronomical_finalize;
    trigger_class->trigger     = pn_astronomical_trigger;

    node_class->palette_icon   = PN_ASTRONOMICAL_ICON;
    node_class->class_name     = "Astronomical";
    node_class->icon           = PN_ASTRONOMICAL_ICON;
    node_class->color          = (PnColor){ 0.42, 0.38, 0.72, 1.0 };
    node_class->category       = "Sources";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_CITY] = g_param_spec_string (
            "city", "City",
            "Place to compute astronomical data for.  Resolved to "
            "coordinates once through Open-Meteo's free geocoder; the "
            "resolved latitude/longitude are emitted in the message.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DATUM] = g_param_spec_enum (
            "datum", "Value source",
            "Which computed quantity drives data.value.  Every emission "
            "carries the full set of fields regardless; this only selects "
            "the canonical value.  The default is \"The Sun is up\" "
            "(1.0 when the Sun is above the horizon, 0.0 when below); pick "
            "\"The Sun is down\" for the inverse.",
            PN_TYPE_ASTRONOMICAL_DATUM, PN_ASTRO_DATUM_SUN_IS_UP,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_astronomical_init (PnAstronomical *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.38, 0.72, 1.0 };

    g_mutex_init (&self->mutex);
    self->city  = g_strdup ("");
    self->datum = PN_ASTRO_DATUM_SUN_IS_UP;

    pn_node_set_class_name (node, "Astronomical");
    pn_node_set_icon       (node, PN_ASTRONOMICAL_ICON);
    pn_node_set_color      (node, &indigo);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_ASTRONOMICAL_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnAstronomical *
pn_astronomical_new (void)
{
    return g_object_new (PN_TYPE_ASTRONOMICAL, NULL);
}
