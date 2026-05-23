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
/*  PnAnalogMeter — logic tier (headless core).                        */
/*                                                                     */
/*  This file holds the GTK-free half of the Analog-Meter node: the     */
/*  GType, the mode enum, all properties (the five colour properties    */
/*  carried as PnColor), receive(), the JSON-path value reader, the     */
/*  damped-spring needle animation, the repaint throttle, get_size /    */
/*  get_header_height, lifecycle and the read seam the GUI tier paints  */
/*  from.  The cairo panel-meter drawing and the settings-dialog tabs   */
/*  live in the companion gui-tier file pn-analog-meter-gui.c, which     */
/*  installs those vfunc slots onto this class at editor startup (see    */
/*  pn_analog_meter_gui_install).  The headless runtime registers and   */
/*  runs this node without ever pulling GTK.                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-analog-meter.h"
#include "pn-json-path.h"
#include "pn-message.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Square footprint matching #PnDial: 220-px-wide header + 220-px     */
/*  square face below.  The face hosts a flat panel-meter look -- a    */
/*  rounded-corner plastic case, a bright inset face, and a needle    */
/*  pivoted at the bottom centre that sweeps the upper arc.            */
/* ------------------------------------------------------------------ */

#define PN_AM_WIDTH         220.0
#define PN_AM_HEADER_HEIGHT  40.0
#define PN_AM_GAP             4.0
#define PN_AM_FACE_SIZE     220.0
#define PN_AM_TOTAL_HEIGHT  (PN_AM_HEADER_HEIGHT + PN_AM_GAP + PN_AM_FACE_SIZE)

/* Repaint throttle: cap incoming-message-driven repaints at 30 Hz so
 * a high-rate feed cannot saturate the worksheet's redraw loop. */
#define PN_AM_MIN_REPAINT_INTERVAL_US (G_TIME_SPAN_MILLISECOND * 33)

/* Damped-spring tuning -- same values as #PnDial so the needle reads
 * as a moving-coil meter (underdamped, visible overshoot, settles in
 * about half a second on a typical step). */
#define PN_AM_SPRING_K   90.0
#define PN_AM_SPRING_C   11.0
#define PN_AM_ANIM_TICK_MS 16

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

typedef struct _PnAnalogMeterPrivate PnAnalogMeterPrivate;

struct _PnAnalogMeterPrivate
{
    /* Data binding. */
    gchar    *key;

    /* Scale geometry.  Angles are clockwise from 12 o'clock; with the
     * pivot at the lower-right corner of the face, a clock-angle of 0
     * is the needle pointing straight up and -90 is the needle
     * pointing horizontally to the left.  The default sweep
     * (-90 → 0) is the diagonal arc from lower-left to upper-right
     * found on classic AC voltmeter / ammeter panels -- the scale
     * fills the upper-left triangle of the face and the bottom band
     * is left clear for the accuracy-class string.                     */
    gdouble   min_value;
    gdouble   max_value;
    gdouble   start_angle;
    gdouble   end_angle;
    guint     major_ticks;
    guint     minor_ticks_per_major;

    /* Cosmetic text.  @unit is the dominant symbol printed in the
     * upper-LEFT corner of the face (the big "V" in the reference
     * photo) -- the corner opposite the pivot, which is the only
     * generously-sized empty region on a diagonal panel meter;
     * @accuracy_class is a small string printed at the lower-left
     * corner of the face (the "2.5" in the reference photo, naming
     * the meter's IEC accuracy class).  @mode selects which IEC
     * current-type symbol is painted directly below @unit --
     * #PN_ANALOG_METER_MODE_AC draws the sine-wave "~" (IEC
     * 60417-5032, alternating current); #PN_ANALOG_METER_MODE_DC
     * draws the solid-over-dashed "═‒‒‒" (IEC 60417-5031, direct
     * current); #PN_ANALOG_METER_MODE_NONE leaves the slot empty,
     * for meters whose unit ("Hz", "°C") needs no current-type
     * indicator.                                                       */
    gchar             *unit;
    gchar             *accuracy_class;
    PnAnalogMeterMode  mode;

    /* Colours.  @frame_color fills the moulded plastic case
     * (default a slightly-tinted off-white); @face_color fills the
     * inset paper face the scale arc sits on. */
    PnColor   frame_color;
    PnColor   face_color;
    PnColor   scale_color;
    PnColor   needle_color;
    PnColor   label_color;

