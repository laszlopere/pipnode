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
/*  PnOscilloscope — GTK-free core (TODO #44).                         */
/*                                                                     */
/*  The display end of the analog-computer toy: a square green-        */
/*  phosphor CRT that traces a Y value against an X value.  Both       */
/*  values are read from each message and each may be a plain number   */
/*  or a $pnvector (TODO #43).  A vector value is the whole captured    */
/*  waveform (snapshot, replacing the screen); a scalar value is the    */
/*  single current point — the latest (x, value) only, drawn as a dot   */
/*  (no history is kept).  Both axes auto-fit the data.  This file owns  */
/*  the type, properties, the receive() contract and the trace store;  */
/*  the cairo CRT painter lives in pn-oscilloscope-gui.c and reads the  */
/*  trace back through the GTK-free seam declared in the header.        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-oscilloscope.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-vector.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  A standard 40 px header sits above a roughly 5:4 CRT screen; the    */
/*  default header→body gap (PN_NODE_PLOT_GAP, 4 px) separates them, so */
/*  the default client-area maths hands the painter exactly the screen  */
/*  rectangle.                                                          */
/* ------------------------------------------------------------------ */

#define PN_OSC_WIDTH          260.0
#define PN_OSC_HEADER_HEIGHT   40.0
#define PN_OSC_GAP              4.0
#define PN_OSC_SCREEN_HEIGHT  210.0
#define PN_OSC_TOTAL_HEIGHT   (PN_OSC_HEADER_HEIGHT + \
                               PN_OSC_GAP +           \
                               PN_OSC_SCREEN_HEIGHT)

/* Maximum repaint rate — the minimum gap between two worksheet redraw
 * requests from this node.  100 ms (10 Hz) is below the perception
 * threshold for a streaming trace yet stops a high-rate feed from
 * forcing a full re-render every display frame.  Mirrors #PnXYGraph. */
#define PN_OSC_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

/* Afterglow persistence — how long a vacated dot position lingers on the
 * phosphor before it has fully decayed.  800 ms reads as a clear streak
 * trailing a moving dot without smearing the whole screen. */
#define PN_OSC_AFTERGLOW_US  (G_TIME_SPAN_MILLISECOND * 800)

/* Fade animation cadence — while the trail is alive the core re-requests a
 * repaint at this interval so the glow decays smoothly after the dot stops
 * moving (message-driven repaints alone would freeze a static dot's trail).
 * ~25 fps is smooth yet only runs for the ~0.8 s a trail takes to die. */
#define PN_OSC_FADE_INTERVAL_MS  40

struct _PnOscilloscope
{
    PnNode parent_instance;

    gchar  *value_key;   /* data-bag member for the Y value */
    gchar  *x_key;       /* data-bag member for the X value */

    /* Appearance — only affects how the trace is drawn; flipping any of
     * these schedules a repaint but never touches the trace store. */
    PnColor   screen_color;
    PnColor   trace_color;
    PnColor   grid_color;
    gboolean  show_graticule;
    guint     trace_width;
    gboolean  x_from_zero;
    gboolean  y_from_zero;

    /* Trace store.  Exactly one of the two is live at a time:
     *   • snapshot  — vy != NULL: the vector is the trace, vx (optional)
     *     supplies per-sample X (else the sample index is used).
     *   • point     — vy == NULL: the single latest (x, y) scalar, drawn
     *     as a dot.  No history is kept — each scalar replaces the last. */
    PnVector *vy;        /* snapshot Y vector (ref), or NULL */
    PnVector *vx;        /* snapshot X vector (ref), or NULL */

    gboolean  have_point;  /* a scalar point is live (vy == NULL)        */
    gdouble   last_x;      /* the current point's X                      */
    gdouble   last_y;      /* the current point's Y                      */

    /* Accumulated bounds of every scalar point seen since the last reset.
     * Only the latest point is ever drawn, but auto-fitting a lone point
     * would pin it dead-centre; tracking the explored min/max gives the
     * dot a stable frame so it visibly MOVES as the inputs change.  Reset
     * when a vector snapshot takes over.  (No sample history is kept —
     * just the four extents.) */
    gboolean  have_bounds;
    gdouble   bx_lo, bx_hi;
    gdouble   by_lo, by_hi;

