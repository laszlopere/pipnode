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
/*  PnPlot — GTK-free core (TODO #48).                                 */
/*                                                                     */
/*  A vector-fed sibling of #PnGraph.  PnGraph time-bins scalar values  */
/*  by their wall-clock arrival; PnPlot instead takes a whole $pnvector */
/*  on data.value in one message (the #PnOscilloscope snapshot path)    */
/*  and distributes its N elements across M consecutive X-buckets       */
/*  (M = the "x-buckets" property).  Each bucket aggregates its run of   */
/*  elements into the same running totals PnGraph keeps per time bin     */
/*  (count / sum / sum_sq / min / max), filling the identical            */
/*  #PnGraphBin ring, so the gui tier reuses PnGraph's PLplot draw path  */
/*  unchanged and every draw-style renders exactly as it does for        */
/*  PnGraph.  The whole dataset lands in one message, so the plot — and  */
/*  in particular the Error-bars box-and-whisker glyphs — fills          */
/*  instantly and deterministically.                                     */
/*                                                                       */
/*  This file owns the type, properties, the receive() contract and the  */
/*  vector→bucket binning; the PLplot/cairo painter lives in             */
/*  pn-plot-gui.c and reads the rings back through the GTK-free seam      */
/*  declared in the header (shared with PnGraph via pn-graph.h).         */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-plot.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-vector.h"

/* ------------------------------------------------------------------ */
/*  Geometry — identical to PnGraph so the two siblings read alike.     */
/* ------------------------------------------------------------------ */

#define PN_PLOT_WIDTH         280.0
#define PN_PLOT_HEADER_HEIGHT  40.0
#define PN_PLOT_GAP             4.0
#define PN_PLOT_PLOT_HEIGHT   173.0
#define PN_PLOT_TOTAL_HEIGHT  (PN_PLOT_HEADER_HEIGHT + \
                               PN_PLOT_GAP +           \
                               PN_PLOT_PLOT_HEIGHT)

/* Default X-bucket count.  Far smaller than PnGraph's 200: a vector of a
 * few hundred samples spread over ~16 buckets puts a genuinely fat run of
 * elements in each, so the Error-bars spread (sd box, min/max whisker) is
 * substantial.  Pushing the count up toward the vector length collapses to
 * ~one sample per bucket and re-exercises the neighbour-reaching whisker
 * path (TODO #48.2). */
#define PN_PLOT_DEF_BINS  16

/* Maximum repaint rate — the minimum gap between two worksheet redraw
 * requests from this node.  Mirrors PnGraph: a snapshot arrives whole, but
 * a fast feed of snapshots must not force a full plplot re-render every
 * display frame. */
#define PN_PLOT_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

struct _PnPlot
{
    PnNode parent_instance;

    gchar       *value_key;   /* data-bag member holding the vector */
    guint        n_bins;      /* active X buckets (M, <= PN_GRAPH_MAX_BINS) */
    PnGraphView  view;        /* time-series (buckets) vs distribution */
    PnGraphStyle style;       /* points / lines / bars / error bars */

    /* Appearance — only affects how the plot is drawn; flipping any of
     * these schedules a repaint but never touches the rings. */
    PnColor   line_color;
    PnColor   axis_color;
    PnColor   background_color;
    guint     line_width;
    gboolean  show_grid;
    gboolean  log_y;
    gboolean  y_from_zero;

    /* The most recent vector, kept by reference so changing the bucket
     * count (or the value key, on the next message) re-bins the same data
     * without waiting for a fresh feed.  NULL until the first vector. */
    PnVector *vec;

    /* The single series whose bucket ring + raw-sample ring the painter
     * walks.  Allocated lazily on the first vector; @have_data tracks
     * whether it currently holds anything. */
    PnGraphSeries *series;
    gboolean       have_data;

    /* Repaint throttle — see schedule_repaint(). */
    gint64  last_repaint_us;
    guint   pending_repaint_id;
};

