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
/*  PnWeatherReport — gui tier.                                         */
/*                                                                     */
/*  The cairo/Pango weather-card painter (background fill or gradient,   */
/*  location row, the hero conditions block, the date/time panel and     */
/*  the row of detail tiles with their FontAwesome icons + wind arrow,   */
/*  plus the waiting / no-reading notices).  The node's GType,           */
/*  properties, receive() with its data snapshot + header-glyph          */
/*  mirroring, the on-card clock tick and the unit enums' GType          */
/*  registrations live in the GTK-free core file pn-weather-report.c;    */
/*  this file installs the paint_plot vfunc onto that class at editor     */
/*  startup (pn_weather_report_gui_install), reading the reading +        */
/*  display config through the core's GTK-free accessors.  The small      */
/*  JSON readers and the unit converters are duplicated here as           */
/*  painter-local statics so the core surface stays minimal.  The         */
/*  headless runtime never loads this file's half, so the Weather Report  */
/*  logic runs without GTK.                                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-weather-report-gui.h"
#include "pn-weather-report.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <json-glib/json-glib.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Glyphs / fonts / palette (painter-side copy)                       */
/* ------------------------------------------------------------------ */

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

#define FONT_SANS          "Sans"
#define FONT_BOLD          "Sans Bold"
#define FONT_FA            "FontAwesome"

#define GLYPH_DEGREE       "\xc2\xb0"      /* °  */
#define GLYPH_MIDDOT       " \xc2\xb7 "    /*  ·  */
#define GLYPH_EMDASH       "\xe2\x80\x94"  /* —  */
#define GLYPH_ELLIPSIS     "\xe2\x80\xa6"  /* …  */

#define LINE_R  0.86
#define LINE_G  0.86
#define LINE_B  0.86

/* ------------------------------------------------------------------ */
/*  JSON readers (painter-local copies of the core's static helpers)   */
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

/* ------------------------------------------------------------------ */
/*  Unit conversions / labels                                          */
/* ------------------------------------------------------------------ */

static gdouble
convert_temp (gdouble celsius, PnWrTempUnit u)
{
    switch (u)
    {
    case PN_WR_TEMP_FAHRENHEIT: return celsius * 9.0 / 5.0 + 32.0;
    case PN_WR_TEMP_KELVIN:     return celsius + 273.15;
    default:                    return celsius;
    }
}

static const gchar *
temp_unit_label (PnWrTempUnit u)
{
    switch (u)
    {
    case PN_WR_TEMP_FAHRENHEIT: return GLYPH_DEGREE "F";
    case PN_WR_TEMP_KELVIN:     return "K";
    default:                    return GLYPH_DEGREE "C";
    }
}

static gdouble
convert_wind (gdouble kmh, PnWrWindUnit u)
{
    switch (u)
    {
    case PN_WR_WIND_MS:    return kmh / 3.6;
    case PN_WR_WIND_MPH:   return kmh / 1.609344;
    case PN_WR_WIND_KNOTS: return kmh / 1.852;
    default:               return kmh;
    }
}

static const gchar *
wind_unit_label (PnWrWindUnit u)
{
    switch (u)
    {
    case PN_WR_WIND_MS:    return "m/s";
    case PN_WR_WIND_MPH:   return "mph";
    case PN_WR_WIND_KNOTS: return "kn";
    default:               return "km/h";
    }
}

static gdouble
convert_press (gdouble hpa, PnWrPressUnit u)
{
    switch (u)
    {
    case PN_WR_PRESS_KPA:  return hpa / 10.0;
    case PN_WR_PRESS_INHG: return hpa * 0.029529983071445;
    case PN_WR_PRESS_MMHG: return hpa * 0.750061682704;
    default:               return hpa;
    }
}

static const gchar *
press_unit_label (PnWrPressUnit u)
{
    switch (u)
    {
    case PN_WR_PRESS_KPA:  return "kPa";
    case PN_WR_PRESS_INHG: return "inHg";
    case PN_WR_PRESS_MMHG: return "mmHg";
    default:               return "hPa";
    }
}

static gchar *
format_press (gdouble v, PnWrPressUnit u)
{
    switch (u)
    {
    case PN_WR_PRESS_KPA:  return g_strdup_printf ("%.1f", v);
    case PN_WR_PRESS_INHG: return g_strdup_printf ("%.2f", v);
    default:               return g_strdup_printf ("%.0f", v);  /* hPa, mmHg */
    }
}

/* ------------------------------------------------------------------ */
/*  Weather helpers                                                    */
/* ------------------------------------------------------------------ */

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

