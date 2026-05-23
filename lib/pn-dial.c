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

#include "pn-dial.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-node-dialog-helpers.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Square footprint: a 220-px-wide header + 220-px-tall dial face.    */
/*  The header keeps the canonical 40 px height (so ports line up      */
/*  with the rest of the worksheet) and the dial sits in the square    */
/*  region directly below it.  Choosing equal width and face-height    */
/*  makes the dial face a true circle regardless of the device pixel   */
/*  density the worksheet is rendering into.                           */
/* ------------------------------------------------------------------ */

#define PN_DIAL_WIDTH         220.0
#define PN_DIAL_HEADER_HEIGHT  40.0
#define PN_DIAL_GAP             4.0
#define PN_DIAL_FACE_SIZE     220.0
#define PN_DIAL_TOTAL_HEIGHT  (PN_DIAL_HEADER_HEIGHT + \
                               PN_DIAL_GAP +           \
                               PN_DIAL_FACE_SIZE)

/* Repaint throttle: cap incoming-message-driven repaints at 30 Hz so
 * a high-rate feed cannot saturate the worksheet's redraw loop. */
#define PN_DIAL_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 33)

/* Damped-spring animation tuning for the needle.  k is the spring     */
/* stiffness, c the damping coefficient -- with implicit mass = 1 the  */
/* natural frequency is sqrt(k) and the damping ratio is c/(2*sqrt(k)).*/
/* k = 90, c = 11 gives omega_n ≈ 9.5 rad/s (settling time ~0.6 s for  */
/* a typical step) and zeta ≈ 0.58 (clearly underdamped -- the needle  */
/* visibly overshoots and oscillates once or twice before settling,    */
/* which is the read of a real moving-coil meter).                     */
#define PN_DIAL_SPRING_K      90.0
#define PN_DIAL_SPRING_C      11.0

/* Animation tick period.  16 ms ≈ 60 Hz, fine for a small Cairo       */
/* shape; the timer self-cancels when the needle reaches rest so the   */
/* idle cost is zero between value changes.                            */
#define PN_DIAL_ANIM_TICK_MS  16

/* ------------------------------------------------------------------ */
/*  PnDial instance                                                    */
/* ------------------------------------------------------------------ */

struct _PnDial
{
    PnNode parent_instance;

    /* Data binding. */
    gchar    *key;             /* JSON path to the value the needle reads */

    /* Scale geometry.  Angles are degrees measured clockwise from
     * 12 o'clock; -120 → 7 o'clock, +120 → 5 o'clock, so the default
     * sweep covers the comfortable ~240° lower-and-upper arc that
     * pressure gauges and speedometers traditionally use. */
    gdouble   min_value;
    gdouble   max_value;
    gdouble   start_angle;
    gdouble   end_angle;
    guint     major_ticks;
    guint     minor_ticks_per_major;

    /* Bottom-of-face labels.  Both sit in the empty wedge below the
     * centre pivot, between the start-angle and end-angle tick
     * labels.  @label is the descriptive line ("speed", "tank A");
     * @unit is the short suffix ("km/h", "L") rendered below it in
     * an even bigger font so it reads as the dominant identifier
     * for the value on a single-glance read. */
    gchar    *label;
    gchar    *unit;

    /* Three optional coloured arcs drawn just inside the tick band.
     * Each zone occupies the value range [start, end]; when start ≥ end
     * the zone is treated as empty and is not drawn.  This lets a
     * user wire up only one or two of the three zones without having
     * to invent placeholder values. */
    gdouble   green_start, green_end;
    gdouble   yellow_start, yellow_end;
    gdouble   red_start, red_end;

    /* Painted colours -- the metallic bezel and centre pivot are NOT
     * configurable; only the colours that the user would actually
     * read off the dial. */
    GdkRGBA   face_color;
    GdkRGBA   scale_color;
    GdkRGBA   needle_color;
    GdkRGBA   label_color;
    GdkRGBA   green_color;
    GdkRGBA   yellow_color;
    GdkRGBA   red_color;

    /* Live state.  @target_value is the most-recent value seen on the
     * input -- where the needle is heading -- and @display_value is
     * where the needle actually is right now, lagged through a damped
     * spring so the needle reads as a moving-coil meter rather than a
     * teleporting indicator.  @display_velocity carries the spring's
     * angular speed across animation ticks.  @has_value flags "we
     * have never received anything yet" so the needle parks at the
     * start-angle rather than drifting from whatever happened to be
     * in the field at construction. */
    gdouble   target_value;
    gdouble   display_value;
    gdouble   display_velocity;
    gboolean  has_value;

    /* g_timeout source running the animation integrator at ~60 Hz.
     * 0 when the needle is at rest; the tick callback self-cancels
     * (returns G_SOURCE_REMOVE) when it detects the spring has come
     * to rest, so the node draws no CPU between value changes. */
    guint     anim_id;
    gint64    anim_last_us;

    /* Throttle bookkeeping, identical pattern to pn-graph. */
    gint64    last_repaint_us;
    guint     pending_repaint_id;
};

