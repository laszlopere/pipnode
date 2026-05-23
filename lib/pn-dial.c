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
    PnColor   face_color;
    PnColor   scale_color;
    PnColor   needle_color;
    PnColor   label_color;
    PnColor   green_color;
    PnColor   yellow_color;
    PnColor   red_color;

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
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Snapshot every field the gui-tier cairo painter (pn-dial-gui.c)    */
/*  reads to draw one frame.  Keeping the painter behind this snapshot */
/*  lets the drawing code live in a separate translation unit without  */
/*  reaching into the private instance struct.                         */
/* ------------------------------------------------------------------ */

void
pn_dial_get_paint_state (PnDial *self, PnDialPaintState *out)
{
    g_return_if_fail (PN_IS_DIAL (self));
    g_return_if_fail (out != NULL);

    out->min_value             = self->min_value;
    out->max_value             = self->max_value;
    out->start_angle           = self->start_angle;
    out->end_angle             = self->end_angle;
    out->major_ticks           = self->major_ticks;
    out->minor_ticks_per_major = self->minor_ticks_per_major;

    out->label = self->label;
    out->unit  = self->unit;

    out->green_start  = self->green_start;   out->green_end  = self->green_end;
    out->yellow_start = self->yellow_start;  out->yellow_end = self->yellow_end;
    out->red_start    = self->red_start;     out->red_end    = self->red_end;

    out->face_color   = self->face_color;
    out->scale_color  = self->scale_color;
    out->needle_color = self->needle_color;
    out->label_color  = self->label_color;
    out->green_color  = self->green_color;
    out->yellow_color = self->yellow_color;
    out->red_color    = self->red_color;

    out->display_value = self->display_value;
    out->has_value     = self->has_value;
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
        PnColor       *slot,
        const PnColor *new_value,
        guint          prop_id)
{
    if (new_value == NULL || pn_color_equal (slot, new_value))
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
    /* The cairo dial-face painter (paint_plot + its skip-shadow flag)
     * and the four-tab settings dialog (build_class_tabs) are installed
     * onto this class by the gui tier — pn_dial_gui_install() in
     * pn-dial-gui.c — so the headless core carries no GTK/cairo. */

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
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_SCALE_COLOR] = g_param_spec_boxed (
            "scale-color", "Scale colour",
            "Colour of the tick marks and digit labels",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_NEEDLE_COLOR] = g_param_spec_boxed (
            "needle-color", "Needle colour",
            "Fill colour of the needle body",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_LABEL_COLOR] = g_param_spec_boxed (
            "label-color", "Label colour",
            "Colour of the lower-centre label text",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_GREEN_COLOR] = g_param_spec_boxed (
            "green-color", "Green zone colour",
            "Stroke colour of the green zone arc",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_YELLOW_COLOR] = g_param_spec_boxed (
            "yellow-color", "Yellow zone colour",
            "Stroke colour of the yellow zone arc",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    props[PROP_RED_COLOR] = g_param_spec_boxed (
            "red-color", "Red zone colour",
            "Stroke colour of the red zone arc",
            PN_TYPE_COLOR,
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
    self->face_color   = (PnColor){ 0.91, 0.91, 0.91, 1.0 };
    self->scale_color  = (PnColor){ 0.70, 0.10, 0.10, 1.0 };
    self->needle_color = (PnColor){ 0.80, 0.10, 0.10, 1.0 };
    self->label_color  = (PnColor){ 0.15, 0.15, 0.15, 1.0 };
    self->green_color  = (PnColor){ 0.20, 0.65, 0.25, 0.85 };
    self->yellow_color = (PnColor){ 0.95, 0.80, 0.15, 0.85 };
    self->red_color    = (PnColor){ 0.85, 0.20, 0.15, 0.85 };

    self->target_value     = self->min_value;
    self->display_value    = self->min_value;
    self->display_velocity = 0.0;
    self->has_value        = FALSE;

    self->anim_id      = 0;
    self->anim_last_us = 0;

    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    {
        PnColor yellow = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &yellow);
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