static gchar *
format_long_date (GDateTime *dt)
{
    gchar *weekday, *month, *out;

    if (dt == NULL)
        return NULL;

    weekday = g_date_time_format (dt, "%A");
    month   = g_date_time_format (dt, "%B");
    out = g_strdup_printf ("%s, %d %s %d",
                           weekday, g_date_time_get_day_of_month (dt),
                           month, g_date_time_get_year (dt));

    g_free (weekday);
    g_free (month);
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

/* A small compass arrow pointing where the wind is *going* (the
 * meteorological from-direction rotated 180°), centred in a box of
 * radius @radius around (@cx, @cy). */
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
              const gchar *glyph, const gchar *line1, const gchar *line2,
              const PnColor *muted)
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
                            FONT_SANS, h * 0.065, line1,
                            muted->red, muted->green, muted->blue);
        if (line2 != NULL)
            draw_text_centered (cr, w / 2.0,
                                top + ih + h * 0.04 + l1h + h * 0.015,
                                FONT_SANS, h * 0.052, line2,
                                muted->red, muted->green, muted->blue);
    }
}

/* One detail tile: a small icon (or a custom painter when @glyph is
 * %NULL — used for the wind arrow), a value, and a tiny grey caption. */
typedef void (*TileIconFn) (cairo_t *cr, double cx, double iy, double size,
                            gpointer user);

static void
paint_tile (cairo_t *cr, double cx, double tile_top, double h,
            const gchar *glyph, TileIconFn icon_fn, gpointer icon_user,
            const gchar *value, const gchar *caption,
            const PnColor *fg, const PnColor *muted)
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
                   fg->red, fg->green, fg->blue);
    }

    y += ih + h * 0.022;
    draw_text_centered (cr, cx, y, FONT_BOLD, val_sz, value,
                        fg->red, fg->green, fg->blue);
    y += vh + h * 0.012;
    draw_text_centered (cr, cx, y, FONT_SANS, cap_sz, caption,
                        muted->red, muted->green, muted->blue);
}

typedef struct { double dir; PnColor col; } WindArrowCtx;

static void
wind_arrow_tile_icon (cairo_t *cr, double cx, double iy, double size,
                      gpointer user)
{
    WindArrowCtx *ctx = user;
    draw_wind_arrow (cr, cx, iy, size * 0.5, ctx->dir,
                     ctx->col.red, ctx->col.green, ctx->col.blue);
}

/* ------------------------------------------------------------------ */
/*  The report                                                        */
/* ------------------------------------------------------------------ */