G_DEFINE_TYPE (PnDial, pn_dial, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_KEY,
    PROP_MIN_VALUE,
    PROP_MAX_VALUE,
    PROP_START_ANGLE,
    PROP_END_ANGLE,
    PROP_MAJOR_TICKS,
    PROP_MINOR_TICKS_PER_MAJOR,
    PROP_LABEL,
    PROP_UNIT,
    PROP_GREEN_START,
    PROP_GREEN_END,
    PROP_YELLOW_START,
    PROP_YELLOW_END,
    PROP_RED_START,
    PROP_RED_END,
    PROP_FACE_COLOR,
    PROP_SCALE_COLOR,
    PROP_NEEDLE_COLOR,
    PROP_LABEL_COLOR,
    PROP_GREEN_COLOR,
    PROP_YELLOW_COLOR,
    PROP_RED_COLOR,
    PROP_VALUE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Convert a clock-angle (degrees, clockwise from 12 o'clock) into a
 *  cairo angle (radians, clockwise from +x axis under cairo's default
 *  y-down coordinates).  At clock_angle = 0 the result is -π/2 which
 *  is the top of the dial; the +x axis is the 3 o'clock direction so
 *  a clock_angle of 90° maps to cairo 0 as expected. */
static inline double
clock_to_cairo (double clock_deg)
{
    return (clock_deg - 90.0) * M_PI / 180.0;
}

/** Map a data value to its clock-angle along the scale.  Values are
 *  clamped to [min_value, max_value] so an out-of-range reading parks
 *  the needle at the corresponding endpoint instead of swinging past
 *  the tick marks (which would look broken). */
static double
value_to_angle (PnDial *self, double v)
{
    double span = self->max_value - self->min_value;
    double t;

    if (span <= 0.0)
        return self->start_angle;

    if (v < self->min_value) v = self->min_value;
    if (v > self->max_value) v = self->max_value;

    t = (v - self->min_value) / span;
    return self->start_angle + t * (self->end_angle - self->start_angle);
}

/** Parse a numeric string into a double.  Accepts decimal and "0x..."
 *  hex literals so JSON-RPC quantities arriving as strings still
 *  drive the needle.  Mirrors the helper in pn-graph.c. */
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

/** Coerce a JsonNode to a finite double; mirrors pn-graph.c. */
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
    PnDial *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnDial *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_DIAL_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_DIAL_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);
        if (delay_ms == 0) delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Damped-spring needle animation                                     */
/*                                                                     */
/*  Treats display_value as a unit-mass particle pulled toward         */
/*  target_value by a spring (stiffness k) and slowed by a damper      */
/*  (coefficient c).  Each tick advances the integrator by the         */
/*  measured wall-clock delta and asks the worksheet to repaint; the   */
/*  tick self-cancels once both the displacement and the velocity      */
/*  fall under a fraction-of-scale epsilon, leaving the needle at      */
/*  exactly target_value with zero velocity.                           */
/* ------------------------------------------------------------------ */

static gboolean
on_anim_tick (gpointer user_data)
{
    PnDial *self  = user_data;
    gint64  now   = g_get_monotonic_time ();
    gint64  dt_us = now - self->anim_last_us;
    gdouble dt;
    gdouble displ;
    gdouble accel;
    gdouble scale;
    gdouble eps_pos;
    gdouble eps_vel;

    if (dt_us <= 0)
        dt_us = PN_DIAL_ANIM_TICK_MS * (gint64) 1000;
    self->anim_last_us = now;

    /* Clamp dt so a long pause (window minimised, machine swapping)
     * cannot blow up the integrator into a giant single step.        */
    dt = (gdouble) dt_us / (gdouble) G_TIME_SPAN_SECOND;
    if (dt > 0.05) dt = 0.05;

    displ = self->target_value - self->display_value;
    accel = PN_DIAL_SPRING_K * displ
          - PN_DIAL_SPRING_C * self->display_velocity;

    self->display_velocity += accel * dt;
    self->display_value    += self->display_velocity * dt;

    /* "At rest" test: both displacement and velocity below 0.05 % of
     * the value scale.  Below that nothing the needle does is visible
     * on the canonical 220-px face, so we snap to target and stop the
     * timer to avoid a permanent idle-cost on every dial.            */
    scale   = self->max_value - self->min_value;
    if (scale <= 0.0) scale = 1.0;
    eps_pos = fabs (scale) * 0.0005;
    eps_vel = fabs (scale) * 0.005;

    if (fabs (displ) < eps_pos &&
        fabs (self->display_velocity) < eps_vel)
    {
        self->display_value    = self->target_value;
        self->display_velocity = 0.0;
        self->anim_id          = 0;
        pn_node_request_repaint (PN_NODE (self));
        return G_SOURCE_REMOVE;
    }

    pn_node_request_repaint (PN_NODE (self));
    return G_SOURCE_CONTINUE;
}

static void
start_anim (PnDial *self)
{
    if (self->anim_id != 0)
        return;
    self->anim_last_us = g_get_monotonic_time ();
    self->anim_id      = g_timeout_add (PN_DIAL_ANIM_TICK_MS,
                                        on_anim_tick, self);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_dial_receive (PnNode *node, PnMessage *message)
{
    PnDial     *self = PN_DIAL (node);
    JsonObject *root;
    JsonNode   *value_node;
    gdouble     value;

    if (self->key == NULL || *self->key == '\0')
        return;

    root       = pn_json_lookup_root_for_message (message);
    value_node = pn_json_resolve_path (root, self->key);

    if (!node_to_finite_double (value_node, &value))
    {
        json_object_unref (root);
        return;
    }
    json_object_unref (root);

    /* Mark "we have a real value now" but keep display_value parked
     * at min_value -- the spring will animate from there on the very
     * first message, the same way every subsequent message animates
     * from wherever the needle happens to be sitting. */
    self->has_value = TRUE;

    if (self->target_value == value)
        return;

    self->target_value = value;
    /* Surface the freshly-received value on the read-only "value"
     * property so inspectors and functional tests can observe what the
     * dial is pointing at without reaching into its private state. */
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALUE]);
    start_anim (self);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/*                                                                     */
/*  Every painter below works in a local cairo coordinate system       */
/*  centred on the dial's geometric centre with +y pointing down (the  */
/*  cairo default).  The caller (pn_dial_paint_plot) translates the    */
/*  context once and passes the geometric radius into each helper, so  */
/*  the helpers themselves never have to mention the worksheet's       */
/*  outer (x, y, w, h) rectangle.                                       */
/* ------------------------------------------------------------------ */

/** Draw the metallic outer bezel: a thick ring formed by a linear
 *  gradient from a light grey at the top to a darker grey at the
 *  bottom (the classic "polished steel" reading), with a thin darker
 *  line on the inner edge to separate it from the face.  The
 *  highlight band running across the top sells the metallic look on
 *  its own, even without animation. */
static void
paint_bezel (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           outer = radius;
    double           inner = radius * 0.90;   /* bezel is 10% of radius */

    /* Outer disc: light-to-dark vertical gradient. */
    grad = cairo_pattern_create_linear (0.0, -outer, 0.0, outer);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.92, 0.92, 0.94);
    cairo_pattern_add_color_stop_rgb (grad, 0.45, 0.45, 0.45, 0.48);
    cairo_pattern_add_color_stop_rgb (grad, 0.55, 0.30, 0.30, 0.32);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.78, 0.78, 0.80);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, outer, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Inner rim: a second, narrower gradient ring sitting just inside
     * the outer one.  Reversing the direction makes the bezel read as
     * a beveled lip catching light from the opposite side, which is
     * what gives the bezel its sense of thickness. */
    grad = cairo_pattern_create_linear (0.0, -inner, 0.0, inner);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.55, 0.55, 0.57);
    cairo_pattern_add_color_stop_rgb (grad, 0.50, 0.85, 0.85, 0.87);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.40, 0.40, 0.42);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, inner * 1.03, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Dark hairline between bezel and face -- without it the bezel
     * gradient bleeds into the face fill on machines whose Cairo
     * builds antialias rings slightly inward. */
    cairo_set_source_rgba (cr, 0.10, 0.10, 0.10, 0.55);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, 0.0, 0.0, inner * 0.985, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);
}

