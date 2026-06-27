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

    /* CRT-tube knobs (maximized view): beam sharpness and brightness, each
     * 0..1.  Pure appearance — they reshape the drawn beam, never the trace
     * store.  Serialized like the rest of the look. */
    gdouble   focus;
    gdouble   intensity;

    /* Manual scale per axis.  When *_auto is TRUE the axis auto-fits the
     * data (honouring *_from_zero) exactly as the scope always has; when
     * FALSE the visible window is [offset - range/2, offset + range/2],
     * driven by the maximized-view knobs.  The values are serialized. */
    gboolean  x_auto;
    gdouble   x_range;
    gdouble   x_offset;
    gboolean  y_auto;
    gdouble   y_range;
    gdouble   y_offset;

    /* Measurement/reference cursors (PnOscCursor): up to two vertical and
     * two horizontal dashed reference lines the user shows from the panel
     * keys and drags across the screen.  A cursor's position is held in DATA
     * units (X for a vertical line, Y for a horizontal one) so it stays glued
     * to the same measured value as the view is panned/zoomed.  Serialized
     * compactly through the single "cursors" string property. */
    gboolean  cursor_on[PN_OSC_CUR_N];
    gdouble   cursor_pos[PN_OSC_CUR_N];

    /* Transient: TRUE only while this card is lifted into the worksheet
     * zoom overlay, gating the on-screen knob panel.  Not serialized. */
    gboolean  maximized;

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
    PROP_FOCUS,
    PROP_INTENSITY,
    PROP_X_AUTO,
    PROP_X_RANGE,
    PROP_X_OFFSET,
    PROP_Y_AUTO,
    PROP_Y_RANGE,
    PROP_Y_OFFSET,
    PROP_CURSORS,
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

/** Raw data extent of the live trace, before any padding or manual
 *  window.  For a vector snapshot this is the exact extent of every
 *  sample; for the scalar dot it is the accumulated envelope of every
 *  point seen since the last snapshot (so a lone dot has a stable frame
 *  to move within).  Returns FALSE when there is no trace yet. */
static gboolean
compute_raw_bounds (
        PnOscilloscope *self,
        gdouble        *xlo,
        gdouble        *xhi,
        gdouble        *ylo,
        gdouble        *yhi)
{
    guint   n = trace_len (self);
    guint   k;
    gdouble lo_x = 0.0, hi_x = 0.0, lo_y = 0.0, hi_y = 0.0;

    if (n == 0)
        return FALSE;

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

    /* Scalar mode draws only the latest dot; frame it by the accumulated
     * extent of every scalar point so the dot moves within the explored
     * range instead of being pinned dead-centre. */
    if (self->vy == NULL && self->have_bounds)
    {
        lo_x = self->bx_lo; hi_x = self->bx_hi;
        lo_y = self->by_lo; hi_y = self->by_hi;
    }

    if (xlo) *xlo = lo_x;
    if (xhi) *xhi = hi_x;
    if (ylo) *ylo = lo_y;
    if (yhi) *yhi = hi_y;
    return TRUE;
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
    out->focus          = self->focus;
    out->intensity      = self->intensity;
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

    /* Bounds over every retained sample (scalar-envelope aware), so the
     * auto-fit is exact even when the trace is decimated below. */
    compute_raw_bounds (self, &lo_x, &hi_x, &lo_y, &hi_y);

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
/*  Maximized controls — knobs (GTK-free)                             */
/*                                                                     */
/*  Geometry, hit-testing and value maths for the four bench-scope     */
/*  knobs and the two "Auto" buttons that appear while the card is      */
/*  maximized.  Kept here, GTK-free, so the painter and the worksheet   */
/*  share one definition (mirrors PnKnob / PnChat in-card controls).    */
/* ------------------------------------------------------------------ */

/* Layout proportions of the knob panel within the maximized rect. */
#define PN_OSC_KNOB_COL_FRAC  0.30
#define PN_OSC_KNOB_COL_MIN   150.0
#define PN_OSC_KNOB_COL_MAX   280.0
#define PN_OSC_BTN_ROW_FRAC   0.18
#define PN_OSC_BTN_ROW_MIN    70.0
#define PN_OSC_BTN_ROW_MAX    130.0
#define PN_OSC_CRT_ASPECT     1.25      /* 10×8 graticule, kept square */

/* Pixels-of-vertical-drag that scale a range by e (one natural-log
 * unit).  ~0.005 → ~139 px per octave, a comfortable knob feel — the
 * COARSE outer ring of the concentric range knob. */
#define PN_OSC_RANGE_DRAG_K   0.005

/* The FINE inner disc of the range knob: a slow vernier at ~1/6 the
 * coarse rate, for trimming the last bit once the coarse ring is close. */
#define PN_OSC_RANGE_FINE_K   (PN_OSC_RANGE_DRAG_K / 6.0)

/* Defaults for the CRT level knobs.  Both rest at the top of travel so a
 * fresh scope draws exactly as it always has (tightest, brightest beam)
 * and the knobs only ever turn the beam DOWN from there. */
#define PN_OSC_FOCUS_DEFAULT      1.0
#define PN_OSC_INTENSITY_DEFAULT  1.0

/** Pad a [lo, hi] range so the data never touches the frame; widen a
 *  degenerate (flat) range to something drawable.  When @from_zero the
 *  range is first anchored at zero.  This is the auto-fit padding that
 *  used to live in the gui tier; the painter mapping and the knob-grab
 *  seeding now share this one copy. */
static void
pad_range (
        gdouble  *lo,
        gdouble  *hi,
        gboolean  from_zero)
{
    if (from_zero)
    {
        if (*lo > 0.0) *lo = 0.0;
        if (*hi < 0.0) *hi = 0.0;
    }

    if (*hi - *lo < 1e-9)
    {
        gdouble pad = (fabs (*hi) > 1e-9) ? fabs (*hi) * 0.05 : 0.5;
        *lo -= pad;
        *hi += pad;
    }
    else
    {
        gdouble pad = (*hi - *lo) * 0.05;
        *lo -= pad;
        *hi += pad;
    }
}

void
pn_oscilloscope_set_maximized (PnOscilloscope *self, gboolean on)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    /* Pure state — NO repaint: this is toggled around every overlay paint,
     * so requesting one here would spin the frame clock forever. */
    self->maximized = on;
}