    /* Live state -- same shape as #PnDial.  @target_value is the
     * most-recent value seen on the input (where the needle is
     * heading); @display_value is where the needle is right now,
     * lagged through a damped spring so the needle reads as a real
     * moving-coil meter rather than a teleporting indicator.        */
    gdouble   target_value;
    gdouble   display_value;
    gdouble   display_velocity;
    gboolean  has_value;

    guint     anim_id;
    gint64    anim_last_us;

    gint64    last_repaint_us;
    guint     pending_repaint_id;
};

G_DEFINE_TYPE_WITH_PRIVATE (PnAnalogMeter, pn_analog_meter, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_KEY,
    PROP_MIN_VALUE,
    PROP_MAX_VALUE,
    PROP_START_ANGLE,
    PROP_END_ANGLE,
    PROP_MAJOR_TICKS,
    PROP_MINOR_TICKS_PER_MAJOR,
    PROP_UNIT,
    PROP_ACCURACY_CLASS,
    PROP_MODE,
    PROP_FRAME_COLOR,
    PROP_FACE_COLOR,
    PROP_SCALE_COLOR,
    PROP_NEEDLE_COLOR,
    PROP_LABEL_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Mode enum type                                                     */
/* ------------------------------------------------------------------ */

GType
pn_analog_meter_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_ANALOG_METER_MODE_AC,
              "PN_ANALOG_METER_MODE_AC",   "AC"   },
            { PN_ANALOG_METER_MODE_DC,
              "PN_ANALOG_METER_MODE_DC",   "DC"   },
            { PN_ANALOG_METER_MODE_NONE,
              "PN_ANALOG_METER_MODE_NONE", "None" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnAnalogMeterMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static gboolean
parse_numeric_string (const gchar *s, gdouble *out)
{
    const gchar *p;
    gboolean     negative = FALSE;
    gdouble      v;

    if (s == NULL)
        return FALSE;

    p = s;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '+')      p++;
    else if (*p == '-') { negative = TRUE; p++; }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        guint64 u;
        gchar  *end = NULL;

        p += 2;
        if (*p == '\0')
            return FALSE;
        u = g_ascii_strtoull (p, &end, 16);
        if (end == NULL || *end != '\0' || end == p)
            return FALSE;
        v = (gdouble) u;
    }
    else
    {
        gchar *end = NULL;
        if (*p == '\0')
            return FALSE;
        v = g_ascii_strtod (p, &end);
        if (end == NULL || *end != '\0' || end == p)
            return FALSE;
    }

    if (negative) v = -v;
    if (!isfinite (v)) return FALSE;
    *out = v;
    return TRUE;
}