/** Paint the dial face: a radial gradient from a slightly brighter
 *  centre to the user-chosen face_color at the rim.  The brightness
 *  bump in the middle gives the face a hint of curvature so the glass
 *  highlight painted later has something to sit on. */
static void
paint_face (cairo_t *cr, double radius, const GdkRGBA *face)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.88;
    double           cr_r, cg, cb;

    /* Brighter centre = face_color blended toward white. */
    cr_r = face->red   * 0.85 + 0.15;
    cg   = face->green * 0.85 + 0.15;
    cb   = face->blue  * 0.85 + 0.15;

    grad = cairo_pattern_create_radial (0.0, -r * 0.20, r * 0.20,
                                        0.0,  0.0,      r);
    cairo_pattern_add_color_stop_rgba (grad, 0.0, cr_r, cg, cb, 1.0);
    cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                       face->red, face->green, face->blue,
                                       1.0);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);
}

/** Paint a single coloured zone arc just inside the tick band.  The
 *  zone is drawn as a stroked thick arc rather than a filled wedge so
 *  it sits visually on the scale, not on the face -- the difference
 *  is what makes the dial in the reference image read as "scale
 *  decoration" rather than "pie chart slice".  No-op when start ≥ end
 *  (the user-disabled case). */
static void
paint_zone (
        cairo_t       *cr,
        PnDial        *self,
        double         radius,
        double         band_radius,
        double         band_width,
        double         zone_start,
        double         zone_end,
        const GdkRGBA *color)
{
    double a0, a1;

    (void) radius;

    if (zone_start >= zone_end)
        return;

    a0 = clock_to_cairo (value_to_angle (self, zone_start));
    a1 = clock_to_cairo (value_to_angle (self, zone_end));

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_set_line_width (cr, band_width);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
    cairo_new_sub_path (cr);
    cairo_arc (cr, 0.0, 0.0, band_radius, a0, a1);
    cairo_stroke (cr);
}

/** Pick a "nice" tick step that yields at most @target_ticks marks
 *  across a range of @range, snapping to one of {1, 2, 5} × 10^n.
 *  This is the same heuristic matplotlib / gnuplot use to keep axis
 *  labels at human-readable values (10, 20, 30 …) rather than at
 *  evenly-divided fractions of the range (10, 13.33, 16.67 …) -- a
 *  user reading a 0-16 dial with 13 requested ticks naturally
 *  expects to see "0, 2, 4, …, 16" and to be able to enter a zone
 *  bound of 9 and have it land at the value 9 on the face, not at
 *  ceil(9 / 1.333) * 1.333 = 9.333 the way evenly-subdivided ticks
 *  would have it. */
static double
pick_nice_step (double range, guint target_ticks)
{
    double raw, mag, norm, nice;

    if (target_ticks < 2 || !(range > 0.0))
        return 0.0;

    raw  = range / (double) (target_ticks - 1);
    mag  = pow (10.0, floor (log10 (raw)));
    norm = raw / mag;

    /* Round norm UP to the nearest of {1, 2, 5, 10} so the chosen
     * step yields at most target_ticks marks across the range -- the
     * dial respects the user's tick-count as an upper bound rather
     * than as a hard count. */
    if      (norm <= 1.0) nice =  1.0;
    else if (norm <= 2.0) nice =  2.0;
    else if (norm <= 5.0) nice =  5.0;
    else                  nice = 10.0;

    return nice * mag;
}

/** Format a tick value at @step's precision: integer when the step is
 *  an integer >= 1 (the common case for 0-100, 0-120, 0-16 dials),
 *  otherwise the minimum number of decimals that keep the printed
 *  values distinct from each other.  Uses %g for the fractional case
 *  so trailing zeros are dropped ("0.5" not "0.50"). */
static void
format_tick_label (gchar *buf, gsize sz, double val, double step)
{
    int decimals;

    /* Snap microscopic floating-point noise (e.g. 0.30000000000000004)
     * to a clean zero so a tick at "0" never prints as "3e-17". */
    if (fabs (val) < step * 1e-9)
        val = 0.0;

    if (step >= 1.0 && fabs (step - round (step)) < 1e-9)
    {
        g_snprintf (buf, sz, "%d", (int) round (val));
        return;
    }

    /* Pick enough decimals to make the step value distinguishable --
     * step 0.5 → 1 decimal, step 0.05 → 2 decimals, etc. */
    decimals = (int) ceil (-log10 (step));
    if (decimals < 1) decimals = 1;
    if (decimals > 6) decimals = 6;
    g_snprintf (buf, sz, "%.*f", decimals, val);
}

/** Paint the tick marks and digit labels.  Major ticks are longer and
 *  carry a digit; minor ticks are short and unlabelled.  Both use the
 *  configurable scale_color (the reference image uses a deep red but
 *  black or any other colour works the same).  Digits live just
 *  inside the tick ring -- placing them outside would push them onto
 *  the bezel where they would disappear into the metallic shading.
 *
 *  Major ticks are placed at "nice" value positions ({1, 2, 5} × 10^n
 *  apart) derived from the user's `major-ticks` target, so the
 *  printed digit at each tick really equals the underlying value at
 *  that angle and a zone bound entered as "9" lands at the "9" tick
 *  mark rather than at the awkward fraction (9.333…) an evenly-
 *  subdivided arc would have placed value 9 at.  Minor ticks fill
 *  evenly between consecutive major ticks. */