G_DEFINE_TYPE (PnPlot, pn_plot, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_VALUE_KEY,
    PROP_X_BUCKETS,
    PROP_DATA_VIEW,
    PROP_DRAW_STYLE,
    PROP_LINE_COLOR,
    PROP_LINE_WIDTH,
    PROP_AXIS_COLOR,
    PROP_BACKGROUND_COLOR,
    PROP_SHOW_GRID,
    PROP_LOG_Y,
    PROP_Y_FROM_ZERO,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Repaint throttle (mirrors PnGraph / PnOscilloscope)                */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnPlot *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnPlot *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_PLOT_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_PLOT_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Vector → buckets                                                   */
/* ------------------------------------------------------------------ */

/** Wipe @s back to the empty state: every bucket marked unused and the
 *  raw-sample ring drained.  Called before re-binning a fresh vector. */
static void
series_reset (PnGraphSeries *s)
{
    guint i;

    for (i = 0; i < PN_GRAPH_MAX_BINS; i++)
        s->ring[i].epoch = G_MININT64;

    s->sample_head  = 0;
    s->sample_count = 0;
}

/** Distribute @vec's N elements across the active M = @self->n_bins
 *  consecutive buckets and aggregate each run into the bucket's running
 *  totals (TODO #48.2).  Bucket b owns the half-open element range
 *  [b·N/M, (b+1)·N/M); when M > N the trailing buckets get an empty range
 *  and stay marked unused (epoch == G_MININT64) so the painter skips them,
 *  exactly as a never-touched PnGraph time slot is skipped.
 *
 *  The bucket index doubles as the bin "epoch": bucket b is stored in
 *  slot b with epoch b, so the painter — handed cur_epoch = M-1 and
 *  oldest_valid = 0 — reads them back through PnGraph's ring walker
 *  unchanged.  The raw vector is also mirrored into the per-series sample
 *  ring (stamped with a sentinel "always in window" time) so the
 *  Distribution view can value-bin it through PnGraph's histogram
 *  painter. */
static void
rebucket (PnPlot *self)
{
    const gdouble *data;
    gsize          n;
    guint          m = self->n_bins;
    guint          b;
    guint          fill;

    series_reset (self->series);
    self->have_data = FALSE;

    if (self->vec == NULL)
        return;

    n    = pn_vector_get_len (self->vec);
    data = pn_vector_get_data (self->vec);
    if (n == 0 || data == NULL)
        return;

    for (b = 0; b < m; b++)
    {
        /* Half-open element range for this bucket, in 64-bit so the
         * products do not overflow for large vectors. */
        guint64     lo = (guint64) b       * (guint64) n / (guint64) m;
        guint64     hi = (guint64) (b + 1) * (guint64) n / (guint64) m;
        PnGraphBin *bin = &self->series->ring[b];
        guint64     k;

        if (hi <= lo)
            continue;   /* M > N: empty bucket, leave it unused */

        bin->epoch  = (gint64) b;
        bin->count  = 0;
        bin->sum    = 0.0;
        bin->sum_sq = 0.0;
        bin->min    = data[lo];
        bin->max    = data[lo];

        for (k = lo; k < hi; k++)
        {
            gdouble v = data[k];

            if (v < bin->min) bin->min = v;
            if (v > bin->max) bin->max = v;
            bin->sum    += v;
            bin->sum_sq += v * v;
            bin->count  += 1;
        }
    }

    /* Mirror the raw elements into the sample ring for the Distribution
     * view.  A vector longer than the ring keeps its leading
     * PN_GRAPH_SAMPLES values (the same cap PnGraph's live feed hits).
     * The sentinel timestamp is never older than any paint-time cutoff,
     * so every retained element is always counted — the distribution is
     * of the whole vector, not a time slice. */
    fill = (guint) MIN (n, (gsize) PN_GRAPH_SAMPLES);
    for (b = 0; b < fill; b++)
    {
        self->series->samples[b].time_us = G_MAXINT64;
        self->series->samples[b].value   = data[b];
    }
    self->series->sample_count = fill;
    self->series->sample_head  = (fill < PN_GRAPH_SAMPLES) ? fill : 0;

    self->have_data = TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_plot_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnPlot   *self = PN_PLOT (node);
    JsonNode *vnode;
    PnVector *vy;

    if (self->value_key == NULL || *self->value_key == '\0')
        return;

    vnode = pn_message_get_member (message, self->value_key);
    if (vnode == NULL)
        return;

    /* Only a vector value is a dataset; a bare scalar carries no spread to
     * bucket, so it is ignored (use a PnGraph for scalar time-series). */
    vy = pn_message_resolve_vector (message, vnode);
    if (vy == NULL)
        return;

    /* Adopt the vector (resolve_vector borrows it; g_set_object takes our
     * own ref) so a later bucket-count change can re-bin the same data,
     * then distribute it across the buckets. */
    g_set_object (&self->vec, vy);
    rebucket (self);

    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  GUI / test read seam (GTK-free)                                    */
/* ------------------------------------------------------------------ */

void
pn_plot_get_paint_state (PnPlot *self, PnGraphPaintState *out)
{
    g_return_if_fail (PN_IS_PLOT (self));
    g_return_if_fail (out != NULL);

    /* The resolution field only feeds PnGraph's time-axis labelling and
     * distribution cutoff; PnPlot's painter labels the X axis by bucket
     * index and its samples never age out, so the value is immaterial —
     * pin it to a stable default. */
    out->resolution       = PN_GRAPH_RES_MINUTE;
    out->n_bins           = self->n_bins;
    out->view             = self->view;
    out->style            = self->style;

    out->line_color       = self->line_color;
    out->axis_color       = self->axis_color;
    out->background_color = self->background_color;
    out->line_width       = self->line_width;
    out->show_grid        = self->show_grid;
    out->log_y            = self->log_y;
    out->y_from_zero      = self->y_from_zero;
}

const PnGraphSeries *
pn_plot_peek_series (PnPlot *self)
{
    g_return_val_if_fail (PN_IS_PLOT (self), NULL);
    return self->have_data ? self->series : NULL;
}

guint
pn_plot_get_n_bins (PnPlot *self)
{
    g_return_val_if_fail (PN_IS_PLOT (self), 0);
    return self->n_bins;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_plot_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_PLOT_WIDTH;
    if (out_height != NULL) *out_height = PN_PLOT_TOTAL_HEIGHT;
}

static double
pn_plot_get_header_height (PnNode *node)
{
    (void) node;
    return PN_PLOT_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_plot_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPlot *self = PN_PLOT (object);

    switch (prop_id)
    {
    case PROP_VALUE_KEY:
        g_value_set_string (value, self->value_key);
        break;
    case PROP_X_BUCKETS:
        g_value_set_uint (value, self->n_bins);
        break;
    case PROP_DATA_VIEW:
        g_value_set_enum (value, self->view);
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
    case PROP_LOG_Y:
        g_value_set_boolean (value, self->log_y);
        break;
    case PROP_Y_FROM_ZERO:
        g_value_set_boolean (value, self->y_from_zero);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_plot_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPlot *self = PN_PLOT (object);

    switch (prop_id)
    {
    case PROP_VALUE_KEY:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->value_key, s) != 0)
            {
                g_free (self->value_key);
                self->value_key = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_VALUE_KEY]);
                /* Keys feed receive(); the current buckets stand until the
                 * next message under the new key, so no repaint here. */
            }
        }
        break;
    case PROP_X_BUCKETS:
        {
            guint b = g_value_get_uint (value);
            if (b != self->n_bins)
            {
                self->n_bins = b;
                /* Re-distribute the held vector across the new bucket
                 * count immediately so the change shows without waiting
                 * for a fresh feed. */
                rebucket (self);
                g_object_notify_by_pspec (object, props[PROP_X_BUCKETS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_DATA_VIEW:
        {
            PnGraphView v = g_value_get_enum (value);
            if (self->view != v)
            {
                self->view = v;
                g_object_notify_by_pspec (object, props[PROP_DATA_VIEW]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_DRAW_STYLE:
        {
            PnGraphStyle st = g_value_get_enum (value);
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
    case PROP_LOG_Y:
        {
            gboolean l = g_value_get_boolean (value);
            if (self->log_y != l)
            {
                self->log_y = l;
                g_object_notify_by_pspec (object, props[PROP_LOG_Y]);
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
pn_plot_finalize (GObject *object)
{
    PnPlot *self = PN_PLOT (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    /* The PLplot stream (if one was ever allocated) is torn down by its
     * GObject-data destroy-notify in the gui tier — the core never calls
     * plend1. */

    g_clear_object  (&self->vec);
    g_clear_pointer (&self->series, g_free);
    g_clear_pointer (&self->value_key, g_free);

    G_OBJECT_CLASS (pn_plot_parent_class)->finalize (object);
}

static void
pn_plot_class_init (PnPlotClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_plot_get_property;
    object_class->set_property = pn_plot_set_property;
    object_class->finalize     = pn_plot_finalize;

    node_class->receive           = pn_plot_receive;
    node_class->get_size          = pn_plot_get_size;
    node_class->get_header_height = pn_plot_get_header_height;
    /* The PLplot/cairo plot painter (paint_plot) is installed onto this
     * class by the gui tier — pn_plot_gui_install() in pn-plot-gui.c —
     * so the headless core carries no GTK/cairo/PLplot. */

    node_class->class_name        = "Plot";
    node_class->icon              = "\xef\x88\x81";  /* fa-line-chart U+F201 */
    node_class->color             = (PnColor){ 0.85, 0.52, 0.24, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_VALUE_KEY] = g_param_spec_string (
            "value-key", "Value key",
            "Data-bag member holding the vector to plot.  A $pnvector "
            "(e.g. the whole dataset emitted by a Ramp on data.value) is "
            "the entire dataset for one message; scalar values are "
            "ignored.",
            "value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_BUCKETS] = g_param_spec_uint (
            "x-buckets", "X buckets",
            "Number of buckets the vector is distributed across along the "
            "X axis.  The N vector elements are split into this many "
            "consecutive runs; each bucket aggregates its run into a real "
            "count, mean and spread, and the time-series view draws one "
            "point — or one error-bar box-and-whisker — per bucket.  Fewer "
            "buckets fold more elements together (a fuller spread per error "
            "bar); setting the count near the vector length collapses to "
            "about one element per bucket.  Does not affect the "
            "distribution view, which bins by value.",
            2, PN_GRAPH_MAX_BINS, PN_PLOT_DEF_BINS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DATA_VIEW] = g_param_spec_enum (
            "data-view", "Data view",
            "What the plot represents.  Time series draws one glyph per "
            "X-bucket (X axis is the bucket index); Distribution bins the "
            "vector's elements by value and plots how often each value "
            "occurs (X axis is value, Y axis is the count).",
            PN_TYPE_GRAPH_VIEW,
            PN_GRAPH_VIEW_TIME_SERIES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DRAW_STYLE] = g_param_spec_enum (
            "draw-style", "Draw as",
            "How the data view is drawn: Points (a marker per bucket), "
            "Lines (a polyline through the bucket means), Bars (a filled "
            "bar per bucket), or Error bars (a box-and-whisker per bucket — "
            "whiskers to the bucket min/max and a box centred on the mean "
            "spanning ±1 standard deviation; each bucket is green when "
            "its mean rose versus the previous bucket and red when it fell).",
            PN_TYPE_GRAPH_STYLE,
            PN_GRAPH_STYLE_ERROR_BARS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_COLOR] = g_param_spec_boxed (
            "line-color", "Line colour",
            "Colour of the plotted data — the polyline, the point markers, "
            "or the bar fill, depending on the draw-style.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_WIDTH] = g_param_spec_uint (
            "line-width", "Line width",
            "Stroke width of the data polyline, in pixels",
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

    props[PROP_LOG_Y] = g_param_spec_boolean (
            "log-y", "Logarithmic Y axis",
            "Plot the Y axis on a base-10 logarithmic scale.  Honoured in "
            "the distribution view, useful for long-tailed distributions.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_FROM_ZERO] = g_param_spec_boolean (
            "y-from-zero", "Y starts from 0",
            "Anchor the Y axis at zero instead of auto-fitting it tightly "
            "around the data.  Honoured in the time-series view on a "
            "linear axis.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema: two themed pages (Appearance | Data),
     * every editor the type-driven default — mirrors PnGraph. */
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
        pn_settings_schema_row (schema, "value-key",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-buckets",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "data-view",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "log-y",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-from-zero", PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_plot_init (PnPlot *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  orange = { 0.85, 0.52, 0.24, 1.0 };

    self->value_key = g_strdup ("value");
    self->n_bins    = PN_PLOT_DEF_BINS;
    self->view      = PN_GRAPH_VIEW_TIME_SERIES;
    self->style     = PN_GRAPH_STYLE_ERROR_BARS;

    /* Match PnGraph's default palette so the two siblings read alike. */
    self->line_color       = (PnColor) {  30.0/255.0,  60.0/255.0, 140.0/255.0, 1.0 };
    self->axis_color       = (PnColor) {  70.0/255.0,  70.0/255.0,  70.0/255.0, 1.0 };
    self->background_color = (PnColor) { 1.0, 1.0, 1.0, 1.0 };
    self->line_width       = 2;
    self->show_grid        = FALSE;
    self->log_y            = FALSE;
    self->y_from_zero      = FALSE;

    self->vec       = NULL;
    self->series    = g_new0 (PnGraphSeries, 1);
    self->have_data = FALSE;
    series_reset (self->series);

    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    pn_node_set_class_name (node, "Plot");
    pn_node_set_icon       (node, "\xef\x88\x81");  /* fa-line-chart U+F201 */
    pn_node_set_color      (node, &orange);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPlot *
pn_plot_new (void)
{
    return g_object_new (PN_TYPE_PLOT, NULL);
}
