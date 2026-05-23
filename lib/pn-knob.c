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

#include "pn-knob.h"
#include <gtk/gtk.h>
#include "pn-message.h"

#include <math.h>
#include <json-glib/json-glib.h>

/* fa-dot-circle U+F192 -- a ringed disc that reads as a rotary knob,
 * pinned across the palette / inspector / icon panel (the dial widget
 * in the header is the live position indicator, so the side glyph
 * stays put). */
#define PN_KNOB_ICON  "\xef\x86\x92"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The knob decoration sits on the right edge of the node header,    */
/*  mirroring PnSwitch's right-decoration approach so the standard     */
/*  Node-RED header (icon panel + centred label) stays untouched and  */
/*  the output port keeps its usual position on the right.            */
/* ------------------------------------------------------------------ */

#define PN_KNOB_NODE_WIDTH      170.0
#define PN_KNOB_NODE_HEIGHT      40.0

#define PN_KNOB_RIGHT_PAD        12.0
#define PN_KNOB_RADIUS           13.0

#define PN_KNOB_RESERVED_RIGHT  (2.0 * PN_KNOB_RADIUS + PN_KNOB_RIGHT_PAD + 6.0)

/* Wheel detents required to drive the knob across the whole range.
 * One scroll notch therefore moves the value by (max-min)/TICKS. */
#define PN_KNOB_SCROLL_TICKS     24.0

#define PN_KNOB_DEFAULT_MIN       0.0
#define PN_KNOB_DEFAULT_MAX       1.0

/* The knob's pointer sweeps 270 degrees with the dead zone at the
 * bottom: minimum parks the indicator at the 7-o'clock position,
 * maximum at 5-o'clock, passing straight up (12-o'clock) at the
 * midpoint.  Expressed as cairo angles (0 = +x, growing clockwise on
 * screen because y points down): the sweep runs from 135 degrees
 * clockwise through the top to 135+270 degrees. */
#define PN_KNOB_SWEEP_START     (0.75 * G_PI)
#define PN_KNOB_SWEEP_EXTENT    (1.5  * G_PI)

struct _PnKnob
{
    PnNode parent_instance;

    /* Range bounds and the current value.  @value is always kept
     * clamped into [min, max] (whichever bound is lower / higher, so
     * an inverted range still behaves).  The knob's visual angle is
     * derived from these on every paint. */
    gdouble min_value;
    gdouble max_value;
    gdouble value;

    /* Main-loop source id of the one-shot "announce my initial value"
     * idle scheduled in constructed(); 0 once it has fired or been
     * cancelled.  Tracked so dispose() can pull it if the node dies
     * before the idle runs. */
    guint   startup_emit_id;
};

G_DEFINE_TYPE (PnKnob, pn_knob, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_MIN,
    PROP_MAX,
    PROP_VALUE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Value helpers                                                       */
/* ------------------------------------------------------------------ */

/** Clamp @v into the closed interval spanned by the knob's bounds,
 *  tolerating an inverted range (max < min) by ordering the limits. */
static gdouble
clamp_to_range (PnKnob *self, gdouble v)
{
    gdouble lo = MIN (self->min_value, self->max_value);
    gdouble hi = MAX (self->min_value, self->max_value);

    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** Normalised knob position in [0, 1]: 0 at @min_value, 1 at
 *  @max_value.  Collapses to 0 when the range has zero width so the
 *  pointer parks at the minimum rather than dividing by zero. */
static gdouble
value_fraction (PnKnob *self)
{
    gdouble range = self->max_value - self->min_value;
    gdouble t;

    if (range == 0.0)
        return 0.0;

    t = (self->value - self->min_value) / range;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t;
}

/* ------------------------------------------------------------------ */
/*  Message emission                                                    */
/* ------------------------------------------------------------------ */

static void
emit_value_message (PnKnob *self)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg  = pn_message_new (node, NULL);

    pn_message_set_double (msg, "value", self->value);

    pn_node_emit_message (node, msg);
    g_object_unref (msg);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                            */
/* ------------------------------------------------------------------ */

/** Paint a rotary knob -- a recessed sweep track, a domed body lit
 *  from the upper-left, and a pointer line marking the current
 *  position.  Drawn concentric on (@cx, @cy) with radius @r, with the
 *  pointer at normalised position @t in [0, 1]. */
static void
paint_knob (
        cairo_t *cr,
        double   cx,
        double   cy,
        double   r,
        double   t)
{
    const double track_r  = r + 2.0;
    const double angle     = PN_KNOB_SWEEP_START + t * PN_KNOB_SWEEP_EXTENT;
    const double pointer_r = r - 3.0;

    /* Sweep track -- a thin dark arc behind the body marking the range
     * of travel, with the dead zone left open at the bottom so the
     * knob reads as a real rotary control with end stops. */
    cairo_set_line_width (cr, 2.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
    cairo_new_sub_path (cr);
    cairo_arc (cr, cx, cy, track_r,
               PN_KNOB_SWEEP_START,
               PN_KNOB_SWEEP_START + PN_KNOB_SWEEP_EXTENT);
    cairo_stroke (cr);

    /* Body -- a near-white disc with a radial gradient lit from the
     * upper-left so it reads as a physical domed knob, matching the
     * PnSwitch thumb's material. */
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial (
                cx - r * 0.30, cy - r * 0.30, r * 0.10,
                cx, cy, r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 1.00, 1.00, 1.00);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.78, 0.78, 0.80);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Rim hairline seals the body against the track. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, cx, cy, r - 0.4, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);

    /* Pointer -- a short stroke from near the centre out to the rim,
     * marking the current position.  Capped round so it reads as a
     * moulded indicator notch rather than a bare line. */
    cairo_set_source_rgb (cr, 0.16, 0.18, 0.20);
    cairo_set_line_width (cr, 2.4);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr,
                   cx + cos (angle) * (r * 0.30),
                   cy + sin (angle) * (r * 0.30));
    cairo_line_to (cr,
                   cx + cos (angle) * pointer_r,
                   cy + sin (angle) * pointer_r);
    cairo_stroke (cr);
}

