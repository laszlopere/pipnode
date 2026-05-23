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

#include <json-glib/json-glib.h>
#include <math.h>

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

/* ------------------------------------------------------------------ */
/*  Display-unit enums                                                 */
/*                                                                     */
/*  The Weather node always reports metric (temperature in °C, wind in */
/*  km/h, pressure in hPa); these let the card convert to the unit the */
/*  user prefers.  Each is a registered enum so the node-settings       */
/*  dialog renders it as a combobox, and so it serialises by nick.     */
/* ------------------------------------------------------------------ */

/* PnWrTempUnit / PnWrWindUnit / PnWrPressUnit (and PnWrGradient below)
 * are published in pn-weather-report.h — both the core property pspecs
 * and the gui-tier painter need the enum values — so only the GType
 * registrations live here. */

#define PN_TYPE_WR_TEMP_UNIT  (pn_wr_temp_unit_get_type ())
#define PN_TYPE_WR_WIND_UNIT  (pn_wr_wind_unit_get_type ())
#define PN_TYPE_WR_PRESS_UNIT (pn_wr_press_unit_get_type ())

static GType
pn_wr_temp_unit_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_WR_TEMP_CELSIUS,    "PN_WR_TEMP_CELSIUS",    "Celsius (\xc2\xb0""C)"    },
            { PN_WR_TEMP_FAHRENHEIT, "PN_WR_TEMP_FAHRENHEIT", "Fahrenheit (\xc2\xb0""F)" },
            { PN_WR_TEMP_KELVIN,     "PN_WR_TEMP_KELVIN",     "Kelvin (K)"               },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnWrTempUnit", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

static GType
pn_wr_wind_unit_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_WR_WIND_KMH,   "PN_WR_WIND_KMH",   "km/h"   },
            { PN_WR_WIND_MS,    "PN_WR_WIND_MS",    "m/s"    },
            { PN_WR_WIND_MPH,   "PN_WR_WIND_MPH",   "mph"    },
            { PN_WR_WIND_KNOTS, "PN_WR_WIND_KNOTS", "knots"  },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnWrWindUnit", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

static GType
pn_wr_press_unit_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_WR_PRESS_HPA,  "PN_WR_PRESS_HPA",  "hPa"  },
            { PN_WR_PRESS_KPA,  "PN_WR_PRESS_KPA",  "kPa"  },
            { PN_WR_PRESS_INHG, "PN_WR_PRESS_INHG", "inHg" },
            { PN_WR_PRESS_MMHG, "PN_WR_PRESS_MMHG", "mmHg" },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnWrPressUnit", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

/* ------------------------------------------------------------------ */
/*  Background-gradient direction                                      */
/*                                                                     */
/*  The card background is normally a flat fill, but the user can ask  */
/*  for a two-stop linear gradient running between the primary         */
/*  background colour and a second colour.  Registered as an enum so   */
/*  the settings dialog renders it as a combobox and it serialises by  */
/*  nick.                                                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WR_GRADIENT (pn_wr_gradient_get_type ())

static GType
pn_wr_gradient_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_WR_GRADIENT_NONE,       "PN_WR_GRADIENT_NONE",       "Solid (no gradient)" },
            { PN_WR_GRADIENT_VERTICAL,   "PN_WR_GRADIENT_VERTICAL",   "Vertical"            },
            { PN_WR_GRADIENT_HORIZONTAL, "PN_WR_GRADIENT_HORIZONTAL", "Horizontal"          },
            { PN_WR_GRADIENT_DIAGONAL,   "PN_WR_GRADIENT_DIAGONAL",   "Diagonal"            },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnWrGradient", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

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

    /* Display units the card converts the (metric) reading into. */
    PnWrTempUnit  temp_unit;
    PnWrWindUnit  wind_unit;
    PnWrPressUnit press_unit;

    /* Colours.  @font_color paints the primary text (place, temperature,
     * description, time, the detail-tile icons and values);
     * @secondary_color paints the muted text (the feels-like sub-line,
     * the date, the tile captions, and the "Waiting for weather" /
     * failure notices).  @bg_color fills the card; when @bg_gradient is
     * not NONE a linear gradient runs from @bg_color to @bg_color2. */
    PnColor       font_color;
    PnColor       secondary_color;
    PnColor       bg_color;
    PnColor       bg_color2;
    PnWrGradient  bg_gradient;

    /* Drives the on-card clock: a 15 s timeout that repaints so the
     * displayed local time tracks the wall clock independently of the
     * (much slower) weather refresh.  0 when not running. */
    guint         clock_source;
};

