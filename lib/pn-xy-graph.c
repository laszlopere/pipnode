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

#include "pn-xy-graph.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Same footprint as #PnGraph: a canonical 40 px header twice as wide */
/*  as a normal node, then a small gap, then the plot rectangle.       */
/* ------------------------------------------------------------------ */

#define PN_XY_GRAPH_WIDTH         280.0
#define PN_XY_GRAPH_HEADER_HEIGHT  40.0
#define PN_XY_GRAPH_GAP             4.0
#define PN_XY_GRAPH_PLOT_HEIGHT   173.0
#define PN_XY_GRAPH_TOTAL_HEIGHT  (PN_XY_GRAPH_HEADER_HEIGHT + \
                                   PN_XY_GRAPH_GAP +           \
                                   PN_XY_GRAPH_PLOT_HEIGHT)

/* PN_XY_GRAPH_SAMPLES (per-series ring capacity) is published in
 * pn-xy-graph.h — the gui-tier painter sizes its scratch arrays to it —
 * so it is not re-defined here. */

/* Default "last N points" draw window: the full ring, so by default
 * every retained sample is plotted. */
#define PN_XY_GRAPH_DEF_POINTS  PN_XY_GRAPH_SAMPLES

/* Maximum repaint rate, expressed as the minimum gap between two
 * worksheet redraw requests originating from this node.  100 ms
 * (10 Hz) is well below the perception threshold for a streaming plot
 * and stops a high-frequency feed from forcing a full plplot re-render
 * every display frame.  Mirrors #PnGraph. */
#define PN_XY_GRAPH_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

/* Maximum number of distinct topics tracked as separate series.  Beyond
 * this, further topics are dropped (silently) so a runaway feed that
 * synthesises a new topic per message can't grow the hashtable unbounded. */
#define PN_XY_GRAPH_MAX_SERIES  12

/* Sentinel topic used when a message arrives without one. */
#define PN_XY_GRAPH_DEFAULT_TOPIC  ""

/* PnXYGraphSample, PnXYGraphSeries and PnXYGraphSeriesView are published
 * in pn-xy-graph.h: they are plain data (no GTK / no PLplot) and the
 * gui-tier painter has to walk the very same rings receive() fills, so
 * the structs cross the tier boundary through the public header. */

struct _PnXYGraph
{
    PnNode parent_instance;

    gchar  *key;     /* JSON path to the Y value */
    gchar  *x_key;   /* JSON path to the X value */
    guint   max_points;

    PnXYGraphStyle  style;

    /* Appearance — these only affect how the plot is drawn; flipping any
     * of them schedules a repaint but never touches the rings. */
    PnColor    line_color;
    PnColor    axis_color;
    PnColor    background_color;
    guint      line_width;
    gboolean   show_grid;
    gboolean   x_from_zero;
    gboolean   y_from_zero;

    /* topic (gchar *) -> PnXYGraphSeries *.  Owns both keys and values. */
    GHashTable *series;
    guint       next_arrival_idx;

    /* Repaint throttle — see schedule_repaint(). */
    gint64  last_repaint_us;
    guint   pending_repaint_id;

    /* The per-instance PLplot stream is owned by the gui tier
     * (pn-xy-graph-gui.c) — allocated lazily on the first paint and
     * stashed on this GObject with a destroy-notify — so the GTK-free
     * core never references PLplot. */
};

G_DEFINE_TYPE (PnXYGraph, pn_xy_graph, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_KEY,
    PROP_X_KEY,
    PROP_MAX_POINTS,
    PROP_DRAW_STYLE,
    PROP_LINE_COLOR,
    PROP_LINE_WIDTH,
    PROP_AXIS_COLOR,
    PROP_BACKGROUND_COLOR,
    PROP_SHOW_GRID,
    PROP_X_FROM_ZERO,
    PROP_Y_FROM_ZERO,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Draw-style enum boxed type                                         */
/* ------------------------------------------------------------------ */

GType
pn_xy_graph_style_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_XY_GRAPH_STYLE_POINTS, "PN_XY_GRAPH_STYLE_POINTS", "Points" },
            { PN_XY_GRAPH_STYLE_LINES,  "PN_XY_GRAPH_STYLE_LINES",  "Lines"  },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnXYGraphStyle", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Series management                                                  */
/* ------------------------------------------------------------------ */

static PnXYGraphSeries *
series_new (guint arrival_idx)
{
    PnXYGraphSeries *s = g_new0 (PnXYGraphSeries, 1);
    s->arrival_idx = arrival_idx;
    return s;
}

static void
series_free (gpointer data)
{
    g_free (data);
}