static void
pn_knob_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnKnob      *self = PN_KNOB (node);
    const double cx   = x + w - PN_KNOB_RIGHT_PAD - PN_KNOB_RADIUS;
    const double cy   = y + h * 0.5;

    paint_knob (cr, cx, cy, PN_KNOB_RADIUS, value_fraction (self));
}

/* ------------------------------------------------------------------ */
/*  Size vfunc                                                          */
/* ------------------------------------------------------------------ */

static void
pn_knob_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_KNOB_NODE_WIDTH;
    if (out_h != NULL) *out_h = PN_KNOB_NODE_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Hit-test / wheel rotation                                          */
/* ------------------------------------------------------------------ */

gboolean
pn_knob_hit_knob (
        PnKnob *self,
        double  px,
        double  py)
{
    PnNode        *node;
    const PnPoint *pos;
    double         w;
    double         h;
    double         cx;
    double         cy;
    double         dx;
    double         dy;
    double         rr;

    g_return_val_if_fail (PN_IS_KNOB (self), FALSE);

    node = PN_NODE (self);
    pos  = pn_node_get_position (node);
    pn_node_get_size (node, &w, &h);

    cx = pos->x + w - PN_KNOB_RIGHT_PAD - PN_KNOB_RADIUS;
    cy = pos->y + pn_node_get_header_height (node) * 0.5;

    dx = px - cx;
    dy = py - cy;
    rr = PN_KNOB_RADIUS + 2.0;   /* slight slack for comfortable aim */

    return (dx * dx + dy * dy) <= rr * rr;
}

