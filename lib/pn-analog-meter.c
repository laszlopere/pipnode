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

#include "pn-analog-meter.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-node-dialog-helpers.h"

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
    GdkRGBA   frame_color;
    GdkRGBA   face_color;
    GdkRGBA   scale_color;
    GdkRGBA   needle_color;
    GdkRGBA   label_color;

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

static inline double
clock_to_cairo (double clock_deg)
{
    return (clock_deg - 90.0) * M_PI / 180.0;
}

static double
value_to_angle (PnAnalogMeter *self, double v)
{
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);
    double span = priv->max_value - priv->min_value;
    double t;

    if (span <= 0.0)
        return priv->start_angle;

    if (v < priv->min_value) v = priv->min_value;
    if (v > priv->max_value) v = priv->max_value;

    t = (v - priv->min_value) / span;
    return priv->start_angle + t * (priv->end_angle - priv->start_angle);
}

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
/*  Painting                                                           */
/*                                                                     */
/*  The painter expects to be called with the cairo origin at the      */
/*  top-left of the face rectangle.  Inside the painter we translate   */
/*  to the dial pivot (bottom centre of the face, slightly inset) so   */
/*  every angle-based helper -- the same clock_to_cairo helper as      */
/*  #PnDial -- can pretend the pivot is at (0, 0) and 12 o'clock is    */
/*  straight up.                                                       */
/* ------------------------------------------------------------------ */

/** Trace a rounded rectangle path of size @w × @h centred on (0, 0). */
static void
rounded_rect_path (cairo_t *cr,
                   double   w,
                   double   h,
                   double   r)
{
    double x0 = -w * 0.5;
    double y0 = -h * 0.5;
    double x1 = +w * 0.5;
    double y1 = +h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x1 - r, y0 + r, r, -M_PI_2, 0.0);
    cairo_arc (cr, x1 - r, y1 - r, r, 0.0,      M_PI_2);
    cairo_arc (cr, x0 + r, y1 - r, r, M_PI_2,   M_PI);
    cairo_arc (cr, x0 + r, y0 + r, r, M_PI,     3.0 * M_PI_2);
    cairo_close_path (cr);
}

/** Paint the plastic case: a slightly off-white rounded rectangle the
 *  size of the whole face, with a subtle dark stroke around it and a
 *  highlight along the top edge so the case reads as a moulded plastic
 *  housing catching light from the upper-left like the rest of the
 *  worksheet.  The inset interior face -- where the dial scale lives
 *  -- is painted separately by paint_inner(); keeping the two passes
 *  separate lets the case keep its slightly-tinted plastic look (a
 *  warm off-white) while the inner well stays a cleaner paper-white,
 *  so the bezel still reads as a distinct moulded frame around the
 *  dial face. */
static void
paint_case (cairo_t *cr, double w, double h, const GdkRGBA *frame)
{
    double r = 8.0;

    /* Body. */
    cairo_set_source_rgba (cr, frame->red, frame->green, frame->blue,
                           frame->alpha);
    rounded_rect_path (cr, w, h, r);
    cairo_fill (cr);

    /* Inner border highlight: light line just inside the body's edge,
     * so the case reads as a moulded shell with a slight bevel.  The
     * white-translucent stroke layers on top of whatever the user
     * picked for @frame -- it reads as a subtle bevel cue on a light
     * default frame and turns into a brighter rim on a dark frame,
     * both of which match the way a moulded plastic case actually
     * catches light from the upper-left of the worksheet. */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.65);
    cairo_set_line_width (cr, 1.2);
    rounded_rect_path (cr, w - 3.0, h - 3.0, r - 1.0);
    cairo_stroke (cr);

    /* Outer hairline: a thin dark stroke on the very outside so the
     * case has a defined edge against the worksheet background -- a
     * frame colour close to the canvas tone (the default near-white
     * grey, a beige, ...) would otherwise dissolve into the surrounding
     * canvas at typical zoom levels. */
    cairo_set_source_rgba (cr, 0.20, 0.20, 0.22, 0.85);
    cairo_set_line_width (cr, 0.8);
    rounded_rect_path (cr, w, h, r);
    cairo_stroke (cr);
}

/** Paint the inset white face the scale sits on.  Rectangle with
 *  slightly rounded corners, inset a few px from the case edge so the
 *  moulded plastic bezel shows around it.  The face fill is the
 *  user-configurable @face_color (default near-white) and the inset
 *  rim is darkened with a thin dark line so the face reads as a
 *  recessed paper card under the bezel. */