static void
paint_ticks (
        cairo_t *cr,
        PnDial  *self,
        double   radius)
{
    double      r_outer = radius * 0.84;    /* outer end of major ticks */
    double      r_minor = radius * 0.79;    /* inner end of minor ticks */
    double      r_major = radius * 0.74;    /* inner end of major ticks */
    double      r_digit = radius * 0.62;    /* digit centre radius */
    PangoLayout *layout;
    PangoFontDescription *fd;
    gchar        buf[32];
    double       step;
    double       first;
    double       v;
    double       prev_v;
    gboolean     have_prev;
    guint        minors;
    guint        j;

    if (self->major_ticks < 2)
        return;
    if (!(self->max_value > self->min_value))
        return;

    step   = pick_nice_step (self->max_value - self->min_value,
                             self->major_ticks);
    if (!(step > 0.0))
        return;

    minors = self->minor_ticks_per_major;

    /* First major tick is the smallest multiple of step that is not
     * less than min_value -- so a min_value of 0 lines up at the
     * start_angle, and a min_value of 3.5 with step=1 starts at 4
     * (the unmarked gap from 3.5 to 4 is the natural visual cue that
     * the scale starts mid-step). */
    first = ceil (self->min_value / step - 1e-9) * step;

    cairo_set_source_rgba (cr,
                           self->scale_color.red,
                           self->scale_color.green,
                           self->scale_color.blue,
                           self->scale_color.alpha);

    /* Minor ticks: between each pair of consecutive major-tick value
     * positions (including the implicit boundary at min_value / first
     * and at last / max_value).  Drawn first so the bolder major
     * ticks overpaint any antialias seam.  Capped at the scale ends
     * so a value slightly past max never gets a tick. */
    cairo_set_line_width (cr, 0.8);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    have_prev = FALSE;
    prev_v    = self->min_value;
    for (v = first; v <= self->max_value + step * 1e-9; v += step)
    {
        double seg_lo = have_prev ? prev_v : self->min_value;
        double seg_hi = v;

        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;

            if (mv <= self->min_value || mv >= self->max_value)
                continue;
            ang = clock_to_cairo (value_to_angle (self, mv));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_minor, sin (ang) * r_minor);
        }
        prev_v    = v;
        have_prev = TRUE;
    }
    /* Trailing minors between the last major tick and max_value, when
     * the scale extends past the last nice-step mark. */
    if (have_prev && prev_v < self->max_value)
    {
        double seg_lo = prev_v;
        double seg_hi = self->max_value;
        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;

            if (mv <= self->min_value || mv >= self->max_value)
                continue;
            ang = clock_to_cairo (value_to_angle (self, mv));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_minor, sin (ang) * r_minor);
        }
    }
    cairo_stroke (cr);

    /* Major ticks at the nice-step value positions. */
    cairo_set_line_width (cr, 1.6);
    for (v = first; v <= self->max_value + step * 1e-9; v += step)
    {
        double ang = clock_to_cairo (value_to_angle (self, v));
        cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
        cairo_line_to (cr, cos (ang) * r_major, sin (ang) * r_major);
    }
    cairo_stroke (cr);

    /* Digit labels.  A single Pango layout is rebuilt per tick with a
     * fresh value formatted into the same scratch buffer; that keeps
     * the per-paint allocation count to one layout (Pango reuses its
     * internal glyph cache for every set_text on the same layout). */
    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string ("Sans 9");
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    for (v = first; v <= self->max_value + step * 1e-9; v += step)
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
    }

    g_object_unref (layout);
}

/** Paint the "glass" reflection: a single arched line splitting the
 *  face into two translucent overlays painted on top of every other
 *  face element (ticks, digits, zones, needle, pivot) so the whole
 *  face reads as if it were under a curved glass cover.  No gradient
 *  anywhere -- one solid alpha for the upper "catch-light" overlay
 *  (translucent white) and one for the lower "shaded" overlay
 *  (translucent dark grey).  The dividing line bulges upward at its
 *  midpoint and is slanted so the left endpoint sits below the right
 *  endpoint; the off-axis bend reads as a reflection of an overhead
 *  source that is itself off-centre, which is what gives the face
 *  its sense of curved glass rather than a flat painted disc.       */
static void
paint_glass (cairo_t *cr, double radius)
{
    double r = radius * 0.88;
    /* Alpha kept low so the glass tints rather than obscures the    */
    /* readout underneath -- 10 % white and 10 % dark grey is enough */
    /* to read as a glass cover on a careful look without washing    */
    /* out the tick digits.                                           */
    const double glass_alpha = 0.10;

    cairo_save (cr);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_clip (cr);

    /* Upper-region path -- the visible boundary of this fill is a   */
    /* single cubic bezier from the left endpoint to the right       */
    /* endpoint; the rest of the path goes off-screen to the top so  */
    /* the upper face arc fills in completely.                       */
    /*   left endpoint:  (-r*1.05, +r*0.18)   below the equator      */
    /*   right endpoint: (+r*1.05, -r*0.15)   above the equator      */
    /*   midpoint:       (≈0, -r*0.45)        above both endpoints,  */
    /*                                       achieved via control    */
    /*                                       points pulled up        */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, glass_alpha);
    cairo_move_to    (cr, -r * 1.50, -r * 1.50);
    cairo_line_to    (cr, -r * 1.05,  r * 0.18);
    cairo_curve_to   (cr,
                      -r * 0.40, -r * 0.55,    /* control 1 */
                      +r * 0.40, -r * 0.55,    /* control 2 */
                      +r * 1.05, -r * 0.15);   /* right endpoint */
    cairo_line_to    (cr, +r * 1.50, -r * 1.50);
    cairo_close_path (cr);
    cairo_fill (cr);

    /* Lower-region path -- the same boundary curve flipped to enclose
     * the area below the line instead of above it. */
    cairo_set_source_rgba (cr, 0.10, 0.10, 0.12, glass_alpha);
    cairo_move_to    (cr, -r * 1.50,  r * 1.50);
    cairo_line_to    (cr, -r * 1.05,  r * 0.18);
    cairo_curve_to   (cr,
                      -r * 0.40, -r * 0.55,
                      +r * 0.40, -r * 0.55,
                      +r * 1.05, -r * 0.15);
    cairo_line_to    (cr, +r * 1.50,  r * 1.50);
    cairo_close_path (cr);
    cairo_fill (cr);

    cairo_restore (cr);
}

/** Paint one line of label text horizontally centred on the face at
 *  the given baseline y offset (measured from the dial centre, in
 *  cairo pixels), using @font_desc.  No-op when @text is empty so a
 *  worksheet that wants only one of the two label lines can leave the
 *  other one unset. */
static void
paint_text_line (
        cairo_t        *cr,
        const gchar    *text,
        const gchar    *font_desc,
        const GdkRGBA  *color,
        double          y_center)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    int                   pw, ph;

    if (text == NULL || *text == '\0')
        return;

    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string (font_desc);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_move_to (cr, -pw * 0.5, y_center - ph * 0.5);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