gboolean
pn_oscilloscope_get_maximized (PnOscilloscope *self)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), FALSE);
    return self->maximized;
}

void
pn_oscilloscope_axis_window (
        PnOscilloscope *self,
        gboolean        x_axis,
        gdouble         raw_lo,
        gdouble         raw_hi,
        gdouble        *lo,
        gdouble        *hi)
{
    gboolean is_auto;
    gdouble  out_lo, out_hi;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));

    is_auto = x_axis ? self->x_auto : self->y_auto;

    if (!is_auto)
    {
        gdouble c = x_axis ? self->x_offset : self->y_offset;
        gdouble r = x_axis ? self->x_range  : self->y_range;

        if (!isfinite (r) || r <= 0.0)
            r = 1.0;                       /* guard a degenerate window */

        out_lo = c - r * 0.5;
        out_hi = c + r * 0.5;
    }
    else
    {
        out_lo = raw_lo;
        out_hi = raw_hi;
        pad_range (&out_lo, &out_hi,
                   x_axis ? self->x_from_zero : self->y_from_zero);
    }

    if (lo) *lo = out_lo;
    if (hi) *hi = out_hi;
}

void
pn_oscilloscope_axis_knob_values (
        PnOscilloscope *self,
        gboolean        x_axis,
        gdouble         raw_lo,
        gdouble         raw_hi,
        gdouble        *range,
        gdouble        *offset)
{
    gdouble lo, hi;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));

    pn_oscilloscope_axis_window (self, x_axis, raw_lo, raw_hi, &lo, &hi);

    if (range)  *range  = hi - lo;
    if (offset) *offset = (lo + hi) * 0.5;
}

gboolean
pn_oscilloscope_axis_is_auto (PnOscilloscope *self, gboolean x_axis)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), TRUE);
    return x_axis ? self->x_auto : self->y_auto;
}