G_DEFINE_TYPE (PnWeatherReport, pn_weather_report, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SHOW_DETAILS,
    PROP_TEMP_UNIT,
    PROP_WIND_UNIT,
    PROP_PRESS_UNIT,
    PROP_FONT_COLOR,
    PROP_SECONDARY_COLOR,
    PROP_BG_COLOR,
    PROP_BG_COLOR2,
    PROP_BG_GRADIENT,
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

/* obj_str() is painter-only and now lives in the gui tier. */

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

/* cardinal() and format_long_date() are painter-only and now live in the
 * gui tier (pn-weather-report-gui.c). */

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
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The gui-tier cairo/Pango painter (pn-weather-report-gui.c) reads    */
/*  the display config through a snapshot and the latest reading        */
/*  through a borrowed-pointer accessor.                                */
/* ------------------------------------------------------------------ */

void
pn_weather_report_get_paint_state (PnWeatherReport           *self,
                                   PnWeatherReportPaintState *out)
{
    g_return_if_fail (PN_IS_WEATHER_REPORT (self));
    g_return_if_fail (out != NULL);

    out->show_details    = self->show_details;
    out->temp_unit       = self->temp_unit;
    out->wind_unit       = self->wind_unit;
    out->press_unit      = self->press_unit;

    out->font_color      = self->font_color;
    out->secondary_color = self->secondary_color;
    out->bg_color        = self->bg_color;
    out->bg_color2       = self->bg_color2;
    out->bg_gradient     = self->bg_gradient;
}

JsonObject *
pn_weather_report_peek_data (PnWeatherReport *self)
{
    g_return_val_if_fail (PN_IS_WEATHER_REPORT (self), NULL);
    return self->data;
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
    case PROP_TEMP_UNIT:
        g_value_set_enum (value, self->temp_unit);
        break;
    case PROP_WIND_UNIT:
        g_value_set_enum (value, self->wind_unit);
        break;
    case PROP_PRESS_UNIT:
        g_value_set_enum (value, self->press_unit);
        break;
    case PROP_FONT_COLOR:
        g_value_set_boxed (value, &self->font_color);
        break;
    case PROP_SECONDARY_COLOR:
        g_value_set_boxed (value, &self->secondary_color);
        break;
    case PROP_BG_COLOR:
        g_value_set_boxed (value, &self->bg_color);
        break;
    case PROP_BG_COLOR2:
        g_value_set_boxed (value, &self->bg_color2);
        break;
    case PROP_BG_GRADIENT:
        g_value_set_enum (value, self->bg_gradient);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* Assign @value (a boxed #PnColor) to *@dest, repainting and notifying
 * via @pspec only when the colour actually changes.  A %NULL boxed value
 * (possible from a malformed deserialisation) is ignored. */
static void
set_rgba_prop (GObject *object, const GValue *value, PnColor *dest,
               GParamSpec *pspec)
{
    const PnColor *c = g_value_get_boxed (value);

    if (c != NULL && !pn_color_equal (dest, c))
    {
        *dest = *c;
        g_object_notify_by_pspec (object, pspec);
        pn_node_request_repaint (PN_NODE (object));
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
    case PROP_TEMP_UNIT:
        {
            PnWrTempUnit v = g_value_get_enum (value);
            if (self->temp_unit != v)
            {
                self->temp_unit = v;
                g_object_notify_by_pspec (object, props[PROP_TEMP_UNIT]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_WIND_UNIT:
        {
            PnWrWindUnit v = g_value_get_enum (value);
            if (self->wind_unit != v)
            {
                self->wind_unit = v;
                g_object_notify_by_pspec (object, props[PROP_WIND_UNIT]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_PRESS_UNIT:
        {
            PnWrPressUnit v = g_value_get_enum (value);
            if (self->press_unit != v)
            {
                self->press_unit = v;
                g_object_notify_by_pspec (object, props[PROP_PRESS_UNIT]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_FONT_COLOR:
        set_rgba_prop (object, value, &self->font_color,
                       props[PROP_FONT_COLOR]);
        break;
    case PROP_SECONDARY_COLOR:
        set_rgba_prop (object, value, &self->secondary_color,
                       props[PROP_SECONDARY_COLOR]);
        break;
    case PROP_BG_COLOR:
        set_rgba_prop (object, value, &self->bg_color,
                       props[PROP_BG_COLOR]);
        break;
    case PROP_BG_COLOR2:
        set_rgba_prop (object, value, &self->bg_color2,
                       props[PROP_BG_COLOR2]);
        break;
    case PROP_BG_GRADIENT:
        {
            PnWrGradient v = g_value_get_enum (value);
            if (self->bg_gradient != v)
            {
                self->bg_gradient = v;
                g_object_notify_by_pspec (object, props[PROP_BG_GRADIENT]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Clock tick                                                         */
/* ------------------------------------------------------------------ */

/* Fires every 15 s to advance the on-card clock.  The time is only drawn
 * on a successful reading, so a waiting / failed node is left untouched
 * and stays quiet on the canvas. */
static gboolean
pn_weather_report_clock_tick (gpointer user_data)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (user_data);

    if (self->data != NULL && obj_bool (self->data, "success", TRUE))
        pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_weather_report_finalize (GObject *object)
{
    PnWeatherReport *self = PN_WEATHER_REPORT (object);

    if (self->clock_source != 0)
    {
        g_source_remove (self->clock_source);
        self->clock_source = 0;
    }

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
    /* The cairo/Pango card painter (paint_plot + its keep-aspect zoom
     * flag) is installed onto this class by the gui tier —
     * pn_weather_report_gui_install() in pn-weather-report-gui.c — so
     * the headless core carries no GTK/cairo/pango. */

    node_class->palette_icon = PN_WR_ICON;
    node_class->class_name   = "Weather Report";
    node_class->icon         = PN_WR_ICON;
    node_class->color        = (PnColor){ 0.30, 0.60, 0.85, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    props[PROP_SHOW_DETAILS] = g_param_spec_boolean (
            "show-details", "Show details",
            "Draw the bottom row of detail tiles (humidity, wind, "
            "pressure, cloud cover) under the headline conditions",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEMP_UNIT] = g_param_spec_enum (
            "temperature-unit", "Temperature unit",
            "Unit the temperature and feels-like values are shown in; the "
            "card converts from the Celsius the Weather node reports",
            PN_TYPE_WR_TEMP_UNIT, PN_WR_TEMP_CELSIUS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WIND_UNIT] = g_param_spec_enum (
            "wind-unit", "Wind speed unit",
            "Unit the wind speed is shown in; the card converts from the "
            "km/h the Weather node reports",
            PN_TYPE_WR_WIND_UNIT, PN_WR_WIND_KMH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PRESS_UNIT] = g_param_spec_enum (
            "pressure-unit", "Air pressure unit",
            "Unit the air pressure is shown in; the card converts from the "
            "hPa the Weather node reports",
            PN_TYPE_WR_PRESS_UNIT, PN_WR_PRESS_HPA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FONT_COLOR] = g_param_spec_boxed (
            "font-color", "Font colour",
            "Colour of the primary text and detail-tile icons: the place "
            "name, temperature, description, time and tile values",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SECONDARY_COLOR] = g_param_spec_boxed (
            "secondary-font-color", "Secondary font colour",
            "Colour of the muted text: the feels-like sub-line, the date, "
            "the tile captions and the \"Waiting for weather\" / failure "
            "notices",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BG_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Card background fill, and the starting colour when a "
            "background gradient is enabled",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BG_COLOR2] = g_param_spec_boxed (
            "background-color2", "Background gradient end",
            "The colour the background gradient runs to; ignored unless "
            "\"background-gradient\" selects a direction",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BG_GRADIENT] = g_param_spec_enum (
            "background-gradient", "Background gradient",
            "Whether the background is a flat fill or a linear gradient "
            "from \"background-color\" to \"background-color2\", and which "
            "way the gradient runs",
            PN_TYPE_WR_GRADIENT, PN_WR_GRADIENT_NONE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_weather_report_init (PnWeatherReport *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  sky  = { 0.30, 0.60, 0.85, 1.0 };

    self->data         = NULL;
    self->show_details = TRUE;
    self->temp_unit    = PN_WR_TEMP_CELSIUS;
    self->wind_unit    = PN_WR_WIND_KMH;
    self->press_unit   = PN_WR_PRESS_HPA;

    /* Colour defaults reproduce the original monochrome card exactly:
     * near-black ink, mid-grey secondary text, white background.  The
     * gradient is off, so background-color2 only matters once the user
     * turns it on; a soft sky-blue makes that an attractive one-click
     * change rather than a white-to-white no-op. */
    self->font_color      = (PnColor){ INK_R, INK_G, INK_B, 1.0 };
    self->secondary_color = (PnColor){ MUT_R, MUT_G, MUT_B, 1.0 };
    self->bg_color        = (PnColor){ 1.0, 1.0, 1.0, 1.0 };
    self->bg_color2       = (PnColor){ 0.80, 0.89, 0.97, 1.0 };
    self->bg_gradient     = PN_WR_GRADIENT_NONE;

    pn_node_set_class_name (node, "Weather Report");
    pn_node_set_icon       (node, PN_WR_ICON);
    pn_node_set_color (node, &sky);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* Tick the on-card clock every 15 s so the displayed local time keeps
     * up with the wall clock, independently of the slow weather refresh. */
    self->clock_source = g_timeout_add_seconds (
            15, pn_weather_report_clock_tick, self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWeatherReport *
pn_weather_report_new (void)
{
    return g_object_new (PN_TYPE_WEATHER_REPORT, NULL);
}