static gboolean
node_to_finite_double (JsonNode *node, gdouble *out)
{
    GType   vtype;
    gdouble v;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vtype = json_node_get_value_type (node);
    if (vtype == G_TYPE_INT64)
        v = (gdouble) json_node_get_int (node);
    else if (vtype == G_TYPE_DOUBLE)
        v = json_node_get_double (node);
    else if (vtype == G_TYPE_STRING)
        return parse_numeric_string (json_node_get_string (node), out);
    else
        return FALSE;

    if (!isfinite (v)) return FALSE;
    *out = v;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnAnalogMeter        *self = user_data;
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);

    priv->pending_repaint_id = 0;
    priv->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnAnalogMeter *self)
{
    PnAnalogMeterPrivate *priv    = pn_analog_meter_get_instance_private (self);
    gint64                now_us  = g_get_monotonic_time ();
    gint64                elapsed = now_us - priv->last_repaint_us;

    if (priv->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_AM_MIN_REPAINT_INTERVAL_US)
    {
        priv->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_AM_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);
        if (delay_ms == 0) delay_ms = 1;
        priv->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Damped-spring needle animation                                     */
/* ------------------------------------------------------------------ */

static gboolean
on_anim_tick (gpointer user_data)
{
    PnAnalogMeter        *self  = user_data;
    PnAnalogMeterPrivate *priv  = pn_analog_meter_get_instance_private (self);
    gint64                now   = g_get_monotonic_time ();
    gint64                dt_us = now - priv->anim_last_us;
    gdouble        dt;
    gdouble        displ;
    gdouble        accel;
    gdouble        scale;
    gdouble        eps_pos;
    gdouble        eps_vel;

    if (dt_us <= 0)
        dt_us = PN_AM_ANIM_TICK_MS * (gint64) 1000;
    priv->anim_last_us = now;

    dt = (gdouble) dt_us / (gdouble) G_TIME_SPAN_SECOND;
    if (dt > 0.05) dt = 0.05;

    displ = priv->target_value - priv->display_value;
    accel = PN_AM_SPRING_K * displ
          - PN_AM_SPRING_C * priv->display_velocity;

    priv->display_velocity += accel * dt;
    priv->display_value    += priv->display_velocity * dt;

    scale   = priv->max_value - priv->min_value;
    if (scale <= 0.0) scale = 1.0;
    eps_pos = fabs (scale) * 0.0005;
    eps_vel = fabs (scale) * 0.005;

    if (fabs (displ) < eps_pos &&
        fabs (priv->display_velocity) < eps_vel)
    {
        priv->display_value    = priv->target_value;
        priv->display_velocity = 0.0;
        priv->anim_id          = 0;
        pn_node_request_repaint (PN_NODE (self));
        return G_SOURCE_REMOVE;
    }

    pn_node_request_repaint (PN_NODE (self));
    return G_SOURCE_CONTINUE;
}

static void
start_anim (PnAnalogMeter *self)
{
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);

    if (priv->anim_id != 0)
        return;
    priv->anim_last_us = g_get_monotonic_time ();
    priv->anim_id      = g_timeout_add (PN_AM_ANIM_TICK_MS,
                                        on_anim_tick, self);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_analog_meter_receive (PnNode *node, PnMessage *message)
{
    PnAnalogMeter        *self = PN_ANALOG_METER (node);
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);
    JsonObject           *root;
    JsonNode             *value_node;
    gdouble               value;

    if (priv->key == NULL || *priv->key == '\0')
        return;

    root       = pn_json_lookup_root_for_message (message);
    value_node = pn_json_resolve_path (root, priv->key);

    if (!node_to_finite_double (value_node, &value))
    {
        json_object_unref (root);
        return;
    }
    json_object_unref (root);

    pn_analog_meter_set_value (self, value);
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo panel-meter painter lives in the gui tier               */
/*  (pn-analog-meter-gui.c) and cannot see this file's private        */
/*  instance struct, so it snapshots every field it needs to draw a    */
/*  frame through this single GTK-free accessor.  The snapshot is a    */
/*  plain by-value copy: the colours are #PnColor (layout-identical to */
/*  GdkRGBA) and the two text strings are borrowed pointers valid for  */
/*  the duration of the paint call, mirroring how the painter used to  */
/*  read priv->* directly.                                             */
/* ------------------------------------------------------------------ */

void
pn_analog_meter_get_paint_state (PnAnalogMeter            *self,
                                 PnAnalogMeterPaintState  *out)
{
    PnAnalogMeterPrivate *priv;

    g_return_if_fail (PN_IS_ANALOG_METER (self));
    g_return_if_fail (out != NULL);

    priv = pn_analog_meter_get_instance_private (self);

    out->min_value             = priv->min_value;
    out->max_value             = priv->max_value;
    out->start_angle           = priv->start_angle;
    out->end_angle             = priv->end_angle;
    out->major_ticks           = priv->major_ticks;
    out->minor_ticks_per_major = priv->minor_ticks_per_major;

    out->unit                  = priv->unit;
    out->accuracy_class        = priv->accuracy_class;
    out->mode                  = priv->mode;

    out->frame_color           = priv->frame_color;
    out->face_color            = priv->face_color;
    out->scale_color           = priv->scale_color;
    out->needle_color          = priv->needle_color;
    out->label_color           = priv->label_color;

    out->display_value         = priv->display_value;
    out->has_value             = priv->has_value;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_analog_meter_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_AM_WIDTH;
    if (out_h != NULL) *out_h = PN_AM_TOTAL_HEIGHT;
}

static double
pn_analog_meter_get_header_height (PnNode *node)
{
    (void) node;
    return PN_AM_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_analog_meter_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnAnalogMeter        *self = PN_ANALOG_METER (object);
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_KEY:                   g_value_set_string  (value, priv->key);                    break;
    case PROP_MIN_VALUE:             g_value_set_double  (value, priv->min_value);              break;
    case PROP_MAX_VALUE:             g_value_set_double  (value, priv->max_value);              break;
    case PROP_START_ANGLE:           g_value_set_double  (value, priv->start_angle);            break;
    case PROP_END_ANGLE:             g_value_set_double  (value, priv->end_angle);              break;
    case PROP_MAJOR_TICKS:           g_value_set_uint    (value, priv->major_ticks);            break;
    case PROP_MINOR_TICKS_PER_MAJOR: g_value_set_uint    (value, priv->minor_ticks_per_major);  break;
    case PROP_UNIT:                  g_value_set_string  (value, priv->unit);                   break;
    case PROP_ACCURACY_CLASS:        g_value_set_string  (value, priv->accuracy_class);         break;
    case PROP_MODE:                  g_value_set_enum    (value, priv->mode);                   break;
    case PROP_FRAME_COLOR:           g_value_set_boxed   (value, &priv->frame_color);           break;
    case PROP_FACE_COLOR:            g_value_set_boxed   (value, &priv->face_color);            break;
    case PROP_SCALE_COLOR:           g_value_set_boxed   (value, &priv->scale_color);           break;
    case PROP_NEEDLE_COLOR:          g_value_set_boxed   (value, &priv->needle_color);          break;
    case PROP_LABEL_COLOR:           g_value_set_boxed   (value, &priv->label_color);           break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_string_prop (PnAnalogMeter *self, gchar **slot,
                 const gchar *new_value, guint prop_id)
{
    const gchar *normalised = new_value != NULL ? new_value : "";

    if (g_strcmp0 (*slot, normalised) == 0)
        return;

    g_free (*slot);
    *slot = g_strdup (normalised);
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_double_prop (PnAnalogMeter *self, double *slot,
                 double new_value, guint prop_id)
{
    if (*slot == new_value)
        return;
    *slot = new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_uint_prop (PnAnalogMeter *self, guint *slot,
               guint new_value, guint prop_id)
{
    if (*slot == new_value)
        return;
    *slot = new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_enum_prop (PnAnalogMeter *self, gint *slot,
               gint new_value, guint prop_id)
{
    if (*slot == new_value)
        return;
    *slot = new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_color_prop (PnAnalogMeter *self, PnColor *slot,
                const PnColor *new_value, guint prop_id)
{
    if (new_value == NULL || pn_color_equal (slot, new_value))
        return;
    *slot = *new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
pn_analog_meter_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnAnalogMeter        *self = PN_ANALOG_METER (object);
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_KEY:
        set_string_prop (self, &priv->key, g_value_get_string (value), PROP_KEY);
        break;
    case PROP_UNIT:
        set_string_prop (self, &priv->unit, g_value_get_string (value), PROP_UNIT);
        break;
    case PROP_ACCURACY_CLASS:
        set_string_prop (self, &priv->accuracy_class,
                         g_value_get_string (value), PROP_ACCURACY_CLASS);
        break;
    case PROP_MODE:
        set_enum_prop (self, (gint *) &priv->mode,
                       g_value_get_enum (value), PROP_MODE);
        break;

    case PROP_MIN_VALUE:
        set_double_prop (self, &priv->min_value, g_value_get_double (value), PROP_MIN_VALUE);
        break;
    case PROP_MAX_VALUE:
        set_double_prop (self, &priv->max_value, g_value_get_double (value), PROP_MAX_VALUE);
        break;
    case PROP_START_ANGLE:
        set_double_prop (self, &priv->start_angle, g_value_get_double (value), PROP_START_ANGLE);
        break;
    case PROP_END_ANGLE:
        set_double_prop (self, &priv->end_angle, g_value_get_double (value), PROP_END_ANGLE);
        break;

    case PROP_MAJOR_TICKS:
        set_uint_prop (self, &priv->major_ticks, g_value_get_uint (value), PROP_MAJOR_TICKS);
        break;
    case PROP_MINOR_TICKS_PER_MAJOR:
        set_uint_prop (self, &priv->minor_ticks_per_major,
                       g_value_get_uint (value), PROP_MINOR_TICKS_PER_MAJOR);
        break;

    case PROP_FRAME_COLOR:
        set_color_prop (self, &priv->frame_color,  g_value_get_boxed (value), PROP_FRAME_COLOR);  break;
    case PROP_FACE_COLOR:
        set_color_prop (self, &priv->face_color,   g_value_get_boxed (value), PROP_FACE_COLOR);   break;
    case PROP_SCALE_COLOR:
        set_color_prop (self, &priv->scale_color,  g_value_get_boxed (value), PROP_SCALE_COLOR);  break;
    case PROP_NEEDLE_COLOR:
        set_color_prop (self, &priv->needle_color, g_value_get_boxed (value), PROP_NEEDLE_COLOR); break;
    case PROP_LABEL_COLOR:
        set_color_prop (self, &priv->label_color,  g_value_get_boxed (value), PROP_LABEL_COLOR);  break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_analog_meter_finalize (GObject *object)
{
    PnAnalogMeter        *self = PN_ANALOG_METER (object);
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);

    if (priv->pending_repaint_id != 0)
    {
        g_source_remove (priv->pending_repaint_id);
        priv->pending_repaint_id = 0;
    }
    if (priv->anim_id != 0)
    {
        g_source_remove (priv->anim_id);
        priv->anim_id = 0;
    }

    g_clear_pointer (&priv->key,            g_free);
    g_clear_pointer (&priv->unit,           g_free);
    g_clear_pointer (&priv->accuracy_class, g_free);

    G_OBJECT_CLASS (pn_analog_meter_parent_class)->finalize (object);
}

static void
pn_analog_meter_class_init (PnAnalogMeterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_analog_meter_get_property;
    object_class->set_property = pn_analog_meter_set_property;
    object_class->finalize     = pn_analog_meter_finalize;

    /* Logic + intrinsic geometry stay in the core class.  The cairo
     * panel-meter drawing (paint_plot + its skip-shadow / skip-zoom
     * flags) and the settings-dialog tabs (build_class_tabs) are
     * installed by the gui tier — see pn_analog_meter_gui_install() in
     * pn-analog-meter-gui.c. */
    node_class->receive           = pn_analog_meter_receive;
    node_class->get_size          = pn_analog_meter_get_size;
    node_class->get_header_height = pn_analog_meter_get_header_height;

    node_class->class_name        = "AnalogMeter";
    node_class->icon              = "\xef\x83\xa4";       /* fa-tachometer U+F0E4 */
    /* Same yellow as #PnDial.  Both nodes are gauge-style sinks that
     * read a single numeric value off the message, so palette-grouping
     * them by colour helps the eye spot a worksheet's metering row at
     * a glance. */
    node_class->color             = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_KEY] = g_param_spec_string (
            "key", "Key",
            "JSON path (\"/\"-separated, e.g. data/value) to the "
            "numeric value the needle points at",
            "data/value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MIN_VALUE] = g_param_spec_double (
            "min-value", "Min value",
            "Value at the start (low end) of the meter's scale",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_VALUE] = g_param_spec_double (
            "max-value", "Max value",
            "Value at the end (high end) of the meter's scale",
            -G_MAXDOUBLE, G_MAXDOUBLE, 300.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_START_ANGLE] = g_param_spec_double (
            "start-angle", "Start angle",
            "Angle (degrees, clockwise from 12 o'clock) of the needle "
            "at the scale's low end.  -90 points the needle horizontally "
            "to the left, which with the default pivot at the lower-"
            "right of the face puts the \"0\" tick at the lower-left "
            "corner.",
            -180.0, 180.0, -90.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_END_ANGLE] = g_param_spec_double (
            "end-angle", "End angle",
            "Angle (degrees, clockwise from 12 o'clock) of the needle "
            "at the scale's high end.  0 points the needle straight up, "
            "which with the default pivot at the lower-right of the "
            "face puts the high-end tick at the upper-right corner.",
            -180.0, 180.0, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAJOR_TICKS] = g_param_spec_uint (
            "major-ticks", "Major ticks",
            "Target upper bound on the number of major tick marks the "
            "meter draws along the scale.  The actual marks are placed "
            "at \"nice\" value positions ({1, 2, 5} \xc3\x97 10^n apart) "
            "derived from this target, the same heuristic the Dial node "
            "uses.",
            2, 50, 4,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MINOR_TICKS_PER_MAJOR] = g_param_spec_uint (
            "minor-ticks-per-major", "Minor ticks per major",
            "Number of minor tick marks drawn between two consecutive "
            "major ticks.  Set to 1 to suppress minor ticks.",
            1, 20, 10,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNIT] = g_param_spec_string (
            "unit", "Unit",
            "Dominant symbol printed near the top centre of the face -- "
            "typically a single letter (\"V\", \"A\", \"mA\") so it "
            "reads as the unit the meter measures at a single glance.",
            "V",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ACCURACY_CLASS] = g_param_spec_string (
            "accuracy-class", "Accuracy class",
            "Small string printed at the lower-left corner of the face "
            "(typically the IEC accuracy class -- \"2.5\", \"1.5\", "
            "\"0.5\").  Cosmetic only; does not affect any readings.",
            "2.5",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODE] = g_param_spec_enum (
            "mode", "Mode",
            "Picks which IEC current-type symbol is painted directly "
            "below the unit.  AC paints the sine-wave \"~\" (IEC "
            "60417-5032, alternating current); DC paints a solid line "
            "above three dashes (IEC 60417-5031, direct current); "
            "None leaves the slot empty, for meters whose unit "
            "(frequency, temperature, …) needs no current-type "
            "indicator at all.",
            PN_TYPE_ANALOG_METER_MODE,
            PN_ANALOG_METER_MODE_AC,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FRAME_COLOR] = g_param_spec_boxed (
            "frame-color", "Frame colour",
            "Fill colour of the moulded plastic case the meter sits "
            "in -- the outer rounded rectangle around the dial face.  "
            "The default is near-black, the moulded-bakelite look of a "
            "classic panel meter; pick an off-white for a warm-plastic "
            "instrument or a brand colour for an instrument cluster.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_FACE_COLOR] = g_param_spec_boxed (
            "face-color", "Face colour",
            "Base fill colour of the inset dial face",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_SCALE_COLOR] = g_param_spec_boxed (
            "scale-color", "Scale colour",
            "Colour of the tick marks, the scale arc and the digit labels",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_NEEDLE_COLOR] = g_param_spec_boxed (
            "needle-color", "Needle colour",
            "Fill colour of the needle body",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_LABEL_COLOR] = g_param_spec_boxed (
            "label-color", "Label colour",
            "Colour of the unit symbol, the AC \"~\" glyph and the "
            "accuracy-class string",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_analog_meter_init (PnAnalogMeter *self)
{
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);
    PnNode               *node = PN_NODE (self);

    priv->key                   = g_strdup ("data/value");
    priv->unit                  = g_strdup ("V");
    priv->accuracy_class        = g_strdup ("2.5");
    priv->mode                  = PN_ANALOG_METER_MODE_AC;

    priv->min_value             = 0.0;
    priv->max_value             = 300.0;
    priv->start_angle           = -90.0;
    priv->end_angle             = 0.0;
    priv->major_ticks           = 4;
    priv->minor_ticks_per_major = 10;

    priv->frame_color  = (PnColor){ 0.05, 0.05, 0.05, 1.0 };
    priv->face_color   = (PnColor){ 0.97, 0.97, 0.95, 1.0 };
    priv->scale_color  = (PnColor){ 0.05, 0.05, 0.05, 1.0 };
    priv->needle_color = (PnColor){ 0.05, 0.05, 0.05, 1.0 };
    priv->label_color  = (PnColor){ 0.05, 0.05, 0.05, 1.0 };

    priv->target_value     = priv->min_value;
    priv->display_value    = priv->min_value;
    priv->display_velocity = 0.0;
    priv->has_value        = FALSE;

    priv->anim_id      = 0;
    priv->anim_last_us = 0;

    priv->last_repaint_us    = 0;
    priv->pending_repaint_id = 0;

    {
        PnColor yellow = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &yellow);
    }
    pn_node_set_class_name (node, "AnalogMeter");
    pn_node_set_icon       (node, "\xef\x83\xa4");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnAnalogMeter *
pn_analog_meter_new (void)
{
    return g_object_new (PN_TYPE_ANALOG_METER, NULL);
}

void
pn_analog_meter_set_value (PnAnalogMeter *self, gdouble value)
{
    PnAnalogMeterPrivate *priv;

    g_return_if_fail (PN_IS_ANALOG_METER (self));
    if (!isfinite (value))
        return;

    priv = pn_analog_meter_get_instance_private (self);

    /* The very first value also flips the @has_value latch -- the
     * needle stays hidden until then so the parked-at-zero position
     * is visually distinguishable from a real zero reading.  Repaint
     * unconditionally on this transition even when the incoming value
     * happens to equal @target_value, otherwise the freshly-revealed
     * needle would not paint until the next message. */
    if (!priv->has_value)
    {
        priv->has_value = TRUE;
        if (priv->target_value != value)
        {
            priv->target_value = value;
            start_anim (self);
        }
        schedule_repaint (self);
        return;
    }

    if (priv->target_value == value)
        return;

    priv->target_value = value;
    start_anim (self);
    schedule_repaint (self);
}