void
pn_oscilloscope_layout (
        PnOscilloscope *self,
        gdouble         px,
        gdouble         py,
        gdouble         pw,
        gdouble         ph,
        PnOscRect      *crt,
        PnOscRect       knobs[PN_OSC_KNOB_N],
        PnOscRect       autobtn[2],
        PnOscRect       curbtn[PN_OSC_CUR_N])
{
    gdouble col_w, row_h, avail_w, avail_h;
    gdouble crt_w, crt_h;
    gdouble kx, kw, cell_w, cell_h;
    gdouble bx, by, bh, btn_w, btn_h;
    int     i;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));

    /* Inset everything by a margin so the CRT and the controls sit inside a
     * border of bare panel face, not flush against the moulded edge. */
    {
        gdouble margin = CLAMP (MIN (pw, ph) * 0.04, 10.0, 32.0);

        px += margin;
        py += margin;
        pw -= 2.0 * margin;
        ph -= 2.0 * margin;
    }

    col_w = CLAMP (pw * PN_OSC_KNOB_COL_FRAC,
                   PN_OSC_KNOB_COL_MIN, PN_OSC_KNOB_COL_MAX);
    row_h = CLAMP (ph * PN_OSC_BTN_ROW_FRAC,
                   PN_OSC_BTN_ROW_MIN, PN_OSC_BTN_ROW_MAX);

    /* Never let the panels eat the whole face on a small overlay. */
    if (col_w > pw * 0.6) col_w = pw * 0.6;
    if (row_h > ph * 0.5) row_h = ph * 0.5;

    avail_w = pw - col_w;
    avail_h = ph - row_h;

    /* CRT fitted to the graticule aspect inside the top-left area. */
    if (avail_w / avail_h > PN_OSC_CRT_ASPECT)
    {
        crt_h = avail_h;
        crt_w = crt_h * PN_OSC_CRT_ASPECT;
    }
    else
    {
        crt_w = avail_w;
        crt_h = crt_w / PN_OSC_CRT_ASPECT;
    }

    if (crt != NULL)
    {
        crt->x = px;
        crt->y = py;
        crt->w = crt_w;
        crt->h = crt_h;
    }

    /* Right column → 2×3 knob grid.  Order matches PnOscKnob:
     *   (col0,row0) X range   (col1,row0) X offset
     *   (col0,row1) Y range   (col1,row1) Y offset
     *   (col0,row2) Focus     (col1,row2) Intensity                    */
    kx     = px + avail_w;
    kw     = col_w;
    /* Knobs sit on a tighter pitch than the full column so the cluster reads
     * as a group with breathing room left over (room for more knobs later):
     * the columns are pulled together and the three rows are packed toward the
     * top, leaving bare panel at the right and bottom of the knob area. */
    cell_w = kw * 0.42;
    cell_h = (avail_h / 3.0) * 0.80;

    if (knobs != NULL)
    {
        for (i = 0; i < PN_OSC_KNOB_N; i++)
        {
            int col = i % 2;
            int row = i / 2;

            knobs[i].x = kx + col * cell_w;
            knobs[i].y = py + row * cell_h;
            knobs[i].w = cell_w;
            knobs[i].h = cell_h;
        }
    }

    /* Bottom strip → two wide "Auto" keys under the CRT, stacked one above
     * the other.  Each key is a flat landscape rectangle (~4.5× as wide as
     * it is tall); its "Auto X" / "Auto Y" legend is painted ABOVE the key,
     * so the clickable rect is just the key. */
    bx    = px;
    by    = py + avail_h;
    bh    = row_h;
    btn_h = CLAMP (bh * 0.22, 12.0, 18.0);
    btn_w = btn_h * 4.5 * 0.75;           /* a landscape key, ~75% of full width */

    if (autobtn != NULL)
    {
        gdouble gap     = 14.0;           /* panel edge → LED bar           */
        gdouble led_w   = btn_w * 0.21;   /* the LED bar left of each key    */
        gdouble led_gap = 6.0;            /* LED bar → key                  */
        gdouble keyx    = bx + gap + led_w + led_gap;
        gdouble lbl     = 13.0;           /* legend room above each key      */
        gdouble gapv    = 5.0;            /* gap between the two keys        */
        gdouble slot    = lbl + btn_h;    /* one key plus its legend         */
        gdouble stack   = slot * 2.0 + gapv;
        gdouble y0      = by + MAX (0.0, (bh - stack) * 0.5);

        autobtn[0].x = keyx;
        autobtn[0].y = y0 + lbl;
        autobtn[0].w = btn_w;
        autobtn[0].h = btn_h;

        autobtn[1].x = keyx;
        autobtn[1].y = y0 + slot + gapv + lbl;
        autobtn[1].w = btn_w;
        autobtn[1].h = btn_h;
    }

    /* Cursor toggle keys → a 2×2 block in the bottom strip, to the RIGHT of
     * the Auto keys: V1 V2 on the top row, H1 H2 below, under one "CURSORS"
     * legend.  Each key is a small square-ish pushbutton. */
    if (curbtn != NULL)
    {
        gdouble gap     = 14.0;            /* same panel edge → LED bar as Auto */
        gdouble led_w   = btn_w * 0.21;
        gdouble led_gap = 6.0;
        gdouble keyx    = bx + gap + led_w + led_gap;   /* Auto-key left edge   */
        gdouble cw      = btn_h * 2.0;     /* fits "V1"                         */
        gdouble chh     = btn_h;
        gdouble cgx     = 8.0;             /* column gap                        */
        gdouble cgy     = 6.0;             /* row gap                           */
        gdouble clbl    = 14.0;            /* "CURSORS" legend room above       */
        gdouble cblockx = keyx + btn_w + 40.0;          /* clear of the Auto keys */
        gdouble cstack  = clbl + 2.0 * chh + cgy;
        gdouble cy0     = by + MAX (0.0, (bh - cstack) * 0.5);
        int     ci;

        for (ci = 0; ci < PN_OSC_CUR_N; ci++)
        {
            int ccol = ci % 2;
            int crow = ci / 2;

            curbtn[ci].x = cblockx + ccol * (cw + cgx);
            curbtn[ci].y = cy0 + clbl + crow * (chh + cgy);
            curbtn[ci].w = cw;
            curbtn[ci].h = chh;
        }
    }
}

