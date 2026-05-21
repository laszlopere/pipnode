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

#include "pn-weather-report.h"
#include "pn-message.h"

#include <pango/pangocairo.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Deliberately identical to #PnGraph: a canonical 40 px header that  */
/*  the worksheet paints in the standard Node-RED style, a small gap,  */
/*  then a white card the same 280 x 173 size as the graph's plot      */
/*  rectangle.  Re-using the graph's dimensions keeps the two sink     */
/*  types visually interchangeable on the canvas.                      */
/* ------------------------------------------------------------------ */

#define PN_WR_WIDTH         280.0
#define PN_WR_HEADER_HEIGHT  40.0
#define PN_WR_GAP             4.0
#define PN_WR_CARD_HEIGHT   173.0
#define PN_WR_TOTAL_HEIGHT  (PN_WR_HEADER_HEIGHT + PN_WR_GAP + PN_WR_CARD_HEIGHT)

/* fa-cloud U+F0C2 — the stable palette / header glyph, shared with the
 * Weather source node so the pair reads as a family.  The header glyph
 * is swapped at run time to mirror the current conditions (sun, cloud,
 * rain, snow, …) once a reading arrives. */
#define PN_WR_ICON         "\xef\x83\x82"

/* FontAwesome 4.7 glyphs used inside the card, as UTF-8 byte strings. */
#define ICON_SUN           "\xef\x86\x85"  /* fa-sun-o        U+F185 */
#define ICON_MOON          "\xef\x86\x86"  /* fa-moon-o       U+F186 */
#define ICON_CLOUD         "\xef\x83\x82"  /* fa-cloud        U+F0C2 */
#define ICON_UMBRELLA      "\xef\x83\xa9"  /* fa-umbrella     U+F0E9 */
#define ICON_SNOW          "\xef\x8b\x9c"  /* fa-snowflake-o  U+F2DC */
#define ICON_BOLT          "\xef\x83\xa7"  /* fa-bolt         U+F0E7 */
#define ICON_MARKER        "\xef\x81\x81"  /* fa-map-marker   U+F041 */
#define ICON_TINT          "\xef\x81\x83"  /* fa-tint         U+F043 */
#define ICON_GAUGE         "\xef\x83\xa4"  /* fa-tachometer   U+F0E4 */
#define ICON_CLOCK         "\xef\x80\x97"  /* fa-clock-o      U+F017 */
#define ICON_COMPASS       "\xef\x85\x8e"  /* fa-compass      U+F14E */

/* Typography. */
#define FONT_SANS          "Sans"
#define FONT_BOLD          "Sans Bold"
#define FONT_FA            "FontAwesome"

/* Punctuation, as UTF-8. */
#define GLYPH_DEGREE       "\xc2\xb0"      /* °  */
#define GLYPH_MIDDOT       " \xc2\xb7 "    /*  ·  */
#define GLYPH_EMDASH       "\xe2\x80\x94"  /* —  */
#define GLYPH_ELLIPSIS     "\xe2\x80\xa6"  /* …  */

/* Palette — mostly black on white, with two greys for the secondary
 * text and the hairlines.  Kept monochrome on purpose so the card
 * reads as a clean print-like report rather than a dashboard. */
#define INK_R   0.13
#define INK_G   0.13
#define INK_B   0.13
#define MUT_R   0.46
#define MUT_G   0.46
#define MUT_B   0.46
#define LINE_R  0.86
#define LINE_G  0.86
#define LINE_B  0.86

struct _PnWeatherReport
{
    PnNode parent_instance;

    /* Latest reading, kept as a deep copy of the message data bag so it
     * survives past the (borrowed) message that delivered it.  %NULL
     * until the first message arrives. */
    JsonObject *data;

    /* When %FALSE the bottom row of detail tiles (humidity, wind,
     * pressure, cloud) is hidden, leaving just the place + headline
     * conditions. */
    gboolean    show_details;
};