static void
paint_report (const PnWeatherReportPaintState *ps,
              JsonObject *data, cairo_t *cr, double w, double h)
{
    JsonObject  *raw  = obj_obj (data, "raw");
    JsonObject  *cur  = obj_obj (raw, "current");

    const PnColor *fg  = &ps->font_color;
    const PnColor *mut = &ps->secondary_color;

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

    const double padx = w * 0.05;
    const double ml   = padx;
    const double mr   = w - padx;

    double loc_bottom;
    double tiles_top = 0.0, tile_h = 0.0;

    /* --- 1. Location row: marker + place name spanning the full width. */
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
                   FONT_FA, mk_sz, ICON_MARKER, fg->red, fg->green, fg->blue);

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

            cairo_set_source_rgb (cr, fg->red, fg->green, fg->blue);
            cairo_move_to (cr, ml + mk_w + w * 0.03,
                           y + (MAX (mk_h, city_h) - city_h) / 2.0);
            pango_cairo_show_layout (cr, l);
            g_object_unref (l);
        }

        g_free (place);
    }

    /* --- 3. Detail tiles, anchored to the bottom. ------------------ */
    if (ps->show_details)
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
                        ICON_TINT, NULL, NULL, s, "HUMIDITY", fg, mut);
            g_free (s);

            /* Wind. */
            {
                WindArrowCtx ctx = { have_wdir ? wdir : 0.0, *fg };
                const gchar *wu  = wind_unit_label (ps->wind_unit);
                gchar       *cap;

                if (!have_wind)
                    s = g_strdup (GLYPH_EMDASH);
                else
                {
                    gdouble v = convert_wind (wind, ps->wind_unit);
                    s = ps->wind_unit == PN_WR_WIND_MS
                            ? g_strdup_printf ("%.1f %s", v, wu)
                            : g_strdup_printf ("%.0f %s", v, wu);
                }
                cap = have_wdir ? g_strdup_printf ("WIND %s", cardinal (wdir))
                                : g_strdup ("WIND");
                if (have_wdir)
                    paint_tile (cr, ml + tw * 1.5, tiles_top, h,
                                NULL, wind_arrow_tile_icon, &ctx, s, cap,
                                fg, mut);
                else
                    paint_tile (cr, ml + tw * 1.5, tiles_top, h,
                                ICON_COMPASS, NULL, NULL, s, cap, fg, mut);
                g_free (s);
                g_free (cap);
            }

            /* Pressure. */
            s = have_press
                    ? format_press (convert_press (press, ps->press_unit),
                                    ps->press_unit)
                    : g_strdup (GLYPH_EMDASH);
            paint_tile (cr, ml + tw * 2.5, tiles_top, h,
                        ICON_GAUGE, NULL, NULL, s,
                        press_unit_label (ps->press_unit), fg, mut);
            g_free (s);

            /* Cloud cover. */
            s = have_cloud ? g_strdup_printf ("%.0f%%", cloud)
                           : g_strdup (GLYPH_EMDASH);
            paint_tile (cr, ml + tw * 3.5, tiles_top, h,
                        ICON_CLOUD, NULL, NULL, s, "CLOUD", fg, mut);
            g_free (s);
        }
    }

    /* --- 2. Hero conditions + date/time panel. --------------------- */
    {
        const double right_w  = (mr - ml) * 0.36;

        const double time_sz  = h * 0.135;
        const double date_sz  = h * 0.052;
        const double icon_sz  = h * 0.20;
        const double num_sz   = h * 0.19;
        const double desc_sz  = h * 0.066;
        const double sub_sz   = h * 0.052;

        const gchar *glyph = condition_glyph (code, is_day);
        gchar       *numbuf;
        gchar       *subbuf;
        gchar       *datebuf;
        gchar       *timebuf;

        double icon_w, icon_h, num_h, time_w, time_h, desc_h, sub_h;
        double hero_top, numx, ly, ry, left_bottom, right_bottom, content_bottom;

        {
            GDateTime *now = g_date_time_new_now_local ();
            datebuf = format_long_date (now);
            timebuf = now != NULL ? g_date_time_format (now, "%H:%M")
                                  : g_strdup ("--:--");
            if (now != NULL)
                g_date_time_unref (now);
        }

        if (!have_temp)
            numbuf = g_strdup (GLYPH_EMDASH);
        else
        {
            gdouble t = convert_temp (temp, ps->temp_unit);
            numbuf = ps->temp_unit == PN_WR_TEMP_KELVIN
                    ? g_strdup_printf ("%.0f %s", t, temp_unit_label (ps->temp_unit))
                    : g_strdup_printf ("%.0f%s",  t, temp_unit_label (ps->temp_unit));
        }

        {
            gchar *afmt = NULL;

            if (have_app)
            {
                gdouble a = convert_temp (app, ps->temp_unit);
                afmt = ps->temp_unit == PN_WR_TEMP_KELVIN
                        ? g_strdup_printf ("%.0f K", a)
                        : g_strdup_printf ("%.0f" GLYPH_DEGREE, a);
            }

            if (afmt != NULL && have_precip && precip > 0.05)
                subbuf = g_strdup_printf ("Feels like %s" GLYPH_MIDDOT
                                          "%.1f mm", afmt, precip);
            else if (afmt != NULL)
                subbuf = g_strdup_printf ("Feels like %s", afmt);
            else if (have_precip && precip > 0.05)
                subbuf = g_strdup_printf ("%.1f mm precipitation", precip);
            else
                subbuf = NULL;

            g_free (afmt);
        }

        measure (cr, FONT_FA,   icon_sz, glyph,   &icon_w, &icon_h);
        measure (cr, FONT_SANS, num_sz,  numbuf,  NULL,    &num_h);
        measure (cr, FONT_BOLD, time_sz, timebuf, &time_w, &time_h);
        measure (cr, FONT_BOLD, desc_sz, desc ? desc : "", NULL, &desc_h);
        if (subbuf != NULL)
            measure (cr, FONT_SANS, sub_sz, subbuf, NULL, &sub_h);
        else
            sub_h = 0.0;

        hero_top = loc_bottom + h * 0.03;

        ly   = hero_top;
        numx = ml + icon_w + w * 0.02;

        draw_text (cr, ml, ly + MAX (0.0, (num_h - icon_h) / 2.0),
                   FONT_FA, icon_sz, glyph, fg->red, fg->green, fg->blue);
        draw_text (cr, numx, ly, FONT_SANS, num_sz, numbuf,
                   fg->red, fg->green, fg->blue);

        ly += num_h;
        if (desc != NULL && *desc != '\0')
        {
            ly += h * 0.004;
            draw_text (cr, ml, ly, FONT_BOLD, desc_sz, desc,
                       fg->red, fg->green, fg->blue);
            ly += desc_h;
        }
        if (subbuf != NULL)
        {
            ly += h * 0.006;
            draw_text (cr, ml, ly, FONT_SANS, sub_sz, subbuf,
                       mut->red, mut->green, mut->blue);
            ly += sub_h;
        }
        left_bottom = ly;

        ry = hero_top;
        draw_text (cr, mr - time_w, ry, FONT_BOLD, time_sz, timebuf,
                   fg->red, fg->green, fg->blue);
        ry += time_h + h * 0.012;
        if (datebuf != NULL)
        {
            PangoLayout *l = build_layout (cr, FONT_SANS, date_sz, datebuf);
            int          pw, ph;

            pango_layout_set_width (l, (int) (right_w * PANGO_SCALE));
            pango_layout_set_wrap (l, PANGO_WRAP_WORD);
            pango_layout_set_alignment (l, PANGO_ALIGN_RIGHT);
            pango_layout_get_pixel_size (l, &pw, &ph);

            cairo_set_source_rgb (cr, mut->red, mut->green, mut->blue);
            cairo_move_to (cr, mr - right_w, ry);
            pango_cairo_show_layout (cr, l);
            g_object_unref (l);
            ry += ph;
        }
        right_bottom = ry;

        content_bottom = MAX (left_bottom, right_bottom);

        /* Grey divider above the tiles. */
        if (ps->show_details && tiles_top > content_bottom)
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
}