/* The graticule/plot sub-rectangle inside a CRT rect.  Mirrors the bezel
 * border + inner margin the painter (paint_crt) insets by, so a cursor maps
 * to exactly the screen position it is drawn at. */
void
pn_oscilloscope_plot_rect (const PnOscRect *crt, PnOscRect *plot)
{
    gdouble smin, m;

    g_return_if_fail (crt != NULL && plot != NULL);

    smin = MIN (crt->w, crt->h);
    m    = smin * 0.030 + smin * 0.045;   /* bezel border + inner margin */

    plot->x = crt->x + m;
    plot->y = crt->y + m;
    plot->w = crt->w - 2.0 * m;
    plot->h = crt->h - 2.0 * m;
}

static int
hit_rect_array (
        const PnOscRect *rects,
        int              n,
        gdouble          x,
        gdouble          y)
{
    int i;

    if (rects == NULL)
        return -1;

    for (i = 0; i < n; i++)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
            y >= rects[i].y && y < rects[i].y + rects[i].h)
            return i;

    return -1;
}

int
pn_oscilloscope_hit_knob (
        const PnOscRect knobs[PN_OSC_KNOB_N],
        gdouble x, gdouble y)
{
    return hit_rect_array (knobs, PN_OSC_KNOB_N, x, y);
}

int
pn_oscilloscope_hit_auto (
        const PnOscRect autobtn[2],
        gdouble x, gdouble y)
{
    return hit_rect_array (autobtn, 2, x, y);
}

/* ------------------------------------------------------------------ */
/*  Measurement cursors                                                */
/* ------------------------------------------------------------------ */

gboolean
pn_oscilloscope_cursor_is_vertical (int cursor)
{
    return cursor == PN_OSC_CUR_V1 || cursor == PN_OSC_CUR_V2;
}

gboolean
pn_oscilloscope_cursor_is_on (PnOscilloscope *self, int cursor)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), FALSE);
    g_return_val_if_fail (cursor >= 0 && cursor < PN_OSC_CUR_N, FALSE);
    return self->cursor_on[cursor];
}

gdouble
pn_oscilloscope_cursor_pos (PnOscilloscope *self, int cursor)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), 0.0);
    g_return_val_if_fail (cursor >= 0 && cursor < PN_OSC_CUR_N, 0.0);
    return self->cursor_pos[cursor];
}

/* The visible data window for an axis right now, reading the live extent —
 * the same window the painter maps the trace into, so a cursor's data
 * position and its drawn screen position always agree. */
static void
cursor_axis_window (
        PnOscilloscope *self,
        gboolean        x_axis,
        gdouble        *lo,
        gdouble        *hi)
{
    gdouble sx[2], sy[2], xmin, xmax, ymin, ymax;

    pn_oscilloscope_read_trace (self, 2, sx, sy, &xmin, &xmax, &ymin, &ymax);
    if (x_axis)
        pn_oscilloscope_axis_window (self, TRUE, xmin, xmax, lo, hi);
    else
        pn_oscilloscope_axis_window (self, FALSE, ymin, ymax, lo, hi);
}

void
pn_oscilloscope_toggle_cursor (PnOscilloscope *self, int cursor)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (cursor >= 0 && cursor < PN_OSC_CUR_N);

    if (self->cursor_on[cursor])
    {
        self->cursor_on[cursor] = FALSE;
    }
    else
    {
        gboolean vert = pn_oscilloscope_cursor_is_vertical (cursor);
        gdouble  lo, hi;

        /* Seed the new line at the centre of the current view so it appears
         * mid-screen, ready to be dragged onto a feature. */
        cursor_axis_window (self, vert, &lo, &hi);
        self->cursor_on[cursor]  = TRUE;
        self->cursor_pos[cursor] = 0.5 * (lo + hi);
    }

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CURSORS]);
    pn_node_request_repaint (PN_NODE (self));
}

int
pn_oscilloscope_hit_cursor_button (
        const PnOscRect curbtn[PN_OSC_CUR_N],
        gdouble x, gdouble y)
{
    return hit_rect_array (curbtn, PN_OSC_CUR_N, x, y);
}

