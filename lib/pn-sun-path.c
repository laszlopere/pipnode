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

#include "pn-sun-path.h"
#include "pn-astronomical.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Identical to #PnWeatherReport / #PnGraph: a 40 px header the        */
/*  worksheet paints in the standard Node-RED style, a small gap, then  */
/*  a 280 x 173 card.  Re-using the dimensions keeps the sink types     */
/*  visually interchangeable on the canvas.                            */
/* ------------------------------------------------------------------ */

#define PN_SP_WIDTH         280.0
#define PN_SP_HEADER_HEIGHT  40.0
#define PN_SP_GAP             4.0
#define PN_SP_CARD_HEIGHT   173.0
#define PN_SP_TOTAL_HEIGHT  (PN_SP_HEADER_HEIGHT + PN_SP_GAP + PN_SP_CARD_HEIGHT)

/* fa-sun-o U+F185 — the header / palette glyph, shared with the
 * Astronomical source node so the data-source / display pair reads as a
 * family. */
#define PN_SP_ICON         "\xef\x86\x85"

/* The Sun's full-day track, sampled at this resolution over a 24 h
 * window centred on the moment the reading arrived.  5-minute steps give
 * a smooth arc at any card size for a handful of microseconds of pure
 * NOAA math per message. */
#define PN_SP_PATH_STEP_S   300     /* 5 minutes  */
#define PN_SP_PATH_SPAN_S   86400   /* 24 hours   */
#define PN_SP_PATH_SAMPLES  ((PN_SP_PATH_SPAN_S / PN_SP_PATH_STEP_S) + 1)

/* Default camera. */
#define PN_SP_DEFAULT_YAW    30.0
#define PN_SP_DEFAULT_PITCH  24.0
#define PN_SP_PITCH_MIN       6.0
#define PN_SP_PITCH_MAX      84.0

/* ------------------------------------------------------------------ */
/*  Sky-gradient enum GType                                            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SP_GRADIENT (pn_sp_gradient_get_type ())

static GType
pn_sp_gradient_get_type (void)
{
    static gsize id = 0;
    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_SP_GRADIENT_NONE,       "PN_SP_GRADIENT_NONE",       "Solid (no gradient)" },
            { PN_SP_GRADIENT_VERTICAL,   "PN_SP_GRADIENT_VERTICAL",   "Vertical"            },
            { PN_SP_GRADIENT_HORIZONTAL, "PN_SP_GRADIENT_HORIZONTAL", "Horizontal"          },
            { PN_SP_GRADIENT_DIAGONAL,   "PN_SP_GRADIENT_DIAGONAL",   "Diagonal"            },
            { 0, NULL, NULL }
        };
        GType t = g_enum_register_static ("PnSpGradient", values);
        g_once_init_leave (&id, t);
    }
    return id;
}

struct _PnSunPath
{
    PnNode parent_instance;

    /* Latest reading, extracted from the message data bag.  @have_data
     * is %FALSE until the first message; @success mirrors the message's
     * own success flag. */
    gboolean have_data;
    gboolean success;
    gboolean sun_up;
    gdouble  sun_azimuth;
    gdouble  sun_altitude;
    gdouble  latitude;
    gdouble  longitude;
    gchar   *city;
    gchar   *country;
    gchar   *output;

    /* The Sun's track for the reading's day, recomputed from
     * @latitude/@longitude on every successful message.  %NULL / 0 until
     * then. */
    PnSunPathPoint *path;
    guint           n_path;

    /* Orbit camera (serialised so the chosen view survives a reload). */
    gdouble yaw;
    gdouble pitch;

    /* House heading: which way the ridge faces, spun by the mouse wheel
     * in the zoom overlay (serialised). */
    gdouble house_heading;

    /* Display config. */
    PnColor      sky_color;
    PnColor      sky_color2;
    PnSpGradient sky_gradient;
    PnColor      ground_color;
    PnColor      path_color;
    PnColor      sun_color;
    PnColor      text_color;
    gboolean     show_house;
};