static void
paint_inner (cairo_t *cr, double w, double h, const GdkRGBA *face)
{
    double iw = w - 16.0;
    double ih = h - 16.0;
    double r  = 4.0;

    /* Soft shadow inset to suggest the face sits below the bezel
     * surface; one translucent dark stroke just outside the inner
     * rect, then the rect itself in @face_color. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.25);
    cairo_set_line_width (cr, 1.6);
    rounded_rect_path (cr, iw + 2.0, ih + 2.0, r + 1.0);
    cairo_stroke (cr);

    cairo_set_source_rgba (cr, face->red, face->green, face->blue,
                           face->alpha);
    rounded_rect_path (cr, iw, ih, r);
    cairo_fill (cr);
}

/** Pick a "nice" tick-spacing step for the meter's scale.
 *
 *  Two-stage selection:
 *
 *    1.  Pure matplotlib / gnuplot heuristic: round the raw
 *        range / (target - 1) up to the next entry in
 *        {1, 2, 5} × 10^n.  This is the fallback when no other
 *        candidate fits.
 *
 *    2.  Bias toward steps that divide @range evenly.  A step
 *        that does not divide @range cleanly leaves a labelless
 *        stub between the last major tick and the end of the
 *        arc -- e.g. range = 5000 with the heuristic's step 2000
 *        labels 0 / 2000 / 4000 and walks 9 minor ticks off to
 *        5000 without ever marking the extremum, which reads as
 *        "the scale is unfinished" on a panel meter.  The search
 *        sweeps {1, 2, 5} × 10^n across the magnitude of the
 *        raw step ±1 decade and picks the divisible candidate
 *        whose interval count comes closest to @target_ticks - 1.
 *
 *  When no divisible candidate falls inside the sane interval-
 *  count window the helper returns the stage-1 fallback and
 *  paint_ticks's endpoint-anchor pass picks up the slack by
 *  painting an explicit major tick at @max_value regardless of
 *  whether the step iteration hit it. */
static double
pick_nice_step (double range, guint target_ticks)
{
    static const double nices[] = { 1.0, 2.0, 5.0 };
    double               raw, mag, norm;
    double               fallback;
    double               best_step  = 0.0;
    double               best_score = G_MAXDOUBLE;
    double               target_int;
    int                  mag_offset;
    int                  i;

    if (target_ticks < 2 || !(range > 0.0))
        return 0.0;

    raw  = range / (double) (target_ticks - 1);
    mag  = pow (10.0, floor (log10 (raw)));
    norm = raw / mag;

    if      (norm <= 1.0) fallback =  1.0 * mag;
    else if (norm <= 2.0) fallback =  2.0 * mag;
    else if (norm <= 5.0) fallback =  5.0 * mag;
    else                  fallback = 10.0 * mag;

    target_int = (double) target_ticks - 1.0;

    /* Allow up to 2 × target interval count when searching for a
     * cleanly-divisible step.  A meter looks like an instrument when
     * every major-tick gap along the arc is the same size; insisting
     * on the strict `major-ticks` upper bound would pin step = 2000
     * for a 0 - 5000 range with target 4 and leave a half-step stub
     * with denser minors between 4000 and the anchored 5000 -- "the
     * ticks are not uniform", exactly the artefact the user pushed
     * back on.  When even the relaxed budget has no nice divisor
     * (range = 240 etc.) the endpoint-anchor fallback in paint_ticks
     * paints an explicit major at @max_value so the extremum is
     * never unlabelled. */
    {
        double max_intervals = 2.0 * target_int;
        if (max_intervals < 6.0) max_intervals = 6.0;

        for (mag_offset = -1; mag_offset <= 1; mag_offset++)
        {
            double m = mag * pow (10.0, (double) mag_offset);

            for (i = 0; i < 3; i++)
            {
                double cand    = nices[i] * m;
                double n_inter;
                double score;

                if (!(cand > 0.0))
                    continue;

                n_inter = range / cand;
                if (n_inter < 2.0 || n_inter > max_intervals)
                    continue;

                /* Demand cleanly-divisible: otherwise the trailing
                 * stub re-appears.  The 1e-6 tolerance shrugs off
                 * the floating-point dust accumulated by the
                 * magnitude sweep (10^-1 × 1000 is not bit-exact
                 * with 100). */
                if (fabs (n_inter - round (n_inter)) > 1e-6)
                    continue;

                score = fabs (n_inter - target_int);

                /* Tie-breaker: prefer the LARGER step (fewer, more
                 * spread-out ticks) so a 0 - 20 A meter with target
                 * 4 lands on 0 / 10 / 20 instead of 0 / 5 / 10 / 15
                 * / 20. */
                if (score < best_score ||
                    (score == best_score && cand > best_step))
                {
                    best_score = score;
                    best_step  = cand;
                }
            }
        }
    }

    return best_step > 0.0 ? best_step : fallback;
}

static void
format_tick_label (gchar *buf, gsize sz, double val, double step)
{
    int decimals;

    if (fabs (val) < step * 1e-9)
        val = 0.0;

    if (step >= 1.0 && fabs (step - round (step)) < 1e-9)
    {
        g_snprintf (buf, sz, "%d", (int) round (val));
        return;
    }

    decimals = (int) ceil (-log10 (step));
    if (decimals < 1) decimals = 1;
    if (decimals > 6) decimals = 6;
    g_snprintf (buf, sz, "%.*f", decimals, val);
}

/** Paint the tick marks and digit labels.  The pivot is at the
 *  origin; ticks are placed on radii measured outward from the pivot.
 *  Major ticks are long and labelled, minor ticks are short and
 *  unlabelled, both painted in @scale_color (black by default on the
 *  classic panel meter look).  Major-tick value positions snap to a
 *  nice step ({1, 2, 5} × 10^n) derived from the user's `major-ticks`
 *  property, the same way #PnDial picks them.                         */
static void
paint_ticks (cairo_t *cr, PnAnalogMeter *self, double radius)
{
    PnAnalogMeterPrivate *priv    = pn_analog_meter_get_instance_private (self);
    double                r_outer      = radius * 1.00;  /* outer end of major ticks                       */
    double                r_minor      = radius * 0.965; /* inner end of minor ticks (30% shorter than 5%) */
    double                r_minor_long = radius * 0.94;  /* every-5th minor: 20% longer than original 5%   */
    double                r_major      = radius * 0.90;  /* inner end of major ticks                       */
    double                r_digit = radius * 0.82;  /* digit centre radius      */
    PangoLayout          *layout;
    PangoFontDescription *fd;
    gchar                 buf[32];
    double                step;
    double                first;
    double                v;
    double                prev_v;
    gboolean              have_prev;
    guint                 minors;
    guint                 j;

    if (priv->major_ticks < 2)
        return;
    if (!(priv->max_value > priv->min_value))
        return;

    step = pick_nice_step (priv->max_value - priv->min_value,
                           priv->major_ticks);
    if (!(step > 0.0))
        return;

    minors = priv->minor_ticks_per_major;
    first  = ceil (priv->min_value / step - 1e-9) * step;

    cairo_set_source_rgba (cr,
                           priv->scale_color.red,
                           priv->scale_color.green,
                           priv->scale_color.blue,
                           priv->scale_color.alpha);

    /* Scale arc: a thin guide line that joins every tick at the outer
     * radius -- the look is the moulded "track" the moving-coil arm
     * pretends to ride along, which every old analog meter prints to
     * sell the reading as continuous between the labelled major
     * ticks. */
    cairo_set_line_width (cr, 1.2);
    cairo_new_sub_path (cr);
    cairo_arc (cr, 0.0, 0.0, r_outer,
               clock_to_cairo (priv->start_angle),
               clock_to_cairo (priv->end_angle));
    cairo_stroke (cr);

    /* Minor ticks: between each pair of consecutive major-tick value
     * positions (including the implicit boundary at min_value / first
     * and at last / max_value).  Drawn first so the bolder major
     * ticks overpaint any antialias seam. */
    cairo_set_line_width (cr, 0.8);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
    have_prev = FALSE;
    prev_v    = priv->min_value;
    for (v = first; v <= priv->max_value + step * 1e-9; v += step)
    {
        double seg_lo = have_prev ? prev_v : priv->min_value;
        double seg_hi = v;

        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;
            double r_in;

            if (mv <= priv->min_value || mv >= priv->max_value)
                continue;
            ang  = clock_to_cairo (value_to_angle (self, mv));
            r_in = (j % 5 == 0) ? r_minor_long : r_minor;
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_in,    sin (ang) * r_in);
        }
        prev_v    = v;
        have_prev = TRUE;
    }
    if (have_prev && prev_v < priv->max_value)
    {
        double seg_lo = prev_v;
        double seg_hi = priv->max_value;
        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;
            double r_in;

            if (mv <= priv->min_value || mv >= priv->max_value)
                continue;
            ang  = clock_to_cairo (value_to_angle (self, mv));
            r_in = (j % 5 == 0) ? r_minor_long : r_minor;
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_in,    sin (ang) * r_in);
        }
    }
    cairo_stroke (cr);

    /* Major ticks.  The endpoint anchors ensure both min_value and
     * max_value get a major tick even when the step doesn't divide
     * the range cleanly -- without them the scale ends on a half-
     * step stub of unlabelled minors instead of a defined extremum
     * (the "the 5000 isn't drawn" case the user sees on a 0-5000
     * range with the heuristic's 2000 step). */
    cairo_set_line_width (cr, 1.8);
    {
        double last_v   = priv->min_value;
        gboolean had_min = FALSE;
        gboolean had_max = FALSE;

        for (v = first; v <= priv->max_value + step * 1e-9; v += step)
        {
            double ang = clock_to_cairo (value_to_angle (self, v));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_major, sin (ang) * r_major);
            if (fabs (v - priv->min_value) < step * 1e-6) had_min = TRUE;
            if (fabs (v - priv->max_value) < step * 1e-6) had_max = TRUE;
            last_v = v;
        }
        (void) last_v;

        if (!had_min)
        {
            double ang = clock_to_cairo (value_to_angle (self, priv->min_value));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_major, sin (ang) * r_major);
        }
        if (!had_max)
        {
            double ang = clock_to_cairo (value_to_angle (self, priv->max_value));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_major, sin (ang) * r_major);
        }
    }
    cairo_stroke (cr);

    /* Digit labels.  Pango layout reused across ticks the same way
     * #PnDial does, to keep the per-paint allocation count at one.
     * Labels mirror the major-tick endpoint anchors so a meter
     * whose max isn't a step multiple ends with a clearly labelled
     * extremum instead of a labelless arc terminus. */
    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string ("Sans 11");
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    {
        gboolean had_min = FALSE;
        gboolean had_max = FALSE;

        for (v = first; v <= priv->max_value + step * 1e-9; v += step)
        {
            double ang = clock_to_cairo (value_to_angle (self, v));
            double cx  = cos (ang) * r_digit;
            double cy  = sin (ang) * r_digit;
            int    pw, ph;

            format_tick_label (buf, sizeof (buf), v, step);
            pango_layout_set_text (layout, buf, -1);
            pango_layout_get_pixel_size (layout, &pw, &ph);

            cairo_save (cr);
            cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
            pango_cairo_show_layout (cr, layout);
            cairo_restore (cr);

            if (fabs (v - priv->min_value) < step * 1e-6) had_min = TRUE;
            if (fabs (v - priv->max_value) < step * 1e-6) had_max = TRUE;
        }

        if (!had_min)
        {
            double ang = clock_to_cairo (value_to_angle (self, priv->min_value));
            double cx  = cos (ang) * r_digit;
            double cy  = sin (ang) * r_digit;
            int    pw, ph;

            format_tick_label (buf, sizeof (buf), priv->min_value, step);
            pango_layout_set_text (layout, buf, -1);
            pango_layout_get_pixel_size (layout, &pw, &ph);
            cairo_save (cr);
            cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
            pango_cairo_show_layout (cr, layout);
            cairo_restore (cr);
        }
        if (!had_max)
        {
            double ang = clock_to_cairo (value_to_angle (self, priv->max_value));
            double cx  = cos (ang) * r_digit;
            double cy  = sin (ang) * r_digit;
            int    pw, ph;

            format_tick_label (buf, sizeof (buf), priv->max_value, step);
            pango_layout_set_text (layout, buf, -1);
            pango_layout_get_pixel_size (layout, &pw, &ph);
            cairo_save (cr);
            cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
            pango_cairo_show_layout (cr, layout);
            cairo_restore (cr);
        }
    }

    g_object_unref (layout);
}