/** Lookup an existing series for @topic, or create one on first sight.
 *  Returns NULL once PN_XY_GRAPH_MAX_SERIES distinct topics have been
 *  seen — the message is silently dropped in that case. */
static PnXYGraphSeries *
series_get_or_create (
        PnXYGraph   *self,
        const gchar *topic)
{
    PnXYGraphSeries *s;
    const gchar     *key = topic != NULL ? topic : PN_XY_GRAPH_DEFAULT_TOPIC;

    s = g_hash_table_lookup (self->series, key);
    if (s != NULL)
        return s;

    if (g_hash_table_size (self->series) >= PN_XY_GRAPH_MAX_SERIES)
        return NULL;

    s = series_new (self->next_arrival_idx++);
    g_hash_table_insert (self->series, g_strdup (key), s);
    return s;
}

static gint
series_view_cmp (
        gconstpointer a,
        gconstpointer b)
{
    const PnXYGraphSeriesView *va = a;
    const PnXYGraphSeriesView *vb = b;
    if (va->series->arrival_idx < vb->series->arrival_idx) return -1;
    if (va->series->arrival_idx > vb->series->arrival_idx) return  1;
    return 0;
}

PnXYGraphSeriesView *
pn_xy_graph_collect_series_sorted (
        PnXYGraph *self,
        guint     *out_n)
{
    GHashTableIter       it;
    gpointer             k, v;
    guint                n = g_hash_table_size (self->series);
    PnXYGraphSeriesView *out;
    guint                i = 0;

    if (n == 0)
    {
        if (out_n) *out_n = 0;
        return NULL;
    }

    out = g_new0 (PnXYGraphSeriesView, n);
    g_hash_table_iter_init (&it, self->series);
    while (g_hash_table_iter_next (&it, &k, &v))
    {
        out[i].topic  = (const gchar *)           k;
        out[i].series = (const PnXYGraphSeries *)  v;
        i += 1;
    }

    g_qsort_with_data (out, n, sizeof (*out),
                       (GCompareDataFunc) series_view_cmp, NULL);

    if (out_n) *out_n = n;
    return out;
}

guint
pn_xy_graph_series_chrono (
        const PnXYGraphSeries *s,
        guint                  limit,
        gdouble               *out_x,
        gdouble               *out_y)
{
    guint take;
    guint oldest;
    guint k;

    g_return_val_if_fail (s != NULL, 0);

    take = s->sample_count;
    if (limit > 0 && take > limit)
        take = limit;
    if (take == 0)
        return 0;

    /* The ring is filled head-first; sample_head points one past the
     * newest.  Walk back @take entries to the oldest we keep, then read
     * forward in arrival order. */
    oldest = (s->sample_head + PN_XY_GRAPH_SAMPLES - take) % PN_XY_GRAPH_SAMPLES;

    for (k = 0; k < take; k++)
    {
        const PnXYGraphSample *p =
            &s->samples[(oldest + k) % PN_XY_GRAPH_SAMPLES];
        out_x[k] = p->x;
        out_y[k] = p->y;
    }

    return take;
}

/* ------------------------------------------------------------------ */
/*  Numeric coercion (shared shape with pn-graph.c)                    */
/* ------------------------------------------------------------------ */

/** Parse a numeric string into a double.  Accepts an optional leading
 *  sign, an optional "0x"/"0X" prefix for base-16, and otherwise falls
 *  back to base-10 with a fractional part. */
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

/** Resolve @path against @message and coerce the addressed node to a
 *  finite double.  Returns FALSE on empty path / missing / non-numeric. */