void
pn_knob_scroll (
        PnKnob *self,
        double  dy)
{
    gdouble range;
    gdouble step;
    gdouble next;

    g_return_if_fail (PN_IS_KNOB (self));

    range = self->max_value - self->min_value;
    if (range == 0.0)
        return;   /* zero-width range: nothing to rotate through */

    /* Wheel up (dy < 0) increases the value; one detent moves by a
     * fixed fraction of the range. */
    step = range / PN_KNOB_SCROLL_TICKS;
    next = clamp_to_range (self, self->value - dy * step);

    if (next == self->value)
        return;   /* already parked at the end stop */

    self->value = next;
    pn_node_request_repaint (PN_NODE (self));
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALUE]);

    emit_value_message (self);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_knob_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnKnob *self = PN_KNOB (object);

    switch (prop_id)
    {
    case PROP_MIN:
        g_value_set_double (value, self->min_value);
        break;
    case PROP_MAX:
        g_value_set_double (value, self->max_value);
        break;
    case PROP_VALUE:
        g_value_set_double (value, self->value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_knob_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnKnob *self = PN_KNOB (object);

    switch (prop_id)
    {
    case PROP_MIN:
        {
            gdouble v = g_value_get_double (value);
            if (self->min_value != v)
            {
                gdouble clamped;
                self->min_value = v;
                /* Re-clamp the value into the new range; notify only
                 * if it actually shifted. */
                clamped = clamp_to_range (self, self->value);
                if (clamped != self->value)
                {
                    self->value = clamped;
                    g_object_notify_by_pspec (object, props[PROP_VALUE]);
                }
                pn_node_request_repaint (PN_NODE (self));
                g_object_notify_by_pspec (object, props[PROP_MIN]);
            }
        }
        break;
    case PROP_MAX:
        {
            gdouble v = g_value_get_double (value);
            if (self->max_value != v)
            {
                gdouble clamped;
                self->max_value = v;
                clamped = clamp_to_range (self, self->value);
                if (clamped != self->value)
                {
                    self->value = clamped;
                    g_object_notify_by_pspec (object, props[PROP_VALUE]);
                }
                pn_node_request_repaint (PN_NODE (self));
                g_object_notify_by_pspec (object, props[PROP_MAX]);
            }
        }
        break;
    case PROP_VALUE:
        pn_knob_set_value (self, g_value_get_double (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Startup announce                                                    */
/*                                                                      */
/*  A knob is a manual source: its only other output is the message it  */
/*  emits when the user turns it.  Without a one-shot announce a         */
/*  freshly-loaded (or freshly-dropped) knob would leave every          */
/*  downstream node ignorant of its position until the first manual     */
/*  turn -- exactly the gap the periodic data sources avoid by firing   */
/*  once shortly after the worksheet loads.  The knob wants the same    */
/*  "report on startup" behaviour but, unlike a PnAutoTrigger data      */
/*  collector, it must fire only once and never on a recurring timer.   */
/* ------------------------------------------------------------------ */

static gboolean
emit_startup_value (gpointer data)
{
    PnKnob *self = PN_KNOB (data);

    self->startup_emit_id = 0;
    emit_value_message (self);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
pn_knob_constructed (GObject *object)
{
    PnKnob *self = PN_KNOB (object);

    G_OBJECT_CLASS (pn_knob_parent_class)->constructed (object);

    /* Defer the announce to a main-loop idle rather than emitting here:
     * at construction time the load path has built this node but not yet
     * attached its output wires (node_from_json creates every node first,
     * applies their properties, and only then connects the wires), so an
     * emit now would reach nobody.  By the time the idle runs the load
     * call has returned to the main loop, the graph is fully wired, and
     * self->value holds the position restored from the file.  The idle
     * holds no reference on @self; dispose() pulls it if the node dies
     * first. */
    self->startup_emit_id = g_idle_add (emit_startup_value, self);
}

static void
pn_knob_dispose (GObject *object)
{
    PnKnob *self = PN_KNOB (object);

    if (self->startup_emit_id != 0)
    {
        g_source_remove (self->startup_emit_id);
        self->startup_emit_id = 0;
    }

    G_OBJECT_CLASS (pn_knob_parent_class)->dispose (object);
}

static void
pn_knob_class_init (PnKnobClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_knob_get_property;
    object_class->set_property = pn_knob_set_property;
    object_class->constructed  = pn_knob_constructed;
    object_class->dispose      = pn_knob_dispose;

    node_class->get_size             = pn_knob_get_size;
    node_class->paint_header_overlay = pn_knob_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_KNOB_RESERVED_RIGHT;

    node_class->palette_icon = PN_KNOB_ICON;
    node_class->class_name   = "Knob";
    node_class->icon         = PN_KNOB_ICON;
    node_class->color        = (PnColor){ 0.50, 0.45, 0.72, 1.0 };
    node_class->category     = "Sources";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;

    props[PROP_MIN] = g_param_spec_double (
            "min", "Min",
            "Value emitted when the knob is turned fully left.",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            PN_KNOB_DEFAULT_MIN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX] = g_param_spec_double (
            "max", "Max",
            "Value emitted when the knob is turned fully right.",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            PN_KNOB_DEFAULT_MAX,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Current knob position, in [min, max].  Writing this turns "
            "the dial without emitting; spin the mouse wheel over the "
            "knob to turn it and emit a message in one step.",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            PN_KNOB_DEFAULT_MIN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_knob_init (PnKnob *self)
{
    PnNode *node = PN_NODE (self);

    self->min_value = PN_KNOB_DEFAULT_MIN;
    self->max_value = PN_KNOB_DEFAULT_MAX;
    self->value     = PN_KNOB_DEFAULT_MIN;
    self->startup_emit_id = 0;

    pn_node_set_class_name (node, "Knob");
    pn_node_set_icon       (node, PN_KNOB_ICON);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    {
        PnColor indigo = { 0.50, 0.45, 0.72, 1.0 };
        pn_node_set_color (node, &indigo);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnKnob *
pn_knob_new (void)
{
    return g_object_new (PN_TYPE_KNOB, NULL);
}

gdouble
pn_knob_get_value (PnKnob *self)
{
    g_return_val_if_fail (PN_IS_KNOB (self), 0.0);
    return self->value;
}

void
pn_knob_set_value (PnKnob *self, gdouble value)
{
    gdouble clamped;

    g_return_if_fail (PN_IS_KNOB (self));

    clamped = clamp_to_range (self, value);
    if (clamped == self->value)
        return;

    self->value = clamped;
    pn_node_request_repaint (PN_NODE (self));
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALUE]);
}