/** Paint the needle: a flat black silhouette pivoted at the origin
 *  with a short stubby counterweight on the opposite side.  The shaft
 *  runs at a constant wide half-width for the first 70% of its length
 *  from the pivot, tapers from wide to thin between 70% and 90%, then
 *  runs at a constant thin half-width for the last 10% out to the tip
 *  -- the silhouette of a moving-coil pointer with a fine, deliberately
 *  parallel-sided needle end (the bit you actually align with a scale
 *  mark) rather than a single linear taper that finishes at a point.
 *  No 3D shading -- every real panel meter's needle is a flat opaque
 *  silhouette.  Hidden until the first message arrives so the parked-
 *  needle state is visually distinguishable from a real reading of
 *  @min_value (same convention as #PnDial). */
static void
paint_needle (cairo_t *cr, PnAnalogMeter *self, double radius)
{
    PnAnalogMeterPrivate *priv = pn_analog_meter_get_instance_private (self);
    double                angle;

    if (!priv->has_value)
        return;

    angle = clock_to_cairo (value_to_angle (self, priv->display_value));
    {
        double tip_r  = radius * 0.95;
        double back_r = radius * 0.08;
        double wide_w = radius * 0.020;   /* half-width of the constant-wide shaft (0% - 70% of length)  */
        double thin_w = radius * 0.008;   /* half-width of the constant-thin tip   (90% - 100% of length) */
        double L70    = tip_r * 0.70;     /* end of constant-wide shaft, start of taper                   */
        double L90    = tip_r * 0.90;     /* end of taper, start of constant-thin tip                     */
        double ux     = cos (angle);
        double uy     = sin (angle);
        double px     = -uy;              /* perpendicular to the needle direction */
        double py     =  ux;
        double bx     = -ux * back_r;
        double by     = -uy * back_r;

        double r0x = px * wide_w,                r0y = py * wide_w;
        double r1x = ux * L70   + px * wide_w,   r1y = uy * L70   + py * wide_w;
        double r2x = ux * L90   + px * thin_w,   r2y = uy * L90   + py * thin_w;
        double r3x = ux * tip_r + px * thin_w,   r3y = uy * tip_r + py * thin_w;
        double l3x = ux * tip_r - px * thin_w,   l3y = uy * tip_r - py * thin_w;
        double l2x = ux * L90   - px * thin_w,   l2y = uy * L90   - py * thin_w;
        double l1x = ux * L70   - px * wide_w,   l1y = uy * L70   - py * wide_w;
        double l0x = -px * wide_w,               l0y = -py * wide_w;

        /* Optional subtle shadow -- a 1 px offset translucent black
         * copy of the needle.  Sells the "needle floats above the
         * face" read at zoom levels where the antialiased edge alone
         * would not be enough to lift the needle off the white face. */
        cairo_save (cr);
        cairo_translate (cr, 1.0, 1.0);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.30);
        cairo_move_to (cr, r0x, r0y);
        cairo_line_to (cr, r1x, r1y);
        cairo_line_to (cr, r2x, r2y);
        cairo_line_to (cr, r3x, r3y);
        cairo_line_to (cr, l3x, l3y);
        cairo_line_to (cr, l2x, l2y);
        cairo_line_to (cr, l1x, l1y);
        cairo_line_to (cr, l0x, l0y);
        cairo_line_to (cr, bx,  by);
        cairo_close_path (cr);
        cairo_fill (cr);
        cairo_restore (cr);

        cairo_set_source_rgba (cr,
                               priv->needle_color.red,
                               priv->needle_color.green,
                               priv->needle_color.blue,
                               priv->needle_color.alpha);
        cairo_move_to (cr, r0x, r0y);
        cairo_line_to (cr, r1x, r1y);
        cairo_line_to (cr, r2x, r2y);
        cairo_line_to (cr, r3x, r3y);
        cairo_line_to (cr, l3x, l3y);
        cairo_line_to (cr, l2x, l2y);
        cairo_line_to (cr, l1x, l1y);
        cairo_line_to (cr, l0x, l0y);
        cairo_line_to (cr, bx,  by);
        cairo_close_path (cr);
        cairo_fill (cr);
    }
}