G_DEFINE_TYPE (PnSunPath, pn_sun_path, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_VIEW_YAW,
    PROP_VIEW_PITCH,
    PROP_HOUSE_HEADING,
    PROP_SHOW_HOUSE,
    PROP_SKY_COLOR,
    PROP_SKY_COLOR2,
    PROP_SKY_GRADIENT,
    PROP_GROUND_COLOR,
    PROP_PATH_COLOR,
    PROP_SUN_COLOR,
    PROP_TEXT_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  JSON readers                                                       */
/* ------------------------------------------------------------------ */

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

static gchar *
obj_str_dup (JsonObject *o, const gchar *key)
{
    JsonNode *n;

    if (o == NULL || !json_object_has_member (o, key))
        return NULL;
    n = json_object_get_member (o, key);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return g_strdup (json_node_get_string (n));
}

/* ------------------------------------------------------------------ */
/*  Path computation                                                   */
/* ------------------------------------------------------------------ */

/* Re-sample the Sun's track for the day around @anchor (Unix seconds)
 * at @self->latitude / @self->longitude, replacing any earlier path.
 * The window is centred on @anchor so the live position the message
 * reports lands on the middle of the arc. */
static void
pn_sun_path_recompute_path (PnSunPath *self, gint64 anchor)
{
    PnSunPathPoint *pts = g_new (PnSunPathPoint, PN_SP_PATH_SAMPLES);
    guint           i;

    for (i = 0; i < PN_SP_PATH_SAMPLES; i++)
    {
        PnAstronomicalResult r;
        gint64 t = anchor - PN_SP_PATH_SPAN_S / 2
                          + (gint64) i * PN_SP_PATH_STEP_S;

        pn_astronomical_compute (self->latitude, self->longitude, t, &r);
        pts[i].azimuth  = r.azimuth;
        pts[i].altitude = r.altitude;
    }

    g_free (self->path);
    self->path   = pts;
    self->n_path = PN_SP_PATH_SAMPLES;
}

static void
pn_sun_path_clear_reading (PnSunPath *self)
{
    g_clear_pointer (&self->city,    g_free);
    g_clear_pointer (&self->country, g_free);
    g_clear_pointer (&self->output,  g_free);
    g_clear_pointer (&self->path,    g_free);
    self->n_path = 0;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_sun_path_receive (PnNode    *node,
                     PnMessage *message)
{
    PnSunPath  *self = PN_SUN_PATH (node);
    JsonObject *data = pn_message_get_data (message);
    gdouble     az, alt, lat, lon;
    gboolean    success, have_az, have_alt;

    if (data == NULL)
        return;

    success  = obj_bool (data, "success", TRUE);
    have_az  = obj_num  (data, "sun_azimuth",  &az);
    have_alt = obj_num  (data, "sun_altitude", &alt);

    /* A Sun Path card consumes astronomical readings only.  A *successful*
     * message that carries no Sun position is not one — most often a
     * Weather report wired in by mistake — so ignore it and keep showing
     * "Waiting for sun position" rather than parking the Sun at (0, 0).
     * Explicit failures (success = FALSE, e.g. an unknown city) still pass
     * through to drive the "No position" notice. */
    if (success && !(have_az && have_alt))
        return;

    self->have_data = TRUE;
    self->success   = success;

    g_clear_pointer (&self->city,    g_free);
    g_clear_pointer (&self->country, g_free);
    g_clear_pointer (&self->output,  g_free);
    self->city    = obj_str_dup (data, "city");
    self->country = obj_str_dup (data, "country");
    self->output  = obj_str_dup (data, "output");

    self->sun_up = obj_bool (data, "sun_up", FALSE);
    if (have_az)  self->sun_azimuth  = az;
    if (have_alt) self->sun_altitude = alt;

    /* Recompute the day's arc from the resolved coordinates.  Only a
     * successful reading carries usable coordinates; a failed lookup
     * drops the old arc so the card does not show a stale path under a
     * "no position" notice. */
    if (self->success &&
        obj_num (data, "latitude",  &lat) &&
        obj_num (data, "longitude", &lon))
    {
        self->latitude  = lat;
        self->longitude = lon;
        pn_sun_path_recompute_path (self, g_get_real_time () / G_USEC_PER_SEC);
    }
    else if (!self->success)
    {
        g_clear_pointer (&self->path, g_free);
        self->n_path = 0;
    }

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                        */
/* ------------------------------------------------------------------ */

void
pn_sun_path_rotate (PnSunPath *self, gdouble dyaw, gdouble dpitch)
{
    gdouble yaw, pitch;

    g_return_if_fail (PN_IS_SUN_PATH (self));

    if (dyaw == 0.0 && dpitch == 0.0)
        return;

    yaw = fmod (self->yaw + dyaw, 360.0);
    if (yaw < 0.0)
        yaw += 360.0;
    pitch = CLAMP (self->pitch + dpitch, PN_SP_PITCH_MIN, PN_SP_PITCH_MAX);

    if (yaw != self->yaw)
    {
        self->yaw = yaw;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VIEW_YAW]);
    }
    if (pitch != self->pitch)
    {
        self->pitch = pitch;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VIEW_PITCH]);
    }

    pn_node_request_repaint (PN_NODE (self));
}