/** Paint the descriptive label + the unit, stacked.  Both live in
 *  the empty wedge below the centre pivot that the default 240°
 *  scale sweep leaves open at 6 o'clock.  The descriptive label
 *  sits closer to the centre in a small font; the unit sits below
 *  it in a much bigger font (the unit is typically one or two
 *  characters, so the bigger size still fits comfortably and reads
 *  as the primary identifier of the value at a single glance).
 *
 *  Both lines are clipped to the bottom-wedge pie slice between the
 *  scale's two endpoint rays (with a 5° angular inset on each side)
 *  so that an over-long label or unit gets cut off at the wedge
 *  boundary rather than bleeding into the bottom-corner tick digits. */
static void
paint_label (cairo_t *cr, PnDial *self, double radius)
{
    /* The two y baselines were picked so the unit's bigger glyphs
     * have headroom under the centre pivot and tail-room above the
     * inner bottom tick mark, with about 4 px of clear space
     * between the two lines on the canonical 220 px face. */
    double y_label = radius * 0.32;
    double y_unit  = radius * 0.58;
    double a0;
    double a1;

    cairo_save (cr);

    /* Pie-slice clip: apex at the dial centre, two straight edges
     * along the scale's start/end rays (inset 5° toward the gap so
     * the clip stops short of the bottom-corner digit text), and a
     * curved edge at the face radius so the labels stay inside the
     * face circle.  When the scale spans a full 360° the two rays
     * collapse to one and nothing renders -- which is the right
     * outcome, since a full-circle scale has no empty wedge for the
     * labels to live in. */
    a0 = clock_to_cairo (self->end_angle   +   5.0);
    a1 = clock_to_cairo (self->start_angle + 360.0 - 5.0);
    cairo_move_to    (cr, 0.0, 0.0);
    cairo_arc        (cr, 0.0, 0.0, radius * 0.88, a0, a1);
    cairo_close_path (cr);
    cairo_clip (cr);

    paint_text_line (cr, self->label, "Sans 10",
                     &self->label_color, y_label);
    paint_text_line (cr, self->unit,  "Sans 22",
                     &self->label_color, y_unit);

    cairo_restore (cr);
}

/** Paint the needle: a tapered triangle pointing outward from the
 *  centre with a thin counterweight on the opposite side.  The 3D
 *  effect comes from drawing the shape twice -- once with a darker
 *  shadow offset down-right, once with the user colour on top --
 *  which reads as the needle catching light from the upper-left like
 *  the rest of the dial. */
static void
paint_needle (cairo_t *cr, PnDial *self, double radius)
{
    double angle;

    /* Hide the needle entirely until the first message arrives -- a
     * needle parked at min_value looks identical to a needle that has
     * received a real reading of min_value, and the user wants the
     * two states visually distinguishable.  On the first message
     * has_value flips to TRUE while display_value is still at
     * min_value (start_anim has not yet run any tick), so the needle
     * pops into view at the leftmost position and the spring then
     * sweeps it to the received value -- exactly the "needle springs
     * in from the parked position" animation the user described. */
    if (!self->has_value)
        return;

    angle = clock_to_cairo (value_to_angle (self, self->display_value));
    double tip_r = radius * 0.78;      /* needle tip near outer ticks */
    double back_r = radius * 0.18;     /* short counterweight */
    double half_w = radius * 0.045;    /* needle half-width at base */
    double tx = cos (angle) * tip_r;
    double ty = sin (angle) * tip_r;
    double bx = cos (angle + M_PI) * back_r;
    double by = sin (angle + M_PI) * back_r;
    /* Perpendicular unit vector for the base of the triangle. */
    double px = -sin (angle);
    double py =  cos (angle);

    /* Shadow pass: same shape, offset 1.5 px to the lower-right, in
     * translucent black so it reads as a cast shadow on the dial face
     * regardless of the needle's user colour. */
    cairo_save (cr);
    cairo_translate (cr, 1.5, 1.5);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
    cairo_move_to (cr, tx, ty);
    cairo_line_to (cr, px * half_w, py * half_w);
    cairo_line_to (cr, bx, by);
    cairo_line_to (cr, -px * half_w, -py * half_w);
    cairo_close_path (cr);
    cairo_fill (cr);
    cairo_restore (cr);

    /* Needle body: user colour. */
    cairo_set_source_rgba (cr,
                           self->needle_color.red,
                           self->needle_color.green,
                           self->needle_color.blue,
                           self->needle_color.alpha);
    cairo_move_to (cr, tx, ty);
    cairo_line_to (cr, px * half_w, py * half_w);
    cairo_line_to (cr, bx, by);
    cairo_line_to (cr, -px * half_w, -py * half_w);
    cairo_close_path (cr);
    cairo_fill_preserve (cr);

    /* Highlight stroke along one edge of the needle.  Stroking the
     * whole closed path with a thin lighter colour catches the upper
     * edge in the rendered output (the bottom edge gets the dark
     * shadow pass underneath) which is what gives the needle its 3D
     * read at this size. */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.40);
    cairo_set_line_width (cr, 0.8);
    cairo_stroke (cr);
}

/** Paint the centre pivot: a small metallic cap covering the base of
 *  the needle.  Two stacked radial gradients give the cap its
 *  three-tone bevel: a darker outer rim, a mid-tone body, and a
 *  bright catch-light in the upper-left where the bezel highlight is
 *  brightest, so the pivot reads as a polished metal dome rather
 *  than a flat disc. */
static void
paint_centre (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.11;

    /* Outer rim: dark grey ring under the dome. */
    cairo_set_source_rgb (cr, 0.20, 0.20, 0.22);
    cairo_arc (cr, 0.0, 0.0, r * 1.15, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Body: bright-to-dark radial gradient. */
    grad = cairo_pattern_create_radial (-r * 0.35, -r * 0.35, r * 0.10,
                                        0.0,        0.0,      r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.95, 0.95, 0.97);
    cairo_pattern_add_color_stop_rgb (grad, 0.5, 0.60, 0.60, 0.62);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.35, 0.35, 0.37);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Catch-light: small bright dot in the upper-left of the dome. */
    grad = cairo_pattern_create_radial (-r * 0.30, -r * 0.30, 0.0,
                                        -r * 0.30, -r * 0.30, r * 0.45);
    cairo_pattern_add_color_stop_rgba (grad, 0.0, 1.0, 1.0, 1.0, 0.85);
    cairo_pattern_add_color_stop_rgba (grad, 1.0, 1.0, 1.0, 1.0, 0.00);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);
}