G_DEFINE_TYPE (PnWeatherReport, pn_weather_report, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SHOW_DETAILS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  JSON readers                                                       */
/* ------------------------------------------------------------------ */

static JsonObject *
obj_obj (JsonObject *o, const gchar *key)
{
    JsonNode *n;

    if (o == NULL || !json_object_has_member (o, key))
        return NULL;
    n = json_object_get_member (o, key);
    return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static const gchar *
obj_str (JsonObject *o, const gchar *key)
{
    JsonNode *n;

    if (o == NULL || !json_object_has_member (o, key))
        return NULL;
    n = json_object_get_member (o, key);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

/* Read a JSON number that Open-Meteo may encode as either int or
 * double.  Returns %FALSE (leaving @out untouched) when the member is
 * absent, non-numeric, or non-finite. */
static gboolean
obj_num (JsonObject *o, const gchar *key, gdouble *out)
{
    JsonNode *n;
    GType     t;
    gdouble   v;

    if (o == NULL || !json_object_has_member (o, key))
        return FALSE;
    n = json_object_get_member (o, key);
    if (!JSON_NODE_HOLDS_VALUE (n))
        return FALSE;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        v = json_node_get_double (n);
    else if (t == G_TYPE_INT64)
        v = (gdouble) json_node_get_int (n);
    else
        return FALSE;

    if (!isfinite (v))
        return FALSE;
    *out = v;
    return TRUE;
}

static gboolean
obj_bool (JsonObject *o, const gchar *key, gboolean def)
{
    JsonNode *n;

    if (o == NULL || !json_object_has_member (o, key))
        return def;
    n = json_object_get_member (o, key);
    if (JSON_NODE_HOLDS_VALUE (n) &&
        json_node_get_value_type (n) == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n);
    return def;
}

/* Deep-copy a #JsonObject so the node owns a snapshot independent of
 * the message.  json_node_copy() does the recursive copy; we keep an
 * extra ref on the copied object and let the wrapper nodes go. */
static JsonObject *
dup_object (JsonObject *src)
{
    JsonNode   *wrap = json_node_init_object (json_node_alloc (), src);
    JsonNode   *copy = json_node_copy (wrap);
    JsonObject *out  = json_object_ref (json_node_get_object (copy));

    json_node_free (copy);
    json_node_free (wrap);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Weather helpers                                                    */
/* ------------------------------------------------------------------ */

/* Pick the headline glyph for a WMO weather code, honouring day/night
 * for the clear-sky case so a clear night shows a moon rather than a
 * sun. */
static const gchar *
condition_glyph (gint code, gboolean is_day)
{
    if (code <= 1)                  return is_day ? ICON_SUN : ICON_MOON;
    if (code == 2 || code == 3)     return ICON_CLOUD;
    if (code == 45 || code == 48)   return ICON_CLOUD;          /* fog   */
    if (code >= 51 && code <= 67)   return ICON_UMBRELLA;       /* rain  */
    if (code >= 71 && code <= 77)   return ICON_SNOW;           /* snow  */
    if (code >= 80 && code <= 82)   return ICON_UMBRELLA;       /* showers */
    if (code >= 85 && code <= 86)   return ICON_SNOW;
    if (code >= 95)                 return ICON_BOLT;           /* storm */
    return ICON_CLOUD;
}

/* Eight-point compass label for a bearing in degrees. */
static const gchar *
cardinal (gdouble deg)
{
    static const gchar *c[8] = { "N", "NE", "E", "SE",
                                 "S", "SW", "W", "NW" };
    gint i = (gint) floor (fmod (deg, 360.0) / 45.0 + 0.5);

    i %= 8;
    if (i < 0)
        i += 8;
    return c[i];
}

/* Pull "HH:MM" out of an ISO-8601-ish timestamp such as
 * "2026-05-21T14:00"; returns %NULL when the shape is unexpected. */
static gchar *
short_time (const gchar *iso)
{
    const gchar *t;

    if (iso == NULL)
        return NULL;
    t = strchr (iso, 'T');
    if (t == NULL || strlen (t + 1) < 5)
        return NULL;
    return g_strndup (t + 1, 5);
}

/* Format the date carried by an ISO-8601-ish timestamp
 * ("2026-05-21T14:00") as a long human label, e.g.
 * "Wednesday, 21 May 2026".  Falls back to the current local date when
 * the stamp is missing or unparseable.  Caller frees with g_free(). */
static gchar *
format_long_date (const gchar *iso)
{
    gint       y = 0, m = 0, d = 0;
    GDateTime *dt = NULL;
    gchar     *weekday, *month, *out;

    if (iso != NULL && sscanf (iso, "%d-%d-%d", &y, &m, &d) == 3)
        dt = g_date_time_new_utc (y, m, d, 12, 0, 0);
    if (dt == NULL)
        dt = g_date_time_new_now_local ();
    if (dt == NULL)
        return NULL;

    weekday = g_date_time_format (dt, "%A");
    month   = g_date_time_format (dt, "%B");
    out = g_strdup_printf ("%s, %d %s %d",
                           weekday, g_date_time_get_day_of_month (dt),
                           month, g_date_time_get_year (dt));

    g_free (weekday);
    g_free (month);
    g_date_time_unref (dt);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Cairo / Pango drawing primitives                                  */
/* ------------------------------------------------------------------ */

static PangoLayout *
build_layout (cairo_t *cr, const gchar *font, double size, const gchar *text)
{
    PangoLayout          *l = pango_cairo_create_layout (cr);
    PangoFontDescription *d = pango_font_description_from_string (font);

    pango_font_description_set_absolute_size (d, size * PANGO_SCALE);
    pango_layout_set_font_description (l, d);
    pango_font_description_free (d);
    pango_layout_set_text (l, text, -1);
    return l;
}

static void
measure (cairo_t *cr, const gchar *font, double size, const gchar *text,
         double *w, double *h)
{
    PangoLayout *l = build_layout (cr, font, size, text);
    int          pw, ph;

    pango_layout_get_pixel_size (l, &pw, &ph);
    if (w) *w = pw;
    if (h) *h = ph;
    g_object_unref (l);
}

/* Draw @text with its top-left at (@x, @y). */
static void
draw_text (cairo_t *cr, double x, double y, const gchar *font, double size,
           const gchar *text, double r, double g, double b)
{
    PangoLayout *l = build_layout (cr, font, size, text);

    cairo_set_source_rgb (cr, r, g, b);
    cairo_move_to (cr, x, y);
    pango_cairo_show_layout (cr, l);
    g_object_unref (l);
}

/* Draw @text horizontally centred on @cx, top at @y. */
static void
draw_text_centered (cairo_t *cr, double cx, double y, const gchar *font,
                    double size, const gchar *text,
                    double r, double g, double b)
{
    double w;

    measure (cr, font, size, text, &w, NULL);
    draw_text (cr, cx - w / 2.0, y, font, size, text, r, g, b);
}

/* A small compass arrow pointing where the wind is *going* (i.e. the
 * meteorological from-direction rotated 180°), centred in a box of
 * radius @radius around (@cx, @cy).  Drawn as a round-capped shaft and
 * a filled triangular head — a couple of strokes, no glyph needed. */
static void
draw_wind_arrow (cairo_t *cr, double cx, double cy, double radius,
                 double dir_from_deg, double r, double g, double b)
{
    double a   = (dir_from_deg + 180.0) * G_PI / 180.0;  /* 0° = up (N) */
    double dx  = sin (a);
    double dy  = -cos (a);
    double px  = -dy;                                     /* perpendicular */
    double py  = dx;
    double tipx = cx + dx * radius,        tipy = cy + dy * radius;
    double tailx = cx - dx * radius,       taily = cy - dy * radius;
    double hb  = radius * 0.85;                           /* head base back */
    double hw  = radius * 0.42;                           /* head half-width */
    double bx  = cx + dx * (radius - hb),  by = cy + dy * (radius - hb);

    cairo_save (cr);
    cairo_set_source_rgb (cr, r, g, b);
    cairo_set_line_width (cr, fmax (1.0, radius * 0.16));
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr, tailx, taily);
    cairo_line_to (cr, bx, by);
    cairo_stroke (cr);

    cairo_move_to (cr, tipx, tipy);
    cairo_line_to (cr, bx + px * hw, by + py * hw);
    cairo_line_to (cr, bx - px * hw, by - py * hw);
    cairo_close_path (cr);
    cairo_fill (cr);
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Card states                                                       */
/* ------------------------------------------------------------------ */

/* Centred icon + message, used for the "no data yet" and "lookup
 * failed" states. */
static void
paint_notice (cairo_t *cr, double w, double h,
              const gchar *glyph, const gchar *line1, const gchar *line2)
{
    double ih, l1h;
    double cy = h * 0.5;

    measure (cr, FONT_FA, h * 0.20, glyph, NULL, &ih);
    measure (cr, FONT_SANS, h * 0.065, line1, NULL, &l1h);

    {
        double block = ih + h * 0.04 + l1h + (line2 ? l1h + h * 0.015 : 0);
        double top   = cy - block / 2.0;

        draw_text_centered (cr, w / 2.0, top, FONT_FA, h * 0.20, glyph,
                            0.72, 0.72, 0.72);
        draw_text_centered (cr, w / 2.0, top + ih + h * 0.04,
                            FONT_SANS, h * 0.065, line1, MUT_R, MUT_G, MUT_B);
        if (line2 != NULL)
            draw_text_centered (cr, w / 2.0,
                                top + ih + h * 0.04 + l1h + h * 0.015,
                                FONT_SANS, h * 0.052, line2,
                                MUT_R, MUT_G, MUT_B);
    }
}

/* One detail tile: a small icon (or a custom painter when @glyph is
 * %NULL — used for the wind arrow), a value, and a tiny grey caption,
 * stacked and centred on @cx within the band [tile_top, tile_top+th]. */
typedef void (*TileIconFn) (cairo_t *cr, double cx, double iy, double size,
                            gpointer user);

static void
paint_tile (cairo_t *cr, double cx, double tile_top, double h,
            const gchar *glyph, TileIconFn icon_fn, gpointer icon_user,
            const gchar *value, const gchar *caption)
{
    double icon_sz = h * 0.072;
    double val_sz  = h * 0.066;
    double cap_sz  = h * 0.044;
    double y       = tile_top;
    double ih = icon_sz, vh, ch;

    measure (cr, FONT_SANS, val_sz, value,  NULL, &vh);
    measure (cr, FONT_SANS, cap_sz, caption, NULL, &ch);

    if (icon_fn != NULL)
    {
        icon_fn (cr, cx, y + icon_sz * 0.5, icon_sz, icon_user);
        ih = icon_sz;
    }
    else
    {
        double iw;
        measure (cr, FONT_FA, icon_sz, glyph, &iw, &ih);
        draw_text (cr, cx - iw / 2.0, y, FONT_FA, icon_sz, glyph,
                   INK_R, INK_G, INK_B);
    }

    y += ih + h * 0.022;
    draw_text_centered (cr, cx, y, FONT_BOLD, val_sz, value,
                        INK_R, INK_G, INK_B);
    y += vh + h * 0.012;
    draw_text_centered (cr, cx, y, FONT_SANS, cap_sz, caption,
                        MUT_R, MUT_G, MUT_B);
}

typedef struct { double dir; } WindArrowCtx;

static void
wind_arrow_tile_icon (cairo_t *cr, double cx, double iy, double size,
                      gpointer user)
{
    WindArrowCtx *ctx = user;
    draw_wind_arrow (cr, cx, iy, size * 0.5, ctx->dir, INK_R, INK_G, INK_B);
}

/* ------------------------------------------------------------------ */
/*  The report                                                        */
/* ------------------------------------------------------------------ */

static void
paint_report (PnWeatherReport *self, cairo_t *cr, double w, double h)
{
    JsonObject  *data = self->data;
    JsonObject  *raw  = obj_obj (data, "raw");
    JsonObject  *cur  = obj_obj (raw, "current");

    const gchar *city    = obj_str (data, "city");
    const gchar *country = obj_str (data, "country");
    const gchar *desc    = obj_str (data, "description");

    gdouble temp, app, hum, wind, wdir, cloud, press, precip, code_d;
    gboolean have_temp = obj_num (data, "temperature", &temp) ||
                         obj_num (data, "value", &temp);
    gboolean have_app    = obj_num (cur, "apparent_temperature", &app);
    gboolean have_hum    = obj_num (data, "humidity", &hum);
    gboolean have_wind   = obj_num (data, "wind_speed", &wind);
    gboolean have_wdir   = obj_num (cur, "wind_direction_10m", &wdir);
    gboolean have_cloud  = obj_num (cur, "cloud_cover", &cloud);
    gboolean have_press  = obj_num (cur, "pressure_msl", &press) ||
                           obj_num (cur, "surface_pressure", &press);
    gboolean have_precip = obj_num (cur, "precipitation", &precip);
    gint     code        = obj_num (data, "weather_code", &code_d)
                           ? (gint) code_d : -1;
    gboolean is_day      = obj_bool (cur, "is_day", TRUE);
    const gchar *iso_time = obj_str (cur, "time");
    gchar   *obstime     = short_time (iso_time);

    const double padx = w * 0.05;
    const double ml   = padx;
    const double mr   = w - padx;

    double loc_bottom;
    double tiles_top = 0.0, tile_h = 0.0;

    /* --- 1. Location row: marker + place name spanning the full width.
     *        The obs time has moved down into the left-hand date/time
     *        panel, so a long "Reykjavík, Iceland" now has the whole row
     *        to ellipsize into. ------------------------------------- */
    {
        const double y       = h * 0.06;
        const double mk_sz   = h * 0.066;
        const double city_sz = h * 0.08;
        double       mk_w, mk_h, city_h;
        gchar       *place;

        if (city != NULL && country != NULL && *country != '\0')
            place = g_strdup_printf ("%s, %s", city, country);
        else if (city != NULL)
            place = g_strdup (city);
        else
            place = g_strdup ("Unknown location");

        measure (cr, FONT_FA, mk_sz, ICON_MARKER, &mk_w, &mk_h);
        measure (cr, FONT_BOLD, city_sz, place, NULL, &city_h);

        loc_bottom = y + MAX (mk_h, city_h);

        draw_text (cr, ml, y + (MAX (mk_h, city_h) - mk_h) / 2.0,
                   FONT_FA, mk_sz, ICON_MARKER, INK_R, INK_G, INK_B);

        /* Ellipsize the place name into the rest of the row. */
        {
            PangoLayout          *l = pango_cairo_create_layout (cr);
            PangoFontDescription *d = pango_font_description_from_string (FONT_BOLD);
            double                avail = mr - (ml + mk_w + w * 0.03);

            pango_font_description_set_absolute_size (d, city_sz * PANGO_SCALE);
            pango_layout_set_font_description (l, d);
            pango_font_description_free (d);
            pango_layout_set_text (l, place, -1);
            pango_layout_set_width (l, (int) (MAX (avail, w * 0.2) * PANGO_SCALE));
            pango_layout_set_ellipsize (l, PANGO_ELLIPSIZE_END);

            cairo_set_source_rgb (cr, INK_R, INK_G, INK_B);
            cairo_move_to (cr, ml + mk_w + w * 0.03,
                           y + (MAX (mk_h, city_h) - city_h) / 2.0);
            pango_cairo_show_layout (cr, l);
            g_object_unref (l);
        }

        g_free (place);
    }

    /* --- 3. Detail tiles, anchored to the bottom so the hero band is
     *        whatever vertical space is left in the middle.  The grey
     *        divider above them is drawn later, once the hero knows
     *        where its date line ends. ----------------------------- */
    if (self->show_details)
    {
        tile_h    = h * 0.235;
        tiles_top = h - h * 0.06 - tile_h;

        {
            const double span  = mr - ml;
            const double tw    = span / 4.0;
            gchar       *s;

            /* Humidity. */
            s = have_hum ? g_strdup_printf ("%.0f%%", hum)
                         : g_strdup (GLYPH_EMDASH);
            paint_tile (cr, ml + tw * 0.5, tiles_top, h,
                        ICON_TINT, NULL, NULL, s, "HUMIDITY");
            g_free (s);

            /* Wind — a compass arrow drawn to the bearing when we have
             * a direction, otherwise a static compass glyph. */
            {
                WindArrowCtx ctx = { have_wdir ? wdir : 0.0 };
                gchar       *cap;

                s   = have_wind ? g_strdup_printf ("%.0f km/h", wind)
                                : g_strdup (GLYPH_EMDASH);
                cap = have_wdir ? g_strdup_printf ("WIND %s", cardinal (wdir))
                                : g_strdup ("WIND");
                if (have_wdir)
                    paint_tile (cr, ml + tw * 1.5, tiles_top, h,
                                NULL, wind_arrow_tile_icon, &ctx, s, cap);
                else
                    paint_tile (cr, ml + tw * 1.5, tiles_top, h,
                                ICON_COMPASS, NULL, NULL, s, cap);
                g_free (s);
                g_free (cap);
            }

            /* Pressure. */
            s = have_press ? g_strdup_printf ("%.0f", press)
                           : g_strdup (GLYPH_EMDASH);
            paint_tile (cr, ml + tw * 2.5, tiles_top, h,
                        ICON_GAUGE, NULL, NULL, s, "hPa");
            g_free (s);

            /* Cloud cover. */
            s = have_cloud ? g_strdup_printf ("%.0f%%", cloud)
                           : g_strdup (GLYPH_EMDASH);
            paint_tile (cr, ml + tw * 3.5, tiles_top, h,
                        ICON_CLOUD, NULL, NULL, s, "CLOUD");
            g_free (s);
        }
    }

    /* --- 2. Hero: the conditions fill the left — the weather glyph next
     *        to the temperature, with the description and feels-like
     *        beneath it — and a date/time panel sits on the right: the
     *        big black time over the long date, both flush to the right
     *        edge.  The grey divider then drops into the gap above the
     *        tiles. ---------------------------------------------------- */
    {
        const double right_w  = (mr - ml) * 0.36;

        const double time_sz  = h * 0.135;
        const double date_sz  = h * 0.052;
        const double icon_sz  = h * 0.20;
        const double num_sz   = h * 0.19;
        const double unit_sz  = h * 0.082;
        const double desc_sz  = h * 0.066;
        const double sub_sz   = h * 0.052;

        const gchar *glyph = condition_glyph (code, is_day);
        gchar       *numbuf;
        gchar       *subbuf;
        gchar       *datebuf = format_long_date (iso_time);
        gchar       *timebuf;

        double icon_w, icon_h, num_w, num_h, unit_h, time_w, time_h, desc_h, sub_h;
        double hero_top, numx, ly, ry, left_bottom, right_bottom, content_bottom;

        /* Observation time, falling back to the local wall clock. */
        if (obstime != NULL)
            timebuf = g_strdup (obstime);
        else
        {
            GDateTime *now = g_date_time_new_now_local ();
            timebuf = now != NULL ? g_date_time_format (now, "%H:%M")
                                  : g_strdup ("--:--");
            if (now != NULL)
                g_date_time_unref (now);
        }

        numbuf = have_temp ? g_strdup_printf ("%.0f" GLYPH_DEGREE, temp)
                           : g_strdup (GLYPH_EMDASH);

        /* Sub-line under the description: feels-like, plus precipitation
         * only when there is some — a dry day shouldn't carry a "0 mm". */
        if (have_app && have_precip && precip > 0.05)
            subbuf = g_strdup_printf ("Feels like %.0f" GLYPH_DEGREE
                                      GLYPH_MIDDOT "%.1f mm", app, precip);
        else if (have_app)
            subbuf = g_strdup_printf ("Feels like %.0f" GLYPH_DEGREE, app);
        else if (have_precip && precip > 0.05)
            subbuf = g_strdup_printf ("%.1f mm precipitation", precip);
        else
            subbuf = NULL;

        measure (cr, FONT_FA,   icon_sz, glyph,   &icon_w, &icon_h);
        measure (cr, FONT_SANS, num_sz,  numbuf,  &num_w,  &num_h);
        measure (cr, FONT_SANS, unit_sz, "C", NULL, &unit_h);
        measure (cr, FONT_BOLD, time_sz, timebuf, &time_w, &time_h);
        measure (cr, FONT_BOLD, desc_sz, desc ? desc : "", NULL, &desc_h);
        if (subbuf != NULL)
            measure (cr, FONT_SANS, sub_sz, subbuf, NULL, &sub_h);
        else
            sub_h = 0.0;

        hero_top = loc_bottom + h * 0.03;

        /* LEFT column: glyph + temperature on one row, then the
         * description and feels-like beneath them. */
        ly   = hero_top;
        numx = ml + icon_w + w * 0.02;

        draw_text (cr, ml, ly + MAX (0.0, (num_h - icon_h) / 2.0),
                   FONT_FA, icon_sz, glyph, INK_R, INK_G, INK_B);
        draw_text (cr, numx, ly, FONT_SANS, num_sz, numbuf,
                   INK_R, INK_G, INK_B);
        if (have_temp)
            draw_text (cr, numx + num_w + w * 0.008,
                       ly + (num_h - unit_h) * 0.42,
                       FONT_SANS, unit_sz, "C", INK_R, INK_G, INK_B);

        ly += num_h;
        if (desc != NULL && *desc != '\0')
        {
            ly += h * 0.004;
            draw_text (cr, ml, ly, FONT_BOLD, desc_sz, desc,
                       INK_R, INK_G, INK_B);
            ly += desc_h;
        }
        if (subbuf != NULL)
        {
            ly += h * 0.006;
            draw_text (cr, ml, ly, FONT_SANS, sub_sz, subbuf,
                       MUT_R, MUT_G, MUT_B);
            ly += sub_h;
        }
        left_bottom = ly;

        /* RIGHT column: big black time, then the long date wrapped and
         * right-aligned beneath it, both flush to the card's right edge. */
        ry = hero_top;
        draw_text (cr, mr - time_w, ry, FONT_BOLD, time_sz, timebuf,
                   INK_R, INK_G, INK_B);
        ry += time_h + h * 0.012;
        if (datebuf != NULL)
        {
            PangoLayout *l = build_layout (cr, FONT_SANS, date_sz, datebuf);
            int          pw, ph;

            pango_layout_set_width (l, (int) (right_w * PANGO_SCALE));
            pango_layout_set_wrap (l, PANGO_WRAP_WORD);
            pango_layout_set_alignment (l, PANGO_ALIGN_RIGHT);
            pango_layout_get_pixel_size (l, &pw, &ph);

            cairo_set_source_rgb (cr, MUT_R, MUT_G, MUT_B);
            cairo_move_to (cr, mr - right_w, ry);
            pango_cairo_show_layout (cr, l);
            g_object_unref (l);
            ry += ph;
        }
        right_bottom = ry;

        content_bottom = MAX (left_bottom, right_bottom);

        /* Grey divider — kept clear of the tile icons by a fixed gap
         * above the tiles, and a touch thicker than a hairline. */
        if (self->show_details && tiles_top > content_bottom)
        {
            double dy = tiles_top - h * 0.06;
            double lw = CLAMP (h * 0.011, 1.5, 4.0);

            if (dy < content_bottom + h * 0.02)
                dy = content_bottom + h * 0.02;

            cairo_save (cr);
            cairo_set_source_rgb (cr, LINE_R, LINE_G, LINE_B);
            cairo_set_line_width (cr, lw);
            cairo_move_to (cr, ml, dy);
            cairo_line_to (cr, mr, dy);
            cairo_stroke (cr);
            cairo_restore (cr);
        }

        g_free (numbuf);
        g_free (subbuf);
        g_free (datebuf);
        g_free (timebuf);
    }

    g_free (obstime);
}

/* ------------------------------------------------------------------ */
/*  paint_plot vfunc                                                   */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_paint_plot (PnNode  *node,
                              cairo_t *cr,
                              double   x,
                              double   y,
                              double   w,
                              double   h)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (node);

    /* White card + hairline frame, always drawn so an empty card reads
     * as a deliberate surface rather than a hole in the canvas. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, LINE_R, LINE_G, LINE_B);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    if (self->data == NULL)
    {
        paint_notice (cr, w, h, ICON_CLOUD,
                      "Waiting for weather" GLYPH_ELLIPSIS, NULL);
    }
    else if (!obj_bool (self->data, "success", TRUE))
    {
        const gchar *out = obj_str (self->data, "output");
        paint_notice (cr, w, h, ICON_CLOUD,
                      "No reading", out);
    }
    else
    {
        paint_report (self, cr, w, h);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_receive (PnNode    *node,
                           PnMessage *message)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (node);
    JsonObject      *data = pn_message_get_data (message);

    if (data == NULL)
        return;

    g_clear_pointer (&self->data, json_object_unref);
    self->data = dup_object (data);

    /* Mirror the current conditions onto the header glyph so the small
     * at-rest node reflects the weather at a glance.  Falls back to the
     * stable cloud when the reading carries no usable code. */
    {
        gdouble code_d;
        if (obj_bool (self->data, "success", TRUE) &&
            obj_num (self->data, "weather_code", &code_d))
        {
            JsonObject  *cur = obj_obj (obj_obj (self->data, "raw"), "current");
            gboolean     day = obj_bool (cur, "is_day", TRUE);
            pn_node_set_icon (node, condition_glyph ((gint) code_d, day));
        }
        else
        {
            pn_node_set_icon (node, PN_WR_ICON);
        }
    }

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_get_size (PnNode *node,
                            double *out_width,
                            double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_WR_WIDTH;
    if (out_height != NULL) *out_height = PN_WR_TOTAL_HEIGHT;
}

static double
pn_weather_report_get_header_height (PnNode *node)
{
    (void) node;
    return PN_WR_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (object);

    switch (prop_id)
    {
    case PROP_SHOW_DETAILS:
        g_value_set_boolean (value, self->show_details);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_weather_report_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (object);

    switch (prop_id)
    {
    case PROP_SHOW_DETAILS:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->show_details != v)
            {
                self->show_details = v;
                g_object_notify_by_pspec (object, props[PROP_SHOW_DETAILS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_finalize (GObject *object)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (object);

    g_clear_pointer (&self->data, json_object_unref);

    G_OBJECT_CLASS (pn_weather_report_parent_class)->finalize (object);
}

static void
pn_weather_report_class_init (PnWeatherReportClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_weather_report_get_property;
    object_class->set_property = pn_weather_report_set_property;
    object_class->finalize     = pn_weather_report_finalize;

    node_class->receive           = pn_weather_report_receive;
    node_class->get_size          = pn_weather_report_get_size;
    node_class->get_header_height  = pn_weather_report_get_header_height;
    node_class->paint_plot        = pn_weather_report_paint_plot;
    /* The card is a fixed-layout reading surface, so the centred zoom
     * overlay should fit it while keeping the 280:173 aspect ratio
     * rather than stretching the type out of shape. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    node_class->palette_icon = PN_WR_ICON;
    node_class->class_name   = "Weather Report";
    node_class->icon         = PN_WR_ICON;
    node_class->color        = (GdkRGBA){ 0.30, 0.60, 0.85, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    props[PROP_SHOW_DETAILS] = g_param_spec_boolean (
            "show-details", "Show details",
            "Draw the bottom row of detail tiles (humidity, wind, "
            "pressure, cloud cover) under the headline conditions",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_weather_report_init (PnWeatherReport *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  sky  = { 0.30, 0.60, 0.85, 1.0 };

    self->data         = NULL;
    self->show_details = TRUE;

    pn_node_set_class_name (node, "Weather Report");
    pn_node_set_icon       (node, PN_WR_ICON);
    pn_node_set_color      (node, &sky);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWeatherReport *
pn_weather_report_new (void)
{
    return g_object_new (PN_TYPE_WEATHER_REPORT, NULL);
}