/** Paint a horizontally-centred Pango text run at (0, @y_center) in
 *  the supplied @font_desc and @color.  When @max_width is positive
 *  the painter measures the text at the requested font size first
 *  and, if it would overflow, scales the size down so the rendered
 *  run is at most @max_width pixels wide -- the badge area on the
 *  meter's face is sized for a single-letter unit ("V", "A") and a
 *  longer string ("cos φ", "mbar") would otherwise spill across the
 *  scale arc.  Pass 0.0 to disable shrinking and render at the
 *  literal @font_desc size.  No-op when @text is empty. */
static void
paint_centered_text (cairo_t       *cr,
                     const gchar   *text,
                     const gchar   *font_desc,
                     const GdkRGBA *color,
                     double         y_center,
                     double         max_width)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    int                   pw, ph;

    if (text == NULL || *text == '\0')
        return;

    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string (font_desc);
    pango_layout_set_font_description (layout, fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    /* Shrink-to-fit: rescale the font size in proportion to the
     * pixel overshoot.  Pango font sizes are integer Pango units
     * (1/PANGO_SCALE of a point) so the round-trip via
     * pango_font_description_set_size keeps the result rasterisable.
     * Clamp the floor at 6 pt so a degenerate max_width does not
     * collapse the label to unreadable. */
    if (max_width > 0.0 && pw > 0 && (double) pw > max_width)
    {
        gint old_size = pango_font_description_get_size (fd);
        gint new_size = (gint) ((double) old_size * max_width / (double) pw);
        gint floor_sz = 6 * PANGO_SCALE;

        if (new_size < floor_sz)
            new_size = floor_sz;

        pango_font_description_set_size (fd, new_size);
        pango_layout_set_font_description (layout, fd);
        pango_layout_get_pixel_size (layout, &pw, &ph);
    }
    pango_font_description_free (fd);

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_move_to (cr, -pw * 0.5, y_center - ph * 0.5);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

/** Paint a small sine-wave glyph centred on (0, @y_center).  Drawn as
 *  one cycle of sin() sampled into a polyline, then stroked.  The
 *  glyph reads as the IEC "~" symbol for an AC instrument -- a single
 *  wave rather than the two-cycle longer variant, matching the look
 *  on a typical analog table meter. */
static void
paint_ac_glyph (cairo_t       *cr,
                const GdkRGBA *color,
                double         y_center)
{
    double w = 22.0;          /* total glyph width */
    double h = 4.0;           /* peak-to-peak amplitude / 2 */
    int    samples = 24;
    int    i;

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_set_line_width (cr, 1.5);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    cairo_new_sub_path (cr);
    for (i = 0; i <= samples; i++)
    {
        double t = (double) i / (double) samples;
        double x = -w * 0.5 + t * w;
        double y = y_center - sin (t * 2.0 * M_PI) * h;
        if (i == 0)
            cairo_move_to (cr, x, y);
        else
            cairo_line_to (cr, x, y);
    }
    cairo_stroke (cr);
}

/** Paint the IEC DC symbol centred on (0, @y_center): a solid line
 *  above a row of three short dashes (IEC 60417-5031, the universal
 *  "direct current" symbol on every multimeter face).  Mirrors
 *  paint_ac_glyph() in stroke width and overall width so a meter
 *  toggling between AC and DC mode keeps the same visual weight in
 *  the upper-left badge area.                                         */
static void
paint_dc_glyph (cairo_t       *cr,
                const GdkRGBA *color,
                double         y_center)
{
    double w   = 22.0;
    double sep = 4.0;            /* vertical gap between the two rows */

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_set_line_width (cr, 1.5);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    /* Solid line on top. */
    cairo_move_to (cr, -w * 0.5, y_center - sep * 0.5);
    cairo_line_to (cr, +w * 0.5, y_center - sep * 0.5);
    cairo_stroke (cr);

    /* Three short dashes below.  Each dash occupies 1/5 of the total
     * glyph width; the remaining 2/5 is split into two inter-dash
     * gaps so the dashes read as a rhythm rather than as a single
     * unbroken bar. */
    {
        double dash  = w * 0.2;
        double pitch = w / 3.0;
        double y     = y_center + sep * 0.5;
        int    i;
        for (i = 0; i < 3; i++)
        {
            double cx = -w * 0.5 + ((double) i + 0.5) * pitch;
            cairo_move_to (cr, cx - dash * 0.5, y);
            cairo_line_to (cr, cx + dash * 0.5, y);
        }
        cairo_stroke (cr);
    }
}

/** Paint the needle's pivot bearing at face-local (cx, cy): a small
 *  dark disc with a thin recessed shadow ring and a single light
 *  diagonal slot across the head.  The disc sits ON TOP of the
 *  needle's counterweight stub so the needle visually emerges from
 *  the bearing rather than passing through it; the slot reads as the
 *  factory-set mark on the moulded plastic moving-coil shell.  Caller
 *  passes the bearing centre in face-local coords (NOT pivot-local)
 *  so the same routine can serve every choice of pivot position the
 *  user might pick via start-angle / end-angle.                       */
static void
paint_pivot_bearing (cairo_t *cr, double cx, double cy)
{
    /* Bearing diameter ≈ 26 px on the canonical 220-px face.  About
     * 1.6× the size of a typical zero-adjust screw -- chunky enough
     * to read as the moving-coil's mechanical pivot at the worksheet
     * zoom levels the editor actually renders at, but small enough
     * that it does not crowd the lower-right-corner ticks.            */
    double           r = 13.0;
    cairo_pattern_t *grad;

    /* Recessed ring around the bearing.  Offset scales with the
     * bearing radius so the shadow gap stays visible at the new
     * doubled diameter. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.40);
    cairo_arc (cr, cx + 1.5, cy + 1.5, r + 1.0, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Head body: dark radial gradient. */
    grad = cairo_pattern_create_radial (cx - r * 0.3, cy - r * 0.3, r * 0.1,
                                        cx,            cy,           r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.40, 0.40, 0.42);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.10, 0.10, 0.12);
    cairo_set_source (cr, grad);
    cairo_arc (cr, cx, cy, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Single light slot across the head, tilted ~30° so the bearing
     * has a recognisable orientation cue (matches the look of a
     * factory-set mechanical pivot rather than a perfectly symmetric
     * machined disc).                                                  */
    cairo_save (cr);
    cairo_translate (cr, cx, cy);
    cairo_rotate (cr, M_PI / 6.0);
    cairo_set_source_rgba (cr, 0.85, 0.85, 0.87, 0.95);
    cairo_set_line_width (cr, 2.0);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
    cairo_move_to (cr, -r * 0.78, 0.0);
    cairo_line_to (cr, +r * 0.78, 0.0);
    cairo_stroke (cr);
    cairo_restore (cr);
}

/** Paint the accuracy-class string at the lower-left corner of the
 *  face.  The text is left-aligned, NOT centred, so the user-supplied
 *  string (typically "2.5", "1.5", "0.5") lands snug against the
 *  inner bezel.  Coordinates are face-local (NOT pivot-local). */
static void
paint_accuracy_class (cairo_t       *cr,
                      const gchar   *text,
                      const GdkRGBA *color,
                      double         face_h)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    int                   pw, ph;

    if (text == NULL || *text == '\0')
        return;

    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string ("Sans 9");
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_move_to (cr, 16.0, face_h - 22.0 - ph * 0.5);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

static void
pn_analog_meter_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnAnalogMeter        *self    = PN_ANALOG_METER (node);
    PnAnalogMeterPrivate *priv    = pn_analog_meter_get_instance_private (self);
    double                face_w  = w;
    double                face_h  = h;
    /* Pivot at the lower-right corner of the face.  The default scale
     * runs lower-left (start-angle = -90, the needle pointing
     * horizontally left) up through the top to upper-right (end-angle
     * = 0, the needle pointing straight up), so the arc fills the
     * upper-left triangle of the face and the empty wedge below the
     * chord is the bottom band where the accuracy-class string lives.
     * The bearing disc painted by paint_pivot_bearing() at (pivot_x,
     * pivot_y) IS the moving-coil pivot the needle swings around --
     * there is no separate centre-of-face dome on top of the needle
     * the way #PnDial has, because the bearing already sits where the
     * needle's stub would otherwise be.                                */
    double         pivot_x = face_w - 30.0;
    double         pivot_y = face_h - 30.0;

    /* Radius is bounded by the worst-case position of any tick at the
     * outer tick radius (== 1.0 * radius) over the @start_angle ..
     * @end_angle sweep.  We compute the extreme values of sin/cos
     * over the swept range -- including the interior extrema at
     * ±90°/0°/180° when they happen to fall inside the range -- and
     * derive the largest radius that keeps every tick at least 16 px
     * clear of the bezel, with extra headroom above for the unit /
     * AC glyphs in the upper-left corner of the face.                  */
    double         lo      = fmin (priv->start_angle, priv->end_angle);
    double         hi      = fmax (priv->start_angle, priv->end_angle);
    double         max_sin = fmax (sin (lo * M_PI / 180.0),
                                   sin (hi * M_PI / 180.0));
    double         min_sin = fmin (sin (lo * M_PI / 180.0),
                                   sin (hi * M_PI / 180.0));
    double         max_cos = fmax (cos (lo * M_PI / 180.0),
                                   cos (hi * M_PI / 180.0));
    double         min_cos = fmin (cos (lo * M_PI / 180.0),
                                   cos (hi * M_PI / 180.0));
    {
        double k;
        for (k = -270.0; k <= 360.0; k += 90.0)
        {
            double s, c;
            if (k < lo || k > hi) continue;
            s = sin (k * M_PI / 180.0);
            c = cos (k * M_PI / 180.0);
            if (s > max_sin) max_sin = s;
            if (s < min_sin) min_sin = s;
            if (c > max_cos) max_cos = c;
            if (c < min_cos) min_cos = c;
        }
    }
    double         margin = 16.0;
    double         top_pad = 14.0;
    double         radius  = 1e9;
    if (max_sin >  0.01) radius = fmin (radius, (face_w - margin - pivot_x) / max_sin);
    if (min_sin < -0.01) radius = fmin (radius, (pivot_x - margin) / (-min_sin));
    if (max_cos >  0.01) radius = fmin (radius, (pivot_y - top_pad) / max_cos);
    if (min_cos < -0.01) radius = fmin (radius, (face_h - margin - pivot_y) / (-min_cos));
    if (radius < 40.0)  radius = 40.0;

    cairo_save (cr);
    cairo_translate (cr, x, y);

    /* Plastic case + inset face. */
    cairo_save (cr);
    cairo_translate (cr, face_w * 0.5, face_h * 0.5);
    paint_case  (cr, face_w, face_h, &priv->frame_color);
    paint_inner (cr, face_w, face_h, &priv->face_color);
    cairo_restore (cr);

    /* Unit + AC/DC glyph stacked in the UPPER-LEFT corner of the
     * face, opposite the pivot.  For the diagonal lower-left →
     * upper-right scale the upper-left is the only generously-sized
     * empty area on the face.  The unit symbol sits on top; the
     * current-type glyph (AC sine-wave when @ac_mode is TRUE, the
     * DC solid-over-dashed mark when FALSE) sits directly below it.
     * Translation is at face_w * 0.17 so the badge hugs the left
     * bezel without crowding it. */
    {
        cairo_save (cr);
        cairo_translate (cr, face_w * 0.17, 0.0);
        /* Cap the unit text at face_w * 0.22 -- the badge sits at
         * face_w * 0.17, which is only ~29 px from the face's inner
         * bezel (the 8-px inset on each side leaves the inset face
         * spanning x=8..212).  Half of 0.22*face_w is ~24 px, so the
         * left edge of the rendered run lands at x=13 -- safely
         * inside the inset face and short of the upper-left corner
         * of the scale arc -- regardless of how chunky the label is.
         * Single-letter units ("V", "A") are well under the cap so
         * they render at the literal 22 pt; longer labels ("cos φ",
         * "mbar") trip paint_centered_text's shrink-to-fit fallback,
         * which scales the font down until the run lands inside the
         * badge area. */
        paint_centered_text (cr,
                             priv->unit, "Sans Bold 22",
                             &priv->label_color,
                             30.0,
                             face_w * 0.22);
        switch (priv->mode)
        {
        case PN_ANALOG_METER_MODE_AC:
            paint_ac_glyph (cr, &priv->label_color, 56.0);
            break;
        case PN_ANALOG_METER_MODE_DC:
            paint_dc_glyph (cr, &priv->label_color, 56.0);
            break;
        case PN_ANALOG_METER_MODE_NONE:
        default:
            /* No current-type glyph -- the unit alone identifies the
             * quantity, which is the right answer for meters whose
             * value has no AC/DC sense (frequency, temperature). */
            break;
        }
        cairo_restore (cr);
    }
    paint_accuracy_class (cr, priv->accuracy_class,
                          &priv->label_color, face_h);

    /* Scale + needle, in pivot-local coords.  The pivot bearing disc
     * is painted AFTER the needle (in face-local coords) so it sits
     * on top of the needle's counterweight stub and reads as the
     * mechanical pivot the needle swings out from.                    */
    cairo_save (cr);
    cairo_translate (cr, pivot_x, pivot_y);
    paint_ticks  (cr, self, radius);
    paint_needle (cr, self, radius);
    cairo_restore (cr);

    paint_pivot_bearing (cr, pivot_x, pivot_y);

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog                                                    */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_am_page (GObject *target, const gchar *const *names)
{
    GtkWidget    *grid  = pn_node_dialog_new_property_grid ();
    GObjectClass *klass = G_OBJECT_GET_CLASS (target);
    guint         row   = 0;
    guint         i;

    for (i = 0; names[i] != NULL; i++)
    {
        GParamSpec  *pspec  = g_object_class_find_property (klass, names[i]);
        GtkWidget   *editor;
        const gchar *nick;

        if (pspec == NULL || (pspec->flags & G_PARAM_WRITABLE) == 0)
            continue;

        editor = pn_node_dialog_default_editor (target, pspec);
        nick   = g_param_spec_get_nick (pspec);
        pn_node_dialog_attach_row (GTK_GRID (grid), row,
                                   nick != NULL ? nick : names[i],
                                   editor);
        row += 1;
    }

    return grid;
}

static void
pn_analog_meter_build_class_tabs (
        PnNode      *self,
        GtkNotebook *notebook,
        GtkWindow   *parent G_GNUC_UNUSED)
{
    GObject *target = G_OBJECT (self);

    static const gchar *const data_props[] = {
        "key", "unit", "mode", "accuracy-class", NULL
    };
    static const gchar *const scale_props[] = {
        "min-value", "max-value",
        "start-angle", "end-angle",
        "major-ticks", "minor-ticks-per-major",
        NULL
    };
    static const gchar *const color_props[] = {
        "frame-color", "face-color",
        "scale-color", "needle-color",
        "label-color",
        NULL
    };

    pn_node_dialog_append_page (notebook,
                                build_am_page (target, data_props),
                                "Data");
    pn_node_dialog_append_page (notebook,
                                build_am_page (target, scale_props),
                                "Scale");
    pn_node_dialog_append_page (notebook,
                                build_am_page (target, color_props),
                                "Colours");
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
set_color_prop (PnAnalogMeter *self, GdkRGBA *slot,
                const GdkRGBA *new_value, guint prop_id)
{
    if (new_value == NULL || gdk_rgba_equal (slot, new_value))
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

    node_class->receive                = pn_analog_meter_receive;
    node_class->get_size               = pn_analog_meter_get_size;
    node_class->get_header_height      = pn_analog_meter_get_header_height;
    node_class->paint_plot             = pn_analog_meter_paint_plot;
    node_class->build_class_tabs       = pn_analog_meter_build_class_tabs;
    /* The case has rounded corners -- the standard rectangular drop
     * shadow leaks past them at the corners and reads as a slightly
     * mis-aligned ghost around the otherwise-flush plastic body.    */
    node_class->paint_plot_skip_shadow = TRUE;
    /* The 220-px face is already the readable instrument: a press
     * should select / drag the meter rather than lift an enlarged
     * copy onto the centred zoom overlay the way it does for graph
     * / table plots. */
    node_class->paint_plot_skip_zoom   = TRUE;

    node_class->class_name        = "AnalogMeter";
    node_class->icon              = "\xef\x83\xa4";       /* fa-tachometer U+F0E4 */
    /* Same yellow as #PnDial.  Both nodes are gauge-style sinks that
     * read a single numeric value off the message, so palette-grouping
     * them by colour helps the eye spot a worksheet's metering row at
     * a glance. */
    node_class->color             = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
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
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_FACE_COLOR] = g_param_spec_boxed (
            "face-color", "Face colour",
            "Base fill colour of the inset dial face",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_SCALE_COLOR] = g_param_spec_boxed (
            "scale-color", "Scale colour",
            "Colour of the tick marks, the scale arc and the digit labels",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_NEEDLE_COLOR] = g_param_spec_boxed (
            "needle-color", "Needle colour",
            "Fill colour of the needle body",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_LABEL_COLOR] = g_param_spec_boxed (
            "label-color", "Label colour",
            "Colour of the unit symbol, the AC \"~\" glyph and the "
            "accuracy-class string",
            GDK_TYPE_RGBA,
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

    priv->frame_color  = (GdkRGBA){ 0.05, 0.05, 0.05, 1.0 };
    priv->face_color   = (GdkRGBA){ 0.97, 0.97, 0.95, 1.0 };
    priv->scale_color  = (GdkRGBA){ 0.05, 0.05, 0.05, 1.0 };
    priv->needle_color = (GdkRGBA){ 0.05, 0.05, 0.05, 1.0 };
    priv->label_color  = (GdkRGBA){ 0.05, 0.05, 0.05, 1.0 };

    priv->target_value     = priv->min_value;
    priv->display_value    = priv->min_value;
    priv->display_velocity = 0.0;
    priv->has_value        = FALSE;

    priv->anim_id      = 0;
    priv->anim_last_us = 0;

    priv->last_repaint_us    = 0;
    priv->pending_repaint_id = 0;

    {
        GdkRGBA yellow = { 0.92, 0.76, 0.27, 1.0 };
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