static void
pn_dial_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnDial *self   = PN_DIAL (node);
    double  side   = (w < h) ? w : h;
    double  radius = side * 0.5 - 4.0;       /* 4 px clear of the edge */
    double  cx     = x + w * 0.5;
    double  cy     = y + h * 0.5;
    double  band_radius;
    double  band_width;

    /* Move the origin to the dial centre once.  Every helper after
     * this point draws relative to that origin, which keeps the
     * geometry maths in each helper readable. */
    cairo_save (cr);
    cairo_translate (cr, cx, cy);

    paint_bezel (cr, radius);
    paint_face  (cr, radius, &self->face_color);

    /* Zone arcs sit on a band that lines up with the inner edge of
     * the major ticks; band_width is the visible thickness of the
     * coloured arc. */
    band_radius = radius * 0.76;
    band_width  = radius * 0.05;

    paint_zone (cr, self, radius, band_radius, band_width,
                self->green_start,  self->green_end,  &self->green_color);
    paint_zone (cr, self, radius, band_radius, band_width,
                self->yellow_start, self->yellow_end, &self->yellow_color);
    paint_zone (cr, self, radius, band_radius, band_width,
                self->red_start,    self->red_end,    &self->red_color);

    paint_ticks  (cr, self, radius);
    paint_label  (cr, self, radius);
    paint_needle (cr, self, radius);
    paint_centre (cr, radius);

    /* Glass goes last -- the two translucent overlays tint everything
     * underneath (digits, needle, pivot), which is how a real glass
     * cover would catch the reflection. */
    paint_glass (cr, radius);

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_class_tabs override             */
/*                                                                     */
/*  The default introspection-driven tab would lay out one row per     */
/*  writable property in a single grid -- with the dial's 21           */
/*  properties that is tall enough that the settings dialog no longer  */
/*  fits on a 1080 px screen.  Contribute four sibling pages           */
/*  (Data, Scale, Zones, Colours) directly to the dialog's top-level   */
/*  notebook via the multi-tab hook so the dialog reads as             */
/*                                                                     */
/*      Class | Node | Data | Scale | Zones | Colours                  */
/*                                                                     */
/*  with each page only as tall as its own row set.  Editors are      */
/*  built through pn_node_dialog_default_editor() so they match the   */
/*  look of every other auto-generated property tab.                  */
/* ------------------------------------------------------------------ */

/** Build a property grid containing one row per element of @names,
 *  looking each name up on @target's class.  Names that do not
 *  resolve to a writable property are silently skipped -- which lets
 *  the four category arrays below be authored as plain lists of
 *  property names without having to keep a parallel "this row is
 *  visible" flag in sync with the property table. */
static GtkWidget *
build_dial_page (
        GObject     *target,
        const gchar *const *names)
{
    GtkWidget   *grid  = pn_node_dialog_new_property_grid ();
    GObjectClass *klass = G_OBJECT_GET_CLASS (target);
    guint        row   = 0;
    guint        i;

    for (i = 0; names[i] != NULL; i++)
    {
        GParamSpec *pspec  = g_object_class_find_property (klass, names[i]);
        GtkWidget  *editor;
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
pn_dial_build_class_tabs (
        PnNode      *self,
        GtkNotebook *notebook,
        GtkWindow   *parent G_GNUC_UNUSED)
{
    GObject *target = G_OBJECT (self);

    /* The four category lists.  Order within each list is the order  */
    /* the rows appear on the page -- chosen to read top-down the way */
    /* a user would fill the dial in (binding first, then the         */
    /* geometry of the scale, then the optional zones, then the paint */
    /* colours that finish the look).                                  */
    static const gchar *const data_props[] = {
        "key", "label", "unit", NULL
    };
    static const gchar *const scale_props[] = {
        "min-value", "max-value",
        "start-angle", "end-angle",
        "major-ticks", "minor-ticks-per-major",
        NULL
    };
    static const gchar *const zone_props[] = {
        "green-start",  "green-end",  "green-color",
        "yellow-start", "yellow-end", "yellow-color",
        "red-start",    "red-end",    "red-color",
        NULL
    };
    static const gchar *const color_props[] = {
        "face-color", "scale-color",
        "needle-color", "label-color",
        NULL
    };

    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, data_props),
                                "Data");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, scale_props),
                                "Scale");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, zone_props),
                                "Zones");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, color_props),
                                "Colours");
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_dial_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_DIAL_WIDTH;
    if (out_h != NULL) *out_h = PN_DIAL_TOTAL_HEIGHT;
}