int
pn_oscilloscope_hit_cursor (
        PnOscilloscope  *self,
        const PnOscRect *crt,
        gdouble x, gdouble y)
{
    const double TOL = 6.0;
    PnOscRect    plot;
    gdouble      x0, x1, y0, y1;
    double       best_d = TOL;
    int          best   = -1;
    int          c;

    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), -1);
    g_return_val_if_fail (crt != NULL, -1);

    pn_oscilloscope_plot_rect (crt, &plot);
    if (plot.w < 1.0 || plot.h < 1.0)
        return -1;

    cursor_axis_window (self, TRUE,  &x0, &x1);
    cursor_axis_window (self, FALSE, &y0, &y1);

    for (c = 0; c < PN_OSC_CUR_N; c++)
    {
        double d;

        if (!self->cursor_on[c])
            continue;

        if (pn_oscilloscope_cursor_is_vertical (c))
        {
            double span = x1 - x0;
            double sxp;

            if (!(fabs (span) > 0.0))
                continue;
            sxp = plot.x + (self->cursor_pos[c] - x0) / span * plot.w;
            if (y < plot.y - TOL || y > plot.y + plot.h + TOL)
                continue;
            d = fabs (x - sxp);
        }
        else
        {
            double span = y1 - y0;
            double syp;

            if (!(fabs (span) > 0.0))
                continue;
            syp = plot.y + plot.h - (self->cursor_pos[c] - y0) / span * plot.h;
            if (x < plot.x - TOL || x > plot.x + plot.w + TOL)
                continue;
            d = fabs (y - syp);
        }

        if (d <= best_d)
        {
            best_d = d;
            best   = c;
        }
    }

    return best;
}

void
pn_oscilloscope_drag_cursor_to (
        PnOscilloscope  *self,
        int              cursor,
        const PnOscRect *crt,
        gdouble x, gdouble y)
{
    PnOscRect plot;
    gdouble   lo, hi;
    gboolean  vert;
    double    f;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (cursor >= 0 && cursor < PN_OSC_CUR_N);
    g_return_if_fail (crt != NULL);

    pn_oscilloscope_plot_rect (crt, &plot);
    if (plot.w < 1.0 || plot.h < 1.0)
        return;

    vert = pn_oscilloscope_cursor_is_vertical (cursor);
    cursor_axis_window (self, vert, &lo, &hi);

    f = vert ? (x - plot.x) / plot.w
             : (plot.y + plot.h - y) / plot.h;
    f = CLAMP (f, 0.0, 1.0);

    self->cursor_pos[cursor] = lo + f * (hi - lo);
    self->cursor_on[cursor]  = TRUE;

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CURSORS]);
    pn_node_request_repaint (PN_NODE (self));
}

/* ---- compact serialization of all four cursors into one string ----
 * Four ';'-separated tokens (V1;V2;H1;H2): an empty token is "off"; a number
 * is the data position of a shown cursor.  Locale-independent (g_ascii_*). */

static gchar *
cursors_to_string (PnOscilloscope *self)
{
    GString *s = g_string_new (NULL);
    int      c;

    for (c = 0; c < PN_OSC_CUR_N; c++)
    {
        if (c > 0)
            g_string_append_c (s, ';');
        if (self->cursor_on[c])
        {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];

            g_string_append (s, g_ascii_dtostr (buf, sizeof buf,
                                                self->cursor_pos[c]));
        }
    }

    return g_string_free (s, FALSE);
}

static void
cursors_from_string (PnOscilloscope *self, const char *str)
{
    gchar **toks;
    int     c;

    for (c = 0; c < PN_OSC_CUR_N; c++)
    {
        self->cursor_on[c]  = FALSE;
        self->cursor_pos[c] = 0.0;
    }

    if (str == NULL || *str == '\0')
        return;

    toks = g_strsplit (str, ";", -1);
    for (c = 0; c < PN_OSC_CUR_N && toks[c] != NULL; c++)
    {
        if (toks[c][0] == '\0')
            continue;
        self->cursor_on[c]  = TRUE;
        self->cursor_pos[c] = g_ascii_strtod (toks[c], NULL);
    }
    g_strfreev (toks);
}

gboolean
pn_oscilloscope_knob_is_concentric (int knob)
{
    return knob == PN_OSC_KNOB_X_RANGE || knob == PN_OSC_KNOB_Y_RANGE;
}

gboolean
pn_oscilloscope_knob_is_level (int knob)
{
    return knob == PN_OSC_KNOB_FOCUS || knob == PN_OSC_KNOB_INTENSITY;
}

gdouble
pn_oscilloscope_knob_level (PnOscilloscope *self, int knob)
{
    g_return_val_if_fail (PN_IS_OSCILLOSCOPE (self), 0.0);

    if (knob == PN_OSC_KNOB_FOCUS)     return self->focus;
    if (knob == PN_OSC_KNOB_INTENSITY) return self->intensity;
    return 0.0;
}

void
pn_oscilloscope_knob_dial (
        const PnOscRect *cell,
        gdouble *cx, gdouble *cy, gdouble *radius)
{
    gdouble label_h, dial_cy, r;

    g_return_if_fail (cell != NULL);

    /* Must match draw_knob in the gui tier exactly. */
    label_h = MIN (cell->h * 0.20, 16.0);
    dial_cy = cell->y + label_h + 4.0
            + (cell->h - 2.0 * label_h - 8.0) * 0.5;
    r       = MIN (cell->w, cell->h - 2.0 * label_h) * 0.36;

    if (cx)     *cx     = cell->x + cell->w * 0.5;
    if (cy)     *cy     = dial_cy;
    if (radius) *radius = r;
}