void
pn_sun_path_spin_house (PnSunPath *self, gdouble ddeg)
{
    gdouble h;

    g_return_if_fail (PN_IS_SUN_PATH (self));

    if (ddeg == 0.0)
        return;

    h = fmod (self->house_heading + ddeg, 360.0);
    if (h < 0.0)
        h += 360.0;

    if (h != self->house_heading)
    {
        self->house_heading = h;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOUSE_HEADING]);
        pn_node_request_repaint (PN_NODE (self));
    }
}

/* Mouse-wheel handler, invoked by the worksheet while the card is the
 * zoom overlay: spin the house a chunk of a turn per notch. */
static void
pn_sun_path_scroll (PnNode *node, double dy)
{
    pn_sun_path_spin_house (PN_SUN_PATH (node), dy * 12.0);
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_sun_path_get_snapshot (PnSunPath *self, PnSunPathSnapshot *out)
{
    g_return_if_fail (PN_IS_SUN_PATH (self));
    g_return_if_fail (out != NULL);

    out->yaw           = self->yaw;
    out->pitch         = self->pitch;
    out->house_heading = self->house_heading;

    out->have_data    = self->have_data;
    out->success      = self->success;
    out->sun_up       = self->sun_up;
    out->sun_azimuth  = self->sun_azimuth;
    out->sun_altitude = self->sun_altitude;

    out->path         = self->path;
    out->n_path       = self->n_path;

    out->city         = self->city;
    out->country      = self->country;
    out->output       = self->output;

    out->sky_color    = self->sky_color;
    out->sky_color2   = self->sky_color2;
    out->sky_gradient = self->sky_gradient;
    out->ground_color = self->ground_color;
    out->path_color   = self->path_color;
    out->sun_color    = self->sun_color;
    out->text_color   = self->text_color;
    out->show_house   = self->show_house;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_sun_path_get_size (PnNode *node,
                      double *out_width,
                      double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_SP_WIDTH;
    if (out_height != NULL) *out_height = PN_SP_TOTAL_HEIGHT;
}

static double
pn_sun_path_get_header_height (PnNode *node)
{
    (void) node;
    return PN_SP_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sun_path_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    PnSunPath *self = PN_SUN_PATH (object);

    switch (prop_id)
    {
    case PROP_VIEW_YAW:      g_value_set_double  (value, self->yaw);          break;
    case PROP_VIEW_PITCH:    g_value_set_double  (value, self->pitch);        break;
    case PROP_HOUSE_HEADING: g_value_set_double  (value, self->house_heading);break;
    case PROP_SHOW_HOUSE:    g_value_set_boolean (value, self->show_house);   break;
    case PROP_SKY_GRADIENT:  g_value_set_enum    (value, self->sky_gradient); break;
    case PROP_SKY_COLOR:     g_value_set_boxed   (value, &self->sky_color);   break;
    case PROP_SKY_COLOR2:    g_value_set_boxed   (value, &self->sky_color2);  break;
    case PROP_GROUND_COLOR:  g_value_set_boxed   (value, &self->ground_color);break;
    case PROP_PATH_COLOR:    g_value_set_boxed   (value, &self->path_color);  break;
    case PROP_SUN_COLOR:     g_value_set_boxed   (value, &self->sun_color);   break;
    case PROP_TEXT_COLOR:    g_value_set_boxed   (value, &self->text_color);  break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* Assign @value (a boxed #PnColor) to *@dest, repainting and notifying
 * via @pspec only when the colour actually changes. */
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
pn_sun_path_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    PnSunPath *self = PN_SUN_PATH (object);

    switch (prop_id)
    {
    case PROP_VIEW_YAW:
        {
            gdouble v = fmod (g_value_get_double (value), 360.0);
            if (v < 0.0) v += 360.0;
            if (self->yaw != v)
            {
                self->yaw = v;
                g_object_notify_by_pspec (object, props[PROP_VIEW_YAW]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_VIEW_PITCH:
        {
            gdouble v = CLAMP (g_value_get_double (value),
                               PN_SP_PITCH_MIN, PN_SP_PITCH_MAX);
            if (self->pitch != v)
            {
                self->pitch = v;
                g_object_notify_by_pspec (object, props[PROP_VIEW_PITCH]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_HOUSE_HEADING:
        {
            gdouble v = fmod (g_value_get_double (value), 360.0);
            if (v < 0.0) v += 360.0;
            if (self->house_heading != v)
            {
                self->house_heading = v;
                g_object_notify_by_pspec (object, props[PROP_HOUSE_HEADING]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_SHOW_HOUSE:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->show_house != v)
            {
                self->show_house = v;
                g_object_notify_by_pspec (object, props[PROP_SHOW_HOUSE]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_SKY_GRADIENT:
        {
            PnSpGradient v = g_value_get_enum (value);
            if (self->sky_gradient != v)
            {
                self->sky_gradient = v;
                g_object_notify_by_pspec (object, props[PROP_SKY_GRADIENT]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_SKY_COLOR:
        set_rgba_prop (object, value, &self->sky_color,    props[PROP_SKY_COLOR]);    break;
    case PROP_SKY_COLOR2:
        set_rgba_prop (object, value, &self->sky_color2,   props[PROP_SKY_COLOR2]);   break;
    case PROP_GROUND_COLOR:
        set_rgba_prop (object, value, &self->ground_color, props[PROP_GROUND_COLOR]); break;
    case PROP_PATH_COLOR:
        set_rgba_prop (object, value, &self->path_color,   props[PROP_PATH_COLOR]);   break;
    case PROP_SUN_COLOR:
        set_rgba_prop (object, value, &self->sun_color,    props[PROP_SUN_COLOR]);    break;
    case PROP_TEXT_COLOR:
        set_rgba_prop (object, value, &self->text_color,   props[PROP_TEXT_COLOR]);   break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sun_path_finalize (GObject *object)
{
    PnSunPath *self = PN_SUN_PATH (object);

    pn_sun_path_clear_reading (self);

    G_OBJECT_CLASS (pn_sun_path_parent_class)->finalize (object);
}

static void
pn_sun_path_class_init (PnSunPathClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_sun_path_get_property;
    object_class->set_property = pn_sun_path_set_property;
    object_class->finalize     = pn_sun_path_finalize;

    node_class->receive           = pn_sun_path_receive;
    node_class->get_size          = pn_sun_path_get_size;
    node_class->get_header_height  = pn_sun_path_get_header_height;
    /* Mouse wheel over the lifted card spins the house (GTK-free, so the
     * slot is wired here in the core rather than the gui tier). */
    node_class->scroll            = pn_sun_path_scroll;
    /* The cairo 3D scene painter (paint_plot + the keep-aspect zoom flag)
     * is installed onto this class by the gui tier —
     * pn_sun_path_gui_install() in pn-sun-path-gui.c — so the headless
     * core carries no GTK/cairo/pango. */

    node_class->palette_icon = PN_SP_ICON;
    node_class->class_name   = "Sun Path";
    node_class->icon         = PN_SP_ICON;
    node_class->color        = (PnColor){ 0.95, 0.66, 0.20, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        /* The view angles persist as properties but are driven by the
         * overlay drag, not the dialog, so keep them out of the form. */
        pn_settings_schema_row       (schema, "view-yaw", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "view-yaw", PN_ROW_FLAG_HIDDEN);
        pn_settings_schema_row       (schema, "view-pitch", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "view-pitch", PN_ROW_FLAG_HIDDEN);
        /* The house heading is spun by the mouse wheel, not the dialog. */
        pn_settings_schema_row       (schema, "house-heading", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "house-heading", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_VIEW_YAW] = g_param_spec_double (
            "view-yaw", "View yaw",
            "Camera heading in degrees; driven by dragging the lifted card",
            0.0, 360.0, PN_SP_DEFAULT_YAW,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_VIEW_PITCH] = g_param_spec_double (
            "view-pitch", "View pitch",
            "Camera tilt above the horizon plane in degrees; driven by "
            "dragging the lifted card",
            PN_SP_PITCH_MIN, PN_SP_PITCH_MAX, PN_SP_DEFAULT_PITCH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HOUSE_HEADING] = g_param_spec_double (
            "house-heading", "House heading",
            "Direction the house ridge faces in degrees; spun by the mouse "
            "wheel over the lifted card",
            0.0, 360.0, 90.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_HOUSE] = g_param_spec_boolean (
            "show-house", "Show house",
            "Draw the reference house at the centre of the dome",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SKY_GRADIENT] = g_param_spec_enum (
            "background-gradient", "Background gradient",
            "Whether the sky background is a flat fill or a linear gradient "
            "from \"background-color\" to \"background-color2\", and which "
            "way it runs",
            PN_TYPE_SP_GRADIENT, PN_SP_GRADIENT_VERTICAL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SKY_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Sky background fill, and the starting colour of the background "
            "gradient",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SKY_COLOR2] = g_param_spec_boxed (
            "background-color2", "Background gradient end",
            "The colour the background gradient runs to; ignored unless "
            "\"background-gradient\" selects a direction",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GROUND_COLOR] = g_param_spec_boxed (
            "ground-color", "Ground colour",
            "Fill of the translucent ground disc carrying the compass marks",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PATH_COLOR] = g_param_spec_boxed (
            "path-color", "Path colour",
            "Colour of the Sun's daily track arc",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SUN_COLOR] = g_param_spec_boxed (
            "sun-color", "Sun colour",
            "Colour of the Sun glyph riding the arc",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the compass labels and the place / readout text",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_sun_path_init (PnSunPath *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  amber = { 0.95, 0.66, 0.20, 1.0 };

    self->have_data    = FALSE;
    self->success      = TRUE;
    self->yaw          = PN_SP_DEFAULT_YAW;
    self->pitch        = PN_SP_DEFAULT_PITCH;
    self->house_heading = 90.0;   /* ridge east-west, as first shipped */
    self->show_house   = TRUE;

    /* A dusk-sky default: a soft blue fading to a warm horizon, a muted
     * green-grey ground, a warm track and a bright Sun.  Chosen so the
     * card looks finished the moment it is dropped, before any reading. */
    self->sky_color    = (PnColor){ 0.36, 0.52, 0.74, 1.0 };
    self->sky_color2   = (PnColor){ 0.86, 0.83, 0.72, 1.0 };
    self->sky_gradient = PN_SP_GRADIENT_VERTICAL;
    self->ground_color = (PnColor){ 0.42, 0.55, 0.50, 0.55 };
    self->path_color   = (PnColor){ 0.13, 0.45, 0.40, 1.0 };
    self->sun_color    = (PnColor){ 0.98, 0.80, 0.18, 1.0 };
    self->text_color   = (PnColor){ 0.13, 0.16, 0.20, 1.0 };

    pn_node_set_class_name (node, "Sun Path");
    pn_node_set_icon       (node, PN_SP_ICON);
    pn_node_set_color      (node, &amber);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSunPath *
pn_sun_path_new (void)
{
    return g_object_new (PN_TYPE_SUN_PATH, NULL);
}