static double
pn_dial_get_header_height (PnNode *node)
{
    (void) node;
    return PN_DIAL_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dial_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDial *self = PN_DIAL (object);

    switch (prop_id)
    {
    case PROP_KEY:                   g_value_set_string  (value, self->key);                 break;
    case PROP_MIN_VALUE:             g_value_set_double  (value, self->min_value);           break;
    case PROP_MAX_VALUE:             g_value_set_double  (value, self->max_value);           break;
    case PROP_START_ANGLE:           g_value_set_double  (value, self->start_angle);         break;
    case PROP_END_ANGLE:             g_value_set_double  (value, self->end_angle);           break;
    case PROP_MAJOR_TICKS:           g_value_set_uint    (value, self->major_ticks);         break;
    case PROP_MINOR_TICKS_PER_MAJOR: g_value_set_uint    (value, self->minor_ticks_per_major); break;
    case PROP_LABEL:                 g_value_set_string  (value, self->label);               break;
    case PROP_UNIT:                  g_value_set_string  (value, self->unit);                break;
    case PROP_GREEN_START:           g_value_set_double  (value, self->green_start);         break;
    case PROP_GREEN_END:             g_value_set_double  (value, self->green_end);           break;
    case PROP_YELLOW_START:          g_value_set_double  (value, self->yellow_start);        break;
    case PROP_YELLOW_END:            g_value_set_double  (value, self->yellow_end);          break;
    case PROP_RED_START:             g_value_set_double  (value, self->red_start);           break;
    case PROP_RED_END:               g_value_set_double  (value, self->red_end);             break;
    case PROP_FACE_COLOR:            g_value_set_boxed   (value, &self->face_color);         break;
    case PROP_SCALE_COLOR:           g_value_set_boxed   (value, &self->scale_color);        break;
    case PROP_NEEDLE_COLOR:          g_value_set_boxed   (value, &self->needle_color);       break;
    case PROP_LABEL_COLOR:           g_value_set_boxed   (value, &self->label_color);        break;
    case PROP_GREEN_COLOR:           g_value_set_boxed   (value, &self->green_color);        break;
    case PROP_YELLOW_COLOR:          g_value_set_boxed   (value, &self->yellow_color);       break;
    case PROP_RED_COLOR:             g_value_set_boxed   (value, &self->red_color);          break;
    case PROP_VALUE:                 g_value_set_double  (value, self->target_value);        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* Helper: assign a string property, only repaint and notify when the
 * value actually changes.  Centralised because every string property
 * on the dial (key, label) follows the same pattern. */
static void
set_string_prop (
        PnDial      *self,
        gchar      **slot,
        const gchar *new_value,
        guint        prop_id)
{
    const gchar *normalised = new_value != NULL ? new_value : "";

    if (g_strcmp0 (*slot, normalised) == 0)
        return;

    g_free (*slot);
    *slot = g_strdup (normalised);
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

/* Helper: same idea for double properties. */
static void
set_double_prop (
        PnDial *self,
        double *slot,
        double  new_value,
        guint   prop_id)
{
    if (*slot == new_value)
        return;
    *slot = new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_uint_prop (
        PnDial *self,
        guint  *slot,
        guint   new_value,
        guint   prop_id)
{
    if (*slot == new_value)
        return;
    *slot = new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_color_prop (
        PnDial        *self,
        GdkRGBA       *slot,
        const GdkRGBA *new_value,
        guint          prop_id)
{
    if (new_value == NULL || gdk_rgba_equal (slot, new_value))
        return;
    *slot = *new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
pn_dial_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDial *self = PN_DIAL (object);

    switch (prop_id)
    {
    case PROP_KEY:
        set_string_prop (self, &self->key, g_value_get_string (value), PROP_KEY);
        break;
    case PROP_LABEL:
        set_string_prop (self, &self->label, g_value_get_string (value), PROP_LABEL);
        break;
    case PROP_UNIT:
        set_string_prop (self, &self->unit, g_value_get_string (value), PROP_UNIT);
        break;

    case PROP_MIN_VALUE:
        set_double_prop (self, &self->min_value, g_value_get_double (value), PROP_MIN_VALUE);
        break;
    case PROP_MAX_VALUE:
        set_double_prop (self, &self->max_value, g_value_get_double (value), PROP_MAX_VALUE);
        break;
    case PROP_START_ANGLE:
        set_double_prop (self, &self->start_angle, g_value_get_double (value), PROP_START_ANGLE);
        break;
    case PROP_END_ANGLE:
        set_double_prop (self, &self->end_angle, g_value_get_double (value), PROP_END_ANGLE);
        break;
    case PROP_GREEN_START:
        set_double_prop (self, &self->green_start, g_value_get_double (value), PROP_GREEN_START);
        break;
    case PROP_GREEN_END:
        set_double_prop (self, &self->green_end, g_value_get_double (value), PROP_GREEN_END);
        break;
    case PROP_YELLOW_START:
        set_double_prop (self, &self->yellow_start, g_value_get_double (value), PROP_YELLOW_START);
        break;
    case PROP_YELLOW_END:
        set_double_prop (self, &self->yellow_end, g_value_get_double (value), PROP_YELLOW_END);
        break;
    case PROP_RED_START:
        set_double_prop (self, &self->red_start, g_value_get_double (value), PROP_RED_START);
        break;
    case PROP_RED_END:
        set_double_prop (self, &self->red_end, g_value_get_double (value), PROP_RED_END);
        break;

    case PROP_MAJOR_TICKS:
        set_uint_prop (self, &self->major_ticks, g_value_get_uint (value), PROP_MAJOR_TICKS);
        break;
    case PROP_MINOR_TICKS_PER_MAJOR:
        set_uint_prop (self, &self->minor_ticks_per_major,
                       g_value_get_uint (value), PROP_MINOR_TICKS_PER_MAJOR);
        break;

    case PROP_FACE_COLOR:
        set_color_prop (self, &self->face_color,   g_value_get_boxed (value), PROP_FACE_COLOR);   break;
    case PROP_SCALE_COLOR:
        set_color_prop (self, &self->scale_color,  g_value_get_boxed (value), PROP_SCALE_COLOR);  break;
    case PROP_NEEDLE_COLOR:
        set_color_prop (self, &self->needle_color, g_value_get_boxed (value), PROP_NEEDLE_COLOR); break;
    case PROP_LABEL_COLOR:
        set_color_prop (self, &self->label_color,  g_value_get_boxed (value), PROP_LABEL_COLOR);  break;
    case PROP_GREEN_COLOR:
        set_color_prop (self, &self->green_color,  g_value_get_boxed (value), PROP_GREEN_COLOR);  break;
    case PROP_YELLOW_COLOR:
        set_color_prop (self, &self->yellow_color, g_value_get_boxed (value), PROP_YELLOW_COLOR); break;
    case PROP_RED_COLOR:
        set_color_prop (self, &self->red_color,    g_value_get_boxed (value), PROP_RED_COLOR);    break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dial_finalize (GObject *object)
{
    PnDial *self = PN_DIAL (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }
    if (self->anim_id != 0)
    {
        g_source_remove (self->anim_id);
        self->anim_id = 0;
    }

    g_clear_pointer (&self->key,   g_free);
    g_clear_pointer (&self->label, g_free);
    g_clear_pointer (&self->unit,  g_free);

    G_OBJECT_CLASS (pn_dial_parent_class)->finalize (object);
}

static void
pn_dial_class_init (PnDialClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_dial_get_property;
    object_class->set_property = pn_dial_set_property;
    object_class->finalize     = pn_dial_finalize;

    node_class->receive                = pn_dial_receive;
    node_class->get_size               = pn_dial_get_size;
    node_class->get_header_height      = pn_dial_get_header_height;
    node_class->paint_plot             = pn_dial_paint_plot;
    node_class->build_class_tabs       = pn_dial_build_class_tabs;
    /* The dial face is circular -- the standard rectangular drop shadow
     * leaks out of the corners and reads as a rectangular ghost around
     * the otherwise-round dial. */
    node_class->paint_plot_skip_shadow = TRUE;

    node_class->class_name        = "Dial";
    node_class->icon              = "\xef\x83\xa4";       /* fa-tachometer U+F0E4 */
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
            "Value at the start (low end) of the dial's scale",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_VALUE] = g_param_spec_double (
            "max-value", "Max value",
            "Value at the end (high end) of the dial's scale",
            -G_MAXDOUBLE, G_MAXDOUBLE, 120.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_START_ANGLE] = g_param_spec_double (
            "start-angle", "Start angle",
            "Angle (degrees, clockwise from 12 o'clock) at which the "
            "scale's low end sits.  -120 puts it at the 8 o'clock "
            "position.",
            -360.0, 360.0, -120.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_END_ANGLE] = g_param_spec_double (
            "end-angle", "End angle",
            "Angle (degrees, clockwise from 12 o'clock) at which the "
            "scale's high end sits.  +120 puts it at the 4 o'clock "
            "position.",
            -360.0, 360.0, 120.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAJOR_TICKS] = g_param_spec_uint (
            "major-ticks", "Major ticks",
            "Target upper bound on the number of major tick marks the "
            "dial draws along the scale.  The actual marks are placed "
            "at \"nice\" value positions ({1, 2, 5} \xc3\x97 10^n apart) "
            "derived from this target, so the printed digit at each "
            "tick really equals the underlying value at that angle and "
            "a zone bound entered as \"9\" lands at the \"9\" tick "
            "mark rather than at a fractional position an evenly-"
            "subdivided arc would have placed it at.  The chosen step "
            "yields at most this many marks across the configured "
            "range; for a clean range / step ratio (0-120 with target "
            "13 \xe2\x86\x92 step 10 \xe2\x86\x92 13 ticks) the target "
            "is hit exactly.",
            2, 50, 13,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MINOR_TICKS_PER_MAJOR] = g_param_spec_uint (
            "minor-ticks-per-major", "Minor ticks per major",
            "Number of minor tick marks drawn between two consecutive "
            "major ticks.  Set to 1 to suppress minor ticks.",
            1, 20, 5,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LABEL] = g_param_spec_string (
            "label", "Label",
            "Descriptive line drawn at the lower centre of the face "
            "(in the wedge below the centre pivot) in a medium font.  "
            "Typically the name of the quantity being measured "
            "(\"speed\", \"tank A\").  Empty to suppress this line.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNIT] = g_param_spec_string (
            "unit", "Unit",
            "Short suffix drawn below the descriptive label in a much "
            "bigger font -- typically one or two characters (\"L\", "
            "\"km/h\", \"°C\") so it reads as the dominant identifier "
            "for the value on a single-glance read.  Empty to suppress.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GREEN_START] = g_param_spec_double (
            "green-start", "Green zone start",
            "Value at which the green zone arc begins.  When "
            "green-start >= green-end the zone is not drawn.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_GREEN_END] = g_param_spec_double (
            "green-end", "Green zone end",
            "Value at which the green zone arc ends.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 60.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_YELLOW_START] = g_param_spec_double (
            "yellow-start", "Yellow zone start",
            "Value at which the yellow zone arc begins.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 60.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_YELLOW_END] = g_param_spec_double (
            "yellow-end", "Yellow zone end",
            "Value at which the yellow zone arc ends.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 90.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RED_START] = g_param_spec_double (
            "red-start", "Red zone start",
            "Value at which the red zone arc begins.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 90.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_RED_END] = g_param_spec_double (
            "red-end", "Red zone end",
            "Value at which the red zone arc ends.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 120.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FACE_COLOR] = g_param_spec_boxed (
            "face-color", "Face colour",
            "Base fill colour of the dial face (the centre of the "
            "face is automatically brightened slightly to give the "
            "glass effect something to sit on)",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_SCALE_COLOR] = g_param_spec_boxed (
            "scale-color", "Scale colour",
            "Colour of the tick marks and digit labels",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_NEEDLE_COLOR] = g_param_spec_boxed (
            "needle-color", "Needle colour",
            "Fill colour of the needle body",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_LABEL_COLOR] = g_param_spec_boxed (
            "label-color", "Label colour",
            "Colour of the lower-centre label text",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_GREEN_COLOR] = g_param_spec_boxed (
            "green-color", "Green zone colour",
            "Stroke colour of the green zone arc",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_YELLOW_COLOR] = g_param_spec_boxed (
            "yellow-color", "Yellow zone colour",
            "Stroke colour of the yellow zone arc",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_RED_COLOR] = g_param_spec_boxed (
            "red-color", "Red zone colour",
            "Stroke colour of the red zone arc",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Read-only: the most recent value the needle was sent to.  Exists
     * for inspection and functional tests; G_PARAM_READABLE-only keeps
     * it out of the settings dialog and out of the saved file (the
     * serialiser persists only read/write properties). */
    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Most recent value received on the input (read-only)",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_dial_init (PnDial *self)
{
    PnNode *node = PN_NODE (self);

    self->key                   = g_strdup ("data/value");
    self->label                 = g_strdup ("");
    self->unit                  = g_strdup ("");
    self->min_value             = 0.0;
    self->max_value             = 120.0;
    self->start_angle           = -120.0;
    self->end_angle             = 120.0;
    self->major_ticks           = 13;
    self->minor_ticks_per_major = 5;

    self->green_start  = 0.0;   self->green_end  = 60.0;
    self->yellow_start = 60.0;  self->yellow_end = 90.0;
    self->red_start    = 90.0;  self->red_end    = 120.0;

    /* Defaults chosen to match the look of the reference dial.png:
     * a pale-grey face, deep-red scale + needle, dark label. */
    self->face_color   = (GdkRGBA){ 0.91, 0.91, 0.91, 1.0 };
    self->scale_color  = (GdkRGBA){ 0.70, 0.10, 0.10, 1.0 };
    self->needle_color = (GdkRGBA){ 0.80, 0.10, 0.10, 1.0 };
    self->label_color  = (GdkRGBA){ 0.15, 0.15, 0.15, 1.0 };
    self->green_color  = (GdkRGBA){ 0.20, 0.65, 0.25, 0.85 };
    self->yellow_color = (GdkRGBA){ 0.95, 0.80, 0.15, 0.85 };
    self->red_color    = (GdkRGBA){ 0.85, 0.20, 0.15, 0.85 };

    self->target_value     = self->min_value;
    self->display_value    = self->min_value;
    self->display_velocity = 0.0;
    self->has_value        = FALSE;

    self->anim_id      = 0;
    self->anim_last_us = 0;

    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    {
        GdkRGBA yellow = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, (const PnColor *)&yellow);
    }
    pn_node_set_class_name (node, "Dial");
    pn_node_set_icon       (node, "\xef\x83\xa4");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDial *
pn_dial_new (void)
{
    return g_object_new (PN_TYPE_DIAL, NULL);
}