static gboolean
resolve_double (
        PnMessage   *message,
        const gchar *path,
        gdouble     *out)
{
    JsonObject *root;
    JsonNode   *node;
    gboolean    ok;

    if (path == NULL || *path == '\0')
        return FALSE;

    root = pn_json_lookup_root_for_message (message);
    node = pn_json_resolve_path (root, path);
    ok   = node_to_finite_double (node, out);
    json_object_unref (root);

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnXYGraph *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

/** Throttled wrapper around pn_node_request_repaint.  Emits immediately
 *  if more than PN_XY_GRAPH_MIN_REPAINT_INTERVAL_US has elapsed since
 *  the last paint request; otherwise arms a single one-shot timer for
 *  the residual time and coalesces every intervening request. */
static void
schedule_repaint (PnXYGraph *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_XY_GRAPH_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us =
            PN_XY_GRAPH_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_xy_graph_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnXYGraph       *self = PN_XY_GRAPH (node);
    gdouble          x, y;
    const gchar     *topic;
    PnXYGraphSeries *series;

    /* Both coordinates have to resolve to a finite number, or the
     * sample is dropped — there is no meaningful point otherwise. */
    if (!resolve_double (message, self->x_key, &x))
        return;
    if (!resolve_double (message, self->key, &y))
        return;

    topic  = pn_message_get_topic (message);
    series = series_get_or_create (self, topic);
    if (series == NULL)
        return;  /* PN_XY_GRAPH_MAX_SERIES cap hit; drop silently. */

    series->samples[series->sample_head].x = x;
    series->samples[series->sample_head].y = y;
    series->sample_head =
        (series->sample_head + 1) % PN_XY_GRAPH_SAMPLES;
    if (series->sample_count < PN_XY_GRAPH_SAMPLES)
        series->sample_count += 1;

    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_xy_graph_get_paint_state (PnXYGraph *self, PnXYGraphPaintState *out)
{
    g_return_if_fail (PN_IS_XY_GRAPH (self));
    g_return_if_fail (out != NULL);

    out->style            = self->style;
    out->max_points       = self->max_points;

    out->line_color       = self->line_color;
    out->axis_color       = self->axis_color;
    out->background_color  = self->background_color;
    out->line_width        = self->line_width;
    out->show_grid         = self->show_grid;
    out->x_from_zero       = self->x_from_zero;
    out->y_from_zero       = self->y_from_zero;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_xy_graph_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_XY_GRAPH_WIDTH;
    if (out_height != NULL) *out_height = PN_XY_GRAPH_TOTAL_HEIGHT;
}

static double
pn_xy_graph_get_header_height (PnNode *node)
{
    (void) node;
    return PN_XY_GRAPH_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_xy_graph_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnXYGraph *self = PN_XY_GRAPH (object);

    switch (prop_id)
    {
    case PROP_KEY:
        g_value_set_string (value, self->key);
        break;
    case PROP_X_KEY:
        g_value_set_string (value, self->x_key);
        break;
    case PROP_MAX_POINTS:
        g_value_set_uint (value, self->max_points);
        break;
    case PROP_DRAW_STYLE:
        g_value_set_enum (value, self->style);
        break;
    case PROP_LINE_COLOR:
        g_value_set_boxed (value, &self->line_color);
        break;
    case PROP_LINE_WIDTH:
        g_value_set_uint (value, self->line_width);
        break;
    case PROP_AXIS_COLOR:
        g_value_set_boxed (value, &self->axis_color);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_SHOW_GRID:
        g_value_set_boolean (value, self->show_grid);
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

/** Set @*field to a copy of the string in @value, notifying @pspec and
 *  scheduling a repaint when it actually changed. */
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

static void
pn_xy_graph_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnXYGraph *self = PN_XY_GRAPH (object);

    switch (prop_id)
    {
    case PROP_KEY:
        set_string_prop (object, &self->key, value, props[PROP_KEY]);
        break;
    case PROP_X_KEY:
        set_string_prop (object, &self->x_key, value, props[PROP_X_KEY]);
        break;
    case PROP_MAX_POINTS:
        {
            guint p = g_value_get_uint (value);
            if (p != self->max_points)
            {
                self->max_points = p;
                g_object_notify_by_pspec (object, props[PROP_MAX_POINTS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_DRAW_STYLE:
        {
            PnXYGraphStyle st = g_value_get_enum (value);
            if (self->style != st)
            {
                self->style = st;
                g_object_notify_by_pspec (object, props[PROP_DRAW_STYLE]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_LINE_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->line_color))
            {
                self->line_color = *c;
                g_object_notify_by_pspec (object, props[PROP_LINE_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_LINE_WIDTH:
        {
            guint w = g_value_get_uint (value);
            if (self->line_width != w)
            {
                self->line_width = w;
                g_object_notify_by_pspec (object, props[PROP_LINE_WIDTH]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_AXIS_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->axis_color))
            {
                self->axis_color = *c;
                g_object_notify_by_pspec (object, props[PROP_AXIS_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BACKGROUND_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->background_color))
            {
                self->background_color = *c;
                g_object_notify_by_pspec (object,
                                          props[PROP_BACKGROUND_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_SHOW_GRID:
        {
            gboolean g = g_value_get_boolean (value);
            if (self->show_grid != g)
            {
                self->show_grid = g;
                g_object_notify_by_pspec (object, props[PROP_SHOW_GRID]);
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
pn_xy_graph_finalize (GObject *object)
{
    PnXYGraph *self = PN_XY_GRAPH (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    /* The PLplot stream (if one was ever allocated) is torn down by its
     * GObject-data destroy-notify in the gui tier. */

    g_clear_pointer (&self->series, g_hash_table_unref);
    g_clear_pointer (&self->key,    g_free);
    g_clear_pointer (&self->x_key,  g_free);

    G_OBJECT_CLASS (pn_xy_graph_parent_class)->finalize (object);
}

static void
pn_xy_graph_class_init (PnXYGraphClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_xy_graph_get_property;
    object_class->set_property = pn_xy_graph_set_property;
    object_class->finalize     = pn_xy_graph_finalize;

    node_class->receive           = pn_xy_graph_receive;
    node_class->get_size          = pn_xy_graph_get_size;
    node_class->get_header_height = pn_xy_graph_get_header_height;
    /* The PLplot/cairo plot painter (paint_plot) is installed onto this
     * class by the gui tier — pn_xy_graph_gui_install() in
     * pn-xy-graph-gui.c — so the headless core carries no GTK/PLplot. */

    node_class->class_name        = "XY Graph";
    node_class->icon              = "\xef\x88\x81";  /* fa-line-chart U+F201 */
    node_class->color             = (PnColor){ 0.30, 0.68, 0.66, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_KEY] = g_param_spec_string (
            "key", "Y key",
            "JSON path (\"/\"-separated, e.g. data/value) to the "
            "numeric value plotted on the Y axis",
            "data/value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_KEY] = g_param_spec_string (
            "x-key", "X key",
            "JSON path (\"/\"-separated, e.g. data/x) to the numeric "
            "value plotted on the X axis.  Each message must carry both "
            "an X and a Y value for a point to be plotted.",
            "data/x",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_POINTS] = g_param_spec_uint (
            "max-points", "Max points",
            "How many of the most recent (x, y) samples to keep and plot "
            "per series.  Older samples scroll off the end of the ring.",
            2, PN_XY_GRAPH_SAMPLES, PN_XY_GRAPH_DEF_POINTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DRAW_STYLE] = g_param_spec_enum (
            "draw-style", "Draw as",
            "How each series is drawn: Points (a discrete marker per "
            "sample) or Lines (a polyline connecting the samples in "
            "arrival order).",
            PN_TYPE_XY_GRAPH_STYLE,
            PN_XY_GRAPH_STYLE_POINTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_COLOR] = g_param_spec_boxed (
            "line-color", "Series colour",
            "Colour of the first series.  When messages arrive on more "
            "than one topic, subsequent series walk the hue wheel in "
            "golden-angle steps from this base.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_WIDTH] = g_param_spec_uint (
            "line-width", "Line / point width",
            "Stroke width of the polyline (Lines) or marker size scale "
            "(Points), in pixels",
            1, 8, 2,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_AXIS_COLOR] = g_param_spec_boxed (
            "axis-color", "Axis colour",
            "Colour of the plot frame, ticks, and tick labels",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the plot rectangle behind the data",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_GRID] = g_param_spec_boolean (
            "show-grid", "Show grid",
            "Draw a thin grid through every major tick on both axes",
            FALSE,
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
     * every editor the type-driven default (PN_EDITOR_AUTO). */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Appearance");
        pn_settings_schema_row (schema, "draw-style",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "line-color",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "line-width",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "axis-color",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "background-color", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "show-grid",        PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Data");
        pn_settings_schema_row (schema, "x-key",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "key",         PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "max-points",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-from-zero", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-from-zero", PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_xy_graph_init (PnXYGraph *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.30, 0.68, 0.66, 1.0 };

    self->key        = g_strdup ("data/value");
    self->x_key      = g_strdup ("data/x");
    self->max_points = PN_XY_GRAPH_DEF_POINTS;
    self->style      = PN_XY_GRAPH_STYLE_POINTS;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    self->line_color       = (PnColor) {  30.0/255.0,  60.0/255.0, 140.0/255.0, 1.0 };
    self->axis_color       = (PnColor) {  70.0/255.0,  70.0/255.0,  70.0/255.0, 1.0 };
    self->background_color = (PnColor) { 1.0, 1.0, 1.0, 1.0 };
    self->line_width       = 2;
    self->show_grid        = FALSE;
    self->x_from_zero      = FALSE;
    self->y_from_zero      = FALSE;

    self->series = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, series_free);
    self->next_arrival_idx = 0;

    pn_node_set_class_name (node, "XY Graph");
    pn_node_set_icon       (node, "\xef\x88\x81");  /* fa-line-chart U+F201 */
    pn_node_set_color (node, &teal);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnXYGraph *
pn_xy_graph_new (void)
{
    return g_object_new (PN_TYPE_XY_GRAPH, NULL);
}

guint
pn_xy_graph_get_series_count (PnXYGraph *self)
{
    g_return_val_if_fail (PN_IS_XY_GRAPH (self), 0);
    return (self->series != NULL) ? g_hash_table_size (self->series) : 0;
}