    /* Afterglow trail — a ring of recent positions the dot has moved AWAY
     * from, each stamped with the monotonic time it was vacated, ordered
     * oldest→newest.  The painter draws a fading segment from each to the
     * next-newer position (and from the newest to the live dot) plus a
     * fading dot, so a moving dot leaves a phosphor streak.  Reset when a
     * vector snapshot takes over (vectors keep no trail). */
    gdouble   trail_x[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    gdouble   trail_y[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    gint64    trail_t[PN_OSCILLOSCOPE_AFTERGLOW_MAX];
    guint     trail_head;   /* index of the oldest live entry              */
    guint     trail_count;  /* number of live entries                      */
    guint     fade_anim_id; /* repaint timer while the trail is decaying   */

    /* Repaint throttle — see schedule_repaint(). */
    gint64  last_repaint_us;
    guint   pending_repaint_id;
};

G_DEFINE_TYPE (PnOscilloscope, pn_oscilloscope, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_VALUE_KEY,
    PROP_X_KEY,
    PROP_SCREEN_COLOR,
    PROP_TRACE_COLOR,
    PROP_GRID_COLOR,
    PROP_SHOW_GRATICULE,
    PROP_TRACE_WIDTH,
    PROP_X_FROM_ZERO,
    PROP_Y_FROM_ZERO,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Numeric coercion (shared shape with pn-xy-graph.c)                 */
/* ------------------------------------------------------------------ */

/** Parse a numeric string into a double.  Accepts an optional leading
 *  sign, an optional "0x"/"0X" prefix for base-16, and otherwise base-10
 *  with a fractional part. */
static gboolean
parse_numeric_string (
        const gchar *s,
        gdouble     *out)
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

    if (negative)
        v = -v;

    if (!isfinite (v))
        return FALSE;

    *out = v;
    return TRUE;
}

/** Coerce a JsonNode to a finite double.  Returns FALSE for missing /
 *  non-scalar / NaN / infinite values. */
static gboolean
node_to_finite_double (
        JsonNode *node,
        gdouble  *out)
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

    if (!isfinite (v))
        return FALSE;

    *out = v;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnOscilloscope *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

/** Throttled wrapper around pn_node_request_repaint: fires immediately
 *  when the minimum interval has elapsed, otherwise coalesces into a
 *  single one-shot timer for the residual time. */
static void
schedule_repaint (PnOscilloscope *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_OSC_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_OSC_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Afterglow trail                                                    */
/* ------------------------------------------------------------------ */

/** Drop trail entries that have aged past the persistence window.  The
 *  ring is ordered oldest→newest with non-decreasing timestamps, so the
 *  expired ones are always a prefix at the head. */
static void
prune_trail (
        PnOscilloscope *self,
        gint64          now)
{
    while (self->trail_count > 0 &&
           now - self->trail_t[self->trail_head] >= PN_OSC_AFTERGLOW_US)
    {
        self->trail_head = (self->trail_head + 1)
                         % PN_OSCILLOSCOPE_AFTERGLOW_MAX;
        self->trail_count -= 1;
    }
}

/** Append a just-vacated position to the trail ring (overwriting the
 *  oldest entry when full — it is the most faded, so the loss is
 *  invisible). */
static void
trail_push (
        PnOscilloscope *self,
        gdouble         x,
        gdouble         y,
        gint64          now)
{
    guint slot = (self->trail_head + self->trail_count)
               % PN_OSCILLOSCOPE_AFTERGLOW_MAX;

    self->trail_x[slot] = x;
    self->trail_y[slot] = y;
    self->trail_t[slot] = now;

    if (self->trail_count < PN_OSCILLOSCOPE_AFTERGLOW_MAX)
        self->trail_count += 1;
    else
        self->trail_head = (self->trail_head + 1)
                         % PN_OSCILLOSCOPE_AFTERGLOW_MAX;
}

static void
trail_clear (PnOscilloscope *self)
{
    self->trail_head  = 0;
    self->trail_count = 0;
}

/** Repaint tick that animates the glow decay: prune the dead tail, ask
 *  for a redraw, and keep ticking until the trail is empty (then the dot
 *  is static again and message-driven repaints suffice). */
static gboolean
on_fade_tick (gpointer user_data)
{
    PnOscilloscope *self = user_data;

    prune_trail (self, g_get_monotonic_time ());
    pn_node_request_repaint (PN_NODE (self));

    if (self->trail_count == 0)
    {
        self->fade_anim_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/** Start the fade animation if the trail has anything to decay and no
 *  timer is already running. */
static void
ensure_fade_anim (PnOscilloscope *self)
{
    if (self->fade_anim_id == 0 && self->trail_count > 0)
        self->fade_anim_id =
                g_timeout_add (PN_OSC_FADE_INTERVAL_MS, on_fade_tick, self);
}

/* ------------------------------------------------------------------ */
/*  Trace store                                                        */
/* ------------------------------------------------------------------ */

/** Adopt @vy (and optional @vx) as the on-screen waveform snapshot,
 *  discarding any scalar point.  References are taken on the vectors
 *  (they are immutable and shared), so the trace survives the message
 *  that delivered it being unref'd after receive(). */
static void
set_snapshot (
        PnOscilloscope *self,
        PnVector       *vy,
        PnVector       *vx)
{
    g_set_object (&self->vy, vy);

    if (vx != NULL)
        g_set_object (&self->vx, vx);
    else
        g_clear_object (&self->vx);

    /* A waveform snapshot supersedes any scalar point, and the explored
     * scalar frame no longer applies — a later scalar stream reframes.
     * The afterglow trail belongs to the dot, so it goes too. */
    self->have_point  = FALSE;
    self->have_bounds = FALSE;
    trail_clear (self);
}

/** Make @x, @y the single current scalar point, replacing whatever was
 *  on screen — the previous point or a vector snapshot.  No history is
 *  kept, so the scalar trace is always exactly this one dot. */
static void
push_scalar (
        PnOscilloscope *self,
        gdouble         x,
        gdouble         y)
{
    g_clear_object (&self->vy);
    g_clear_object (&self->vx);

    /* If the dot is actually moving, the position it is leaving becomes an
     * afterglow point — a glowing breadcrumb the painter trails a fading
     * connector back to.  A repeated value (no motion) leaves no streak. */
    if (self->have_point &&
        (x != self->last_x || y != self->last_y))
    {
        trail_push (self, self->last_x, self->last_y,
                    g_get_monotonic_time ());
        ensure_fade_anim (self);
    }

    self->last_x     = x;
    self->last_y     = y;
    self->have_point = TRUE;

    /* Grow the explored frame to include this point (it only ever widens
     * until a snapshot resets it). */
    if (!self->have_bounds)
    {
        self->bx_lo = self->bx_hi = x;
        self->by_lo = self->by_hi = y;
        self->have_bounds = TRUE;
    }
    else
    {
        if (x < self->bx_lo) self->bx_lo = x;
        if (x > self->bx_hi) self->bx_hi = x;
        if (y < self->by_lo) self->by_lo = y;
        if (y > self->by_hi) self->by_hi = y;
    }
}

/* The current trace addressed by a single logical index k in display
 * order (0 = leftmost).  Resolves through whichever store is live so the
 * bounds scan and the decimator share one code path; the scalar store is
 * a single point (k is always 0). */

static guint
trace_len (PnOscilloscope *self)
{
    if (self->vy != NULL)
        return (guint) MIN (pn_vector_get_len (self->vy), (gsize) G_MAXUINT);
    return self->have_point ? 1u : 0u;
}

static gdouble
trace_y (PnOscilloscope *self, guint k)
{
    if (self->vy != NULL)
        return pn_vector_get_data (self->vy)[k];
    return self->last_y;
}

static gdouble
trace_x (PnOscilloscope *self, guint k)
{
    if (self->vy != NULL)
    {
        if (self->vx != NULL && k < pn_vector_get_len (self->vx))
            return pn_vector_get_data (self->vx)[k];
        return (gdouble) k;   /* no X vector: plot against the sample index */
    }

    return self->last_x;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_oscilloscope_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnOscilloscope *self = PN_OSCILLOSCOPE (node);
    JsonNode       *vnode;
    JsonNode       *xnode;
    PnVector       *vy;
    gdouble         y;

    if (self->value_key == NULL || *self->value_key == '\0')
        return;

    vnode = pn_message_get_member (message, self->value_key);
    if (vnode == NULL)
        return;

    xnode = (self->x_key != NULL && *self->x_key != '\0')
          ? pn_message_get_member (message, self->x_key)
          : NULL;

    /* Vector value → capture the whole waveform as a snapshot. */
    vy = pn_message_resolve_vector (message, vnode);
    if (vy != NULL)
    {
        PnVector *vx = (xnode != NULL)
                     ? pn_message_resolve_vector (message, xnode)
                     : NULL;
        set_snapshot (self, vy, vx);
        schedule_repaint (self);
        return;
    }

    /* Scalar value → show the single current point (a dot), replacing the
     * previous one.  X is the matching number, or 0 when none is supplied
     * (a lone scalar then rides the centre line, auto-fit centring it). */
    if (node_to_finite_double (vnode, &y))
    {
        gdouble x = 0.0;

        if (xnode != NULL)
            (void) node_to_finite_double (xnode, &x);

        push_scalar (self, x, y);
        schedule_repaint (self);
        return;
    }

    /* Neither a vector nor a number — nothing to trace; stay silent. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_oscilloscope_get_paint_state (
        PnOscilloscope           *self,
        PnOscilloscopePaintState *out)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (out != NULL);

    out->screen_color   = self->screen_color;
    out->trace_color    = self->trace_color;
    out->grid_color     = self->grid_color;
    out->show_graticule = self->show_graticule;
    out->trace_width    = self->trace_width;
    out->x_from_zero    = self->x_from_zero;
    out->y_from_zero    = self->y_from_zero;
}

guint
pn_oscilloscope_read_trace (
        PnOscilloscope *self,
        guint           cap,
        gdouble        *out_x,
        gdouble        *out_y,
        gdouble        *xmin,
        gdouble        *xmax,
        gdouble        *ymin,
        gdouble        *ymax)
{
    guint   n;
    guint   k;
    gdouble lo_x = 0.0, hi_x = 0.0, lo_y = 0.0, hi_y = 0.0;
    guint   wn = 0;

    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), 0);
    g_return_val_if_fail (out_x != NULL && out_y != NULL, 0);
    g_return_val_if_fail (cap > 0, 0);

    n = trace_len (self);
    if (n == 0)
    {
        if (xmin) *xmin = 0.0;
        if (xmax) *xmax = 1.0;
        if (ymin) *ymin = 0.0;
        if (ymax) *ymax = 1.0;
        return 0;
    }

    /* Bounds over every retained sample, so the auto-fit is exact even
     * when the trace is decimated below for drawing.  (For the scalar dot
     * this is overridden by the accumulated envelope after the loop.) */
    for (k = 0; k < n; k++)
    {
        gdouble x = trace_x (self, k);
        gdouble y = trace_y (self, k);

        if (k == 0)
        {
            lo_x = hi_x = x;
            lo_y = hi_y = y;
        }
        else
        {
            if (x < lo_x) lo_x = x;
            if (x > hi_x) hi_x = x;
            if (y < lo_y) lo_y = y;
            if (y > hi_y) hi_y = y;
        }
    }

    if (n <= cap)
    {
        for (k = 0; k < n; k++)
        {
            out_x[wn] = trace_x (self, k);
            out_y[wn] = trace_y (self, k);
            wn += 1;
        }
    }
    else
    {
        /* Decimate to per-bucket extrema: each of @buckets index ranges
         * contributes the sample with the smallest Y and the one with
         * the largest Y, emitted in index order.  Real samples are kept
         * (not averaged), so a dense waveform's envelope and a parametric
         * curve both survive, and the output never exceeds @cap points. */
        guint buckets = cap / 2;
        guint b;

        if (buckets < 1)
            buckets = 1;

        for (b = 0; b < buckets; b++)
        {
            guint   b0 = (guint) ((guint64) b       * n / buckets);
            guint   b1 = (guint) ((guint64) (b + 1) * n / buckets);
            guint   kmin, kmax, ka, kb;
            gdouble ymn, ymx;

            if (b1 <= b0)
                continue;

            kmin = kmax = b0;
            ymn  = ymx  = trace_y (self, b0);
            for (k = b0 + 1; k < b1; k++)
            {
                gdouble yv = trace_y (self, k);
                if (yv < ymn) { ymn = yv; kmin = k; }
                if (yv > ymx) { ymx = yv; kmax = k; }
            }

            ka = MIN (kmin, kmax);
            kb = MAX (kmin, kmax);

            out_x[wn] = trace_x (self, ka);
            out_y[wn] = trace_y (self, ka);
            wn += 1;
            if (kb != ka)
            {
                out_x[wn] = trace_x (self, kb);
                out_y[wn] = trace_y (self, kb);
                wn += 1;
            }
        }
    }

    /* Scalar mode draws only the latest dot, but auto-fitting that single
     * point would centre it forever; frame it by the accumulated extent of
     * every scalar point seen so the dot moves within the explored range. */
    if (self->vy == NULL && self->have_bounds)
    {
        lo_x = self->bx_lo; hi_x = self->bx_hi;
        lo_y = self->by_lo; hi_y = self->by_hi;
    }

    if (xmin) *xmin = lo_x;
    if (xmax) *xmax = hi_x;
    if (ymin) *ymin = lo_y;
    if (ymax) *ymax = hi_y;

    return wn;
}

guint
pn_oscilloscope_read_afterglow (
        PnOscilloscope *self,
        guint           cap,
        gdouble        *out_x,
        gdouble        *out_y,
        gdouble        *out_life)
{
    gint64 now;
    guint  i;
    guint  wn = 0;

    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), 0);
    g_return_val_if_fail (out_x != NULL && out_y != NULL && out_life != NULL,
                          0);

    now = g_get_monotonic_time ();

    for (i = 0; i < self->trail_count && wn < cap; i++)
    {
        guint   idx  = (self->trail_head + i) % PN_OSCILLOSCOPE_AFTERGLOW_MAX;
        gdouble life = 1.0 - (gdouble) (now - self->trail_t[idx])
                             / (gdouble) PN_OSC_AFTERGLOW_US;

        if (life <= 0.0)
            continue;   /* aged out since the last prune — skip it */

        out_x[wn]    = self->trail_x[idx];
        out_y[wn]    = self->trail_y[idx];
        out_life[wn] = life;
        wn += 1;
    }

    return wn;
}

guint
pn_oscilloscope_get_point_count (PnOscilloscope *self)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), 0);
    return self->have_point ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_oscilloscope_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_OSC_WIDTH;
    if (out_height != NULL) *out_height = PN_OSC_TOTAL_HEIGHT;
}