gboolean
pn_oscilloscope_hit_knob_fine (
        const PnOscRect knobs[PN_OSC_KNOB_N],
        int knob, gdouble x, gdouble y)
{
    gdouble cx, cy, r, dx, dy, inner;

    if (knobs == NULL || knob < 0 || knob >= PN_OSC_KNOB_N)
        return FALSE;
    if (!pn_oscilloscope_knob_is_concentric (knob))
        return FALSE;

    pn_oscilloscope_knob_dial (&knobs[knob], &cx, &cy, &r);
    dx    = x - cx;
    dy    = y - cy;
    inner = r * PN_OSC_FINE_RADIUS_FRAC;

    return (dx * dx + dy * dy) <= inner * inner;
}

/* Raw data extent of one axis, with a sane 0..1 fallback when there is
 * no trace yet, so a knob can still be grabbed on an empty screen. */
static void
axis_raw_bounds (
        PnOscilloscope *self,
        gboolean        x_axis,
        gdouble        *raw_lo,
        gdouble        *raw_hi)
{
    gdouble xlo, xhi, ylo, yhi;

    if (compute_raw_bounds (self, &xlo, &xhi, &ylo, &yhi))
    {
        *raw_lo = x_axis ? xlo : ylo;
        *raw_hi = x_axis ? xhi : yhi;
    }
    else
    {
        *raw_lo = 0.0;
        *raw_hi = 1.0;
    }
}

void
pn_oscilloscope_begin_knob (PnOscilloscope *self, int knob)
{
    gboolean x_axis;
    gboolean is_auto;
    gdouble  raw_lo, raw_hi, range, offset;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (knob >= 0 && knob < PN_OSC_KNOB_N);

    if (pn_oscilloscope_knob_is_level (knob))
        return;                            /* Focus / Intensity: no axis */

    x_axis  = (knob == PN_OSC_KNOB_X_RANGE || knob == PN_OSC_KNOB_X_OFFSET);
    is_auto = x_axis ? self->x_auto : self->y_auto;
    if (!is_auto)
        return;                            /* already manual — no jump */

    axis_raw_bounds (self, x_axis, &raw_lo, &raw_hi);
    pn_oscilloscope_axis_knob_values (self, x_axis, raw_lo, raw_hi,
                                      &range, &offset);

    if (x_axis)
    {
        self->x_range  = range;
        self->x_offset = offset;
        self->x_auto   = FALSE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_X_RANGE]);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_X_OFFSET]);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_X_AUTO]);
    }
    else
    {
        self->y_range  = range;
        self->y_offset = offset;
        self->y_auto   = FALSE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_Y_RANGE]);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_Y_OFFSET]);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_Y_AUTO]);
    }

    pn_node_request_repaint (PN_NODE (self));
}

void
pn_oscilloscope_drag_knob (
        PnOscilloscope *self,
        int             knob,
        gboolean        fine,
        gdouble         dy,
        gdouble         crt_extent)
{
    gboolean x_axis;
    gboolean is_range;

    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (knob >= 0 && knob < PN_OSC_KNOB_N);

    if (!isfinite (dy) || dy == 0.0)
        return;

    if (!isfinite (crt_extent) || crt_extent <= 0.0)
        crt_extent = 1.0;

    /* Level knobs (Focus / Intensity): move the 0..1 value directly, one
     * CRT extent of UP drag (dy < 0) sweeping the whole range upward. */
    if (pn_oscilloscope_knob_is_level (knob))
    {
        gdouble *vp = (knob == PN_OSC_KNOB_FOCUS) ? &self->focus
                                                  : &self->intensity;
        gdouble  nv = CLAMP (*vp - dy / crt_extent, 0.0, 1.0);

        if (nv != *vp)
        {
            *vp = nv;
            g_object_notify_by_pspec (G_OBJECT (self),
                    props[(knob == PN_OSC_KNOB_FOCUS) ? PROP_FOCUS
                                                      : PROP_INTENSITY]);
            pn_node_request_repaint (PN_NODE (self));
        }
        return;
    }

    x_axis   = (knob == PN_OSC_KNOB_X_RANGE || knob == PN_OSC_KNOB_X_OFFSET);
    is_range = (knob == PN_OSC_KNOB_X_RANGE || knob == PN_OSC_KNOB_Y_RANGE);

    /* Safety: grab from the live window if this axis was never seeded. */
    pn_oscilloscope_begin_knob (self, knob);

    if (is_range)
    {
        /* Multiplicative: dragging UP (dy < 0) shrinks the range → zoom
         * in; dragging down widens it → zoom out.  The fine inner disc
         * turns the same way, just far more slowly (a vernier). */
        gdouble  k  = fine ? PN_OSC_RANGE_FINE_K : PN_OSC_RANGE_DRAG_K;
        gdouble *rp = x_axis ? &self->x_range : &self->y_range;
        gdouble  nr = *rp * exp (dy * k);

        if (!isfinite (nr))
            return;
        nr = CLAMP (nr, 1e-12, 1e12);
        *rp = nr;

        g_object_notify_by_pspec (G_OBJECT (self),
                props[x_axis ? PROP_X_RANGE : PROP_Y_RANGE]);
    }
    else
    {
        /* Additive, scaled so one CRT extent of drag pans one screenful.
         * Dragging UP (dy < 0) lowers the offset → the trace moves up
         * (Y) / right (X), i.e. the content follows the drag. */
        gdouble *op = x_axis ? &self->x_offset : &self->y_offset;
        gdouble  r  = x_axis ? self->x_range   : self->y_range;

        *op += (dy / crt_extent) * r;

        g_object_notify_by_pspec (G_OBJECT (self),
                props[x_axis ? PROP_X_OFFSET : PROP_Y_OFFSET]);
    }

    pn_node_request_repaint (PN_NODE (self));
}