/* ------------------------------------------------------------------ */
/*  paint_plot vfunc                                                   */
/* ------------------------------------------------------------------ */

/* Set @cr's source to the card background: a flat fill of @bg_color, or
 * a two-stop linear gradient from @bg_color to @bg_color2 when a
 * gradient direction is selected. */
static void
set_background_source (const PnWeatherReportPaintState *ps, cairo_t *cr,
                       double x, double y, double w, double h)
{
    const PnColor *a = &ps->bg_color;
    const PnColor *b = &ps->bg_color2;
    cairo_pattern_t *grad;
    double x2 = x, y2 = y;

    if (ps->bg_gradient == PN_WR_GRADIENT_NONE)
    {
        cairo_set_source_rgba (cr, a->red, a->green, a->blue, a->alpha);
        return;
    }

    switch (ps->bg_gradient)
    {
    case PN_WR_GRADIENT_HORIZONTAL: x2 = x + w;             break;
    case PN_WR_GRADIENT_DIAGONAL:   x2 = x + w; y2 = y + h; break;
    case PN_WR_GRADIENT_VERTICAL:
    default:                        y2 = y + h;             break;
    }

    grad = cairo_pattern_create_linear (x, y, x2, y2);
    cairo_pattern_add_color_stop_rgba (grad, 0.0,
                                       a->red, a->green, a->blue, a->alpha);
    cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                       b->red, b->green, b->blue, b->alpha);
    cairo_set_source (cr, grad);
    cairo_pattern_destroy (grad);
}

static void
pn_weather_report_paint_plot (PnNode  *node,
                              cairo_t *cr,
                              double   x,
                              double   y,
                              double   w,
                              double   h)
{
    PnWeatherReport          *self = PN_WEATHER_REPORT (node);
    PnWeatherReportPaintState ps;
    JsonObject               *data;

    pn_weather_report_get_paint_state (self, &ps);
    data = pn_weather_report_peek_data (self);

    /* Card background + hairline frame, always drawn so an empty card
     * reads as a deliberate surface rather than a hole in the canvas. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    set_background_source (&ps, cr, x, y, w, h);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, LINE_R, LINE_G, LINE_B);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    if (data == NULL)
    {
        paint_notice (cr, w, h, ICON_CLOUD,
                      "Waiting for weather" GLYPH_ELLIPSIS, NULL,
                      &ps.secondary_color);
    }
    else if (!obj_bool (data, "success", TRUE))
    {
        const gchar *out = obj_str (data, "output");
        paint_notice (cr, w, h, ICON_CLOUD,
                      "No reading", out, &ps.secondary_color);
    }
    else
    {
        paint_report (&ps, data, cr, w, h);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_weather_report_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_WEATHER_REPORT));

    node_class->paint_plot = pn_weather_report_paint_plot;
    /* The card is a fixed-layout reading surface, so the centred zoom
     * overlay should fit it while keeping the 280:173 aspect ratio
     * rather than stretching the type out of shape. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