static double
pn_oscilloscope_get_header_height (PnNode *node)
{
    (void) node;
    return PN_OSC_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_oscilloscope_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnOscilloscope *self = PN_OSCILLOSCOPE (object);

    switch (prop_id)
    {
    case PROP_VALUE_KEY:
        g_value_set_string (value, self->value_key);
        break;
    case PROP_X_KEY:
        g_value_set_string (value, self->x_key);
        break;
    case PROP_SCREEN_COLOR:
        g_value_set_boxed (value, &self->screen_color);
        break;
    case PROP_TRACE_COLOR:
        g_value_set_boxed (value, &self->trace_color);
        break;
    case PROP_GRID_COLOR:
        g_value_set_boxed (value, &self->grid_color);
        break;
    case PROP_SHOW_GRATICULE:
        g_value_set_boolean (value, self->show_graticule);
        break;
    case PROP_TRACE_WIDTH:
        g_value_set_uint (value, self->trace_width);
        break;
    case PROP_X_FROM_ZERO:
        g_value_set_boolean (value, self->x_from_zero);
        break;
    case PROP_Y_FROM_ZERO:
        g_value_set_boolean (value, self->y_from_zero);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/** Set @*field to a copy of the string in @value, notifying @pspec when
 *  it actually changed.  Keys feed receive(), so no repaint is forced. */
static void
set_string_prop (
        GObject       *object,
        gchar        **field,
        const GValue  *value,
        GParamSpec    *pspec)
{
    const gchar *s = g_value_get_string (value);
    if (g_strcmp0 (*field, s) != 0)
    {
        g_free (*field);
        *field = g_strdup (s ? s : "");
        g_object_notify_by_pspec (object, pspec);
    }
}

/** Store @value's #PnColor into @field, repainting on a real change. */
static void
set_color_prop (
        PnOscilloscope *self,
        PnColor        *field,
        const GValue   *value,
        GParamSpec     *pspec)
{
    const PnColor *c = g_value_get_boxed (value);
    if (c != NULL && !pn_color_equal (c, field))
    {
        *field = *c;
        g_object_notify_by_pspec (G_OBJECT (self), pspec);
        pn_node_request_repaint (PN_NODE (self));
    }
}

static void
pn_oscilloscope_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnOscilloscope *self = PN_OSCILLOSCOPE (object);

    switch (prop_id)
    {
    case PROP_VALUE_KEY:
        set_string_prop (object, &self->value_key, value,
                         props[PROP_VALUE_KEY]);
        break;
    case PROP_X_KEY:
        set_string_prop (object, &self->x_key, value, props[PROP_X_KEY]);
        break;
    case PROP_SCREEN_COLOR:
        set_color_prop (self, &self->screen_color, value,
                        props[PROP_SCREEN_COLOR]);
        break;
    case PROP_TRACE_COLOR:
        set_color_prop (self, &self->trace_color, value,
                        props[PROP_TRACE_COLOR]);
        break;
    case PROP_GRID_COLOR:
        set_color_prop (self, &self->grid_color, value,
                        props[PROP_GRID_COLOR]);
        break;
    case PROP_SHOW_GRATICULE:
        {
            gboolean g = g_value_get_boolean (value);
            if (self->show_graticule != g)
            {
                self->show_graticule = g;
                g_object_notify_by_pspec (object, props[PROP_SHOW_GRATICULE]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_TRACE_WIDTH:
        {
            guint w = g_value_get_uint (value);
            if (self->trace_width != w)
            {
                self->trace_width = w;
                g_object_notify_by_pspec (object, props[PROP_TRACE_WIDTH]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_X_FROM_ZERO:
        {
            gboolean z = g_value_get_boolean (value);
            if (self->x_from_zero != z)
            {
                self->x_from_zero = z;
                g_object_notify_by_pspec (object, props[PROP_X_FROM_ZERO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_Y_FROM_ZERO:
        {
            gboolean z = g_value_get_boolean (value);
            if (self->y_from_zero != z)
            {
                self->y_from_zero = z;
                g_object_notify_by_pspec (object, props[PROP_Y_FROM_ZERO]);
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
pn_oscilloscope_finalize (GObject *object)
{
    PnOscilloscope *self = PN_OSCILLOSCOPE (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    if (self->fade_anim_id != 0)
    {
        g_source_remove (self->fade_anim_id);
        self->fade_anim_id = 0;
    }

    g_clear_object  (&self->vy);
    g_clear_object  (&self->vx);
    g_clear_pointer (&self->value_key, g_free);
    g_clear_pointer (&self->x_key, g_free);

    G_OBJECT_CLASS (pn_oscilloscope_parent_class)->finalize (object);
}

static void
pn_oscilloscope_class_init (PnOscilloscopeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_oscilloscope_get_property;
    object_class->set_property = pn_oscilloscope_set_property;
    object_class->finalize     = pn_oscilloscope_finalize;

    node_class->receive           = pn_oscilloscope_receive;
    node_class->get_size          = pn_oscilloscope_get_size;
    node_class->get_header_height = pn_oscilloscope_get_header_height;
    /* The cairo CRT painter (paint_plot) is installed onto this class by
     * the gui tier — pn_oscilloscope_gui_install() — so the headless core
     * carries no GTK or cairo. */

    node_class->class_name        = "Oscilloscope";
    node_class->icon              = "\xef\x84\x88";  /* fa-desktop U+F108 */
    node_class->color             = (PnColor){ 0.20, 0.45, 0.30, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_VALUE_KEY] = g_param_spec_string (
            "value-key", "Value key",
            "Data-bag member holding the Y value to trace.  May be a "
            "number or a $pnvector (a captured waveform).",
            "value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_KEY] = g_param_spec_string (
            "x-key", "X key",
            "Data-bag member holding the X value.  May be a number or a "
            "$pnvector.  Optional: when absent, a vector value is plotted "
            "against its sample index and a scalar value's dot sits at "
            "X = 0 (centred).",
            "x",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SCREEN_COLOR] = g_param_spec_boxed (
            "screen-color", "Screen colour",
            "Backdrop of the CRT screen behind the trace",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TRACE_COLOR] = g_param_spec_boxed (
            "trace-color", "Trace colour",
            "Colour of the electron beam / waveform",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GRID_COLOR] = g_param_spec_boxed (
            "grid-color", "Graticule colour",
            "Colour of the division grid etched on the screen",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_GRATICULE] = g_param_spec_boolean (
            "show-graticule", "Show graticule",
            "Draw the division grid across the screen",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TRACE_WIDTH] = g_param_spec_uint (
            "trace-width", "Trace width",
            "Stroke width of the beam, in pixels",
            1, 8, 2,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_FROM_ZERO] = g_param_spec_boolean (
            "x-from-zero", "X starts from 0",
            "Anchor the X axis at zero instead of auto-fitting it tightly "
            "around the data.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_FROM_ZERO] = g_param_spec_boolean (
            "y-from-zero", "Y starts from 0",
            "Anchor the Y axis at zero instead of auto-fitting it tightly "
            "around the data.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema: two themed pages (Appearance | Data),
     * every editor the type-driven default. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Appearance");
        pn_settings_schema_row (schema, "trace-color",    PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "trace-width",    PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "screen-color",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "grid-color",     PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "show-graticule", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Data");
        pn_settings_schema_row (schema, "x-key",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "value-key",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-from-zero", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-from-zero", PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_oscilloscope_init (PnOscilloscope *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  green = { 0.20, 0.45, 0.30, 1.0 };

    self->value_key = g_strdup ("value");
    self->x_key     = g_strdup ("x");

    /* Classic P-series CRT: a near-black green screen, a bright phosphor
     * beam, and a dim green graticule etched across it. */
    self->screen_color   = (PnColor) { 0.02, 0.05, 0.03, 1.0 };
    self->trace_color    = (PnColor) { 0.34, 1.00, 0.45, 1.0 };
    self->grid_color     = (PnColor) { 0.22, 0.50, 0.30, 1.0 };
    self->show_graticule = TRUE;
    self->trace_width    = 2;
    self->x_from_zero    = FALSE;
    self->y_from_zero    = FALSE;

    self->vy = NULL;
    self->vx = NULL;
    self->have_point  = FALSE;
    self->last_x      = 0.0;
    self->last_y      = 0.0;
    self->have_bounds = FALSE;
    self->bx_lo = self->bx_hi = 0.0;
    self->by_lo = self->by_hi = 0.0;

    self->trail_head   = 0;
    self->trail_count  = 0;
    self->fade_anim_id = 0;

    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    pn_node_set_class_name (node, "Oscilloscope");
    pn_node_set_icon       (node, "\xef\x84\x88");  /* fa-desktop U+F108 */
    pn_node_set_color      (node, &green);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnOscilloscope *
pn_oscilloscope_new (void)
{
    return g_object_new (PN_TYPE_OSCILLOSCOPE, NULL);
}