void
pn_oscilloscope_reset_axis (PnOscilloscope *self, gboolean x_axis)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));

    if (x_axis)
    {
        if (self->x_auto)
            return;
        self->x_auto = TRUE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_X_AUTO]);
    }
    else
    {
        if (self->y_auto)
            return;
        self->y_auto = TRUE;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_Y_AUTO]);
    }

    pn_node_request_repaint (PN_NODE (self));
}

void
pn_oscilloscope_toggle_axis_auto (PnOscilloscope *self, gboolean x_axis)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));

    if (pn_oscilloscope_axis_is_auto (self, x_axis))
    {
        /* ON → OFF: pin the axis at its current auto-fitted window so the
         * trace stays put, then drop out of auto.  begin_knob already does
         * exactly this seeding (and is a no-op if somehow already manual). */
        pn_oscilloscope_begin_knob (self,
                x_axis ? PN_OSC_KNOB_X_RANGE : PN_OSC_KNOB_Y_RANGE);
    }
    else
    {
        /* OFF → ON: back to auto-fit. */
        pn_oscilloscope_reset_axis (self, x_axis);
    }
}

void
pn_oscilloscope_reset_knob (PnOscilloscope *self, int knob)
{
    g_return_if_fail (PN_IS_OSCILLOSCOPE (self));
    g_return_if_fail (knob >= 0 && knob < PN_OSC_KNOB_N);

    if (knob == PN_OSC_KNOB_FOCUS)
    {
        if (self->focus != PN_OSC_FOCUS_DEFAULT)
        {
            self->focus = PN_OSC_FOCUS_DEFAULT;
            g_object_notify_by_pspec (G_OBJECT (self), props[PROP_FOCUS]);
            pn_node_request_repaint (PN_NODE (self));
        }
    }
    else if (knob == PN_OSC_KNOB_INTENSITY)
    {
        if (self->intensity != PN_OSC_INTENSITY_DEFAULT)
        {
            self->intensity = PN_OSC_INTENSITY_DEFAULT;
            g_object_notify_by_pspec (G_OBJECT (self), props[PROP_INTENSITY]);
            pn_node_request_repaint (PN_NODE (self));
        }
    }
    else
    {
        gboolean x_axis = (knob == PN_OSC_KNOB_X_RANGE ||
                           knob == PN_OSC_KNOB_X_OFFSET);
        pn_oscilloscope_reset_axis (self, x_axis);
    }
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
    case PROP_FOCUS:
        g_value_set_double (value, self->focus);
        break;
    case PROP_INTENSITY:
        g_value_set_double (value, self->intensity);
        break;
    case PROP_X_AUTO:
        g_value_set_boolean (value, self->x_auto);
        break;
    case PROP_X_RANGE:
        g_value_set_double (value, self->x_range);
        break;
    case PROP_X_OFFSET:
        g_value_set_double (value, self->x_offset);
        break;
    case PROP_Y_AUTO:
        g_value_set_boolean (value, self->y_auto);
        break;
    case PROP_Y_RANGE:
        g_value_set_double (value, self->y_range);
        break;
    case PROP_Y_OFFSET:
        g_value_set_double (value, self->y_offset);
        break;
    case PROP_CURSORS:
        g_value_take_string (value, cursors_to_string (self));
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
    case PROP_FOCUS:
        {
            gdouble f = CLAMP (g_value_get_double (value), 0.0, 1.0);
            if (self->focus != f)
            {
                self->focus = f;
                g_object_notify_by_pspec (object, props[PROP_FOCUS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_INTENSITY:
        {
            gdouble v = CLAMP (g_value_get_double (value), 0.0, 1.0);
            if (self->intensity != v)
            {
                self->intensity = v;
                g_object_notify_by_pspec (object, props[PROP_INTENSITY]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_X_AUTO:
        {
            gboolean a = g_value_get_boolean (value);
            if (self->x_auto != a)
            {
                self->x_auto = a;
                g_object_notify_by_pspec (object, props[PROP_X_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_Y_AUTO:
        {
            gboolean a = g_value_get_boolean (value);
            if (self->y_auto != a)
            {
                self->y_auto = a;
                g_object_notify_by_pspec (object, props[PROP_Y_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_X_RANGE:
        {
            gdouble r = g_value_get_double (value);
            if (self->x_range != r || self->x_auto)
            {
                self->x_range = r;
                self->x_auto  = FALSE;
                g_object_notify_by_pspec (object, props[PROP_X_RANGE]);
                g_object_notify_by_pspec (object, props[PROP_X_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_X_OFFSET:
        {
            gdouble o = g_value_get_double (value);
            if (self->x_offset != o || self->x_auto)
            {
                self->x_offset = o;
                self->x_auto   = FALSE;
                g_object_notify_by_pspec (object, props[PROP_X_OFFSET]);
                g_object_notify_by_pspec (object, props[PROP_X_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_Y_RANGE:
        {
            gdouble r = g_value_get_double (value);
            if (self->y_range != r || self->y_auto)
            {
                self->y_range = r;
                self->y_auto  = FALSE;
                g_object_notify_by_pspec (object, props[PROP_Y_RANGE]);
                g_object_notify_by_pspec (object, props[PROP_Y_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_Y_OFFSET:
        {
            gdouble o = g_value_get_double (value);
            if (self->y_offset != o || self->y_auto)
            {
                self->y_offset = o;
                self->y_auto   = FALSE;
                g_object_notify_by_pspec (object, props[PROP_Y_OFFSET]);
                g_object_notify_by_pspec (object, props[PROP_Y_AUTO]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_CURSORS:
        cursors_from_string (self, g_value_get_string (value));
        pn_node_request_repaint (PN_NODE (self));
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
            "around the data.  Only applies while the Y axis is in auto mode.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FOCUS] = g_param_spec_double (
            "focus", "Focus",
            "Beam sharpness, 0..1 (1 = tightest).  Lower values defocus the "
            "trace into a wider, softer beam.  The maximized-view Focus knob.",
            0.0, 1.0, PN_OSC_FOCUS_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INTENSITY] = g_param_spec_double (
            "intensity", "Intensity",
            "Beam brightness, 0..1 (1 = brightest).  Lower values dim the "
            "trace and its phosphor glow.  The maximized-view Intensity knob.",
            0.0, 1.0, PN_OSC_INTENSITY_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_AUTO] = g_param_spec_boolean (
            "x-auto", "X auto-fit",
            "Auto-fit the X axis to the data.  When off, the X window is "
            "x-offset ± x-range/2 (set by the X knobs).",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_RANGE] = g_param_spec_double (
            "x-range", "X range",
            "Width of the visible X window in data units (manual mode).  "
            "Setting it switches the X axis to manual.",
            0.0, G_MAXDOUBLE, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_OFFSET] = g_param_spec_double (
            "x-offset", "X offset",
            "Data value at the centre of the screen along X (manual mode).  "
            "Setting it switches the X axis to manual.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_AUTO] = g_param_spec_boolean (
            "y-auto", "Y auto-fit",
            "Auto-fit the Y axis to the data.  When off, the Y window is "
            "y-offset ± y-range/2 (set by the Y knobs).",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_RANGE] = g_param_spec_double (
            "y-range", "Y range",
            "Height of the visible Y window in data units (manual mode).  "
            "Setting it switches the Y axis to manual.",
            0.0, G_MAXDOUBLE, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_OFFSET] = g_param_spec_double (
            "y-offset", "Y offset",
            "Data value at the centre of the screen along Y (manual mode).  "
            "Setting it switches the Y axis to manual.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_CURSORS] = g_param_spec_string (
            "cursors", "Cursors",
            "Measurement-cursor state: four ';'-separated tokens (V1;V2;H1;H2), "
            "each empty (hidden) or the cursor's data position.  Driven by the "
            "panel keys and drags, not edited directly.",
            "",
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
        pn_settings_schema_row (schema, "focus",          PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "intensity",      PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Data");
        pn_settings_schema_row (schema, "x-key",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "value-key",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-from-zero", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-from-zero", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Scale");
        pn_settings_schema_row (schema, "x-auto",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-range",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-offset", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-auto",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-range",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-offset", PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        /* Cursor state persists but is driven by the panel keys/drags, never
         * the settings form. */
        pn_settings_schema_row       (schema, "cursors", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "cursors", PN_ROW_FLAG_HIDDEN);

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
    self->focus          = PN_OSC_FOCUS_DEFAULT;
    self->intensity      = PN_OSC_INTENSITY_DEFAULT;

    self->x_auto   = TRUE;
    self->x_range  = 1.0;
    self->x_offset = 0.0;
    self->y_auto   = TRUE;
    self->y_range  = 1.0;
    self->y_offset = 0.0;
    self->maximized = FALSE;

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
