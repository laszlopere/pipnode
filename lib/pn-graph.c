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

#include "pn-graph.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The header keeps the canonical 40 px height but is twice as wide   */
/*  as a normal node so the plot rectangle below has room for a        */
/*  meaningful curve.  A small gap separates the rounded-rect node     */
/*  body from the white plot rectangle so they read as two visual     */
/*  elements.                                                          */
/* ------------------------------------------------------------------ */

#define PN_GRAPH_WIDTH         280.0
#define PN_GRAPH_HEADER_HEIGHT  40.0
#define PN_GRAPH_GAP             4.0
#define PN_GRAPH_PLOT_HEIGHT   173.0
#define PN_GRAPH_TOTAL_HEIGHT  (PN_GRAPH_HEADER_HEIGHT + \
                                PN_GRAPH_GAP +           \
                                PN_GRAPH_PLOT_HEIGHT)

/* PN_GRAPH_MAX_BINS (ring capacity) and PN_GRAPH_SAMPLES (raw-sample
 * ring) are published in pn-graph.h — the gui-tier painter sizes its
 * scratch arrays to them — so they are not re-defined here. */

/* Default active X-bucket count — the full ring, matching the value the
 * window was hard-split into before the count became configurable, so
 * worksheets saved without an "x-buckets" property render unchanged. */
#define PN_GRAPH_DEF_BINS  200

/* Maximum repaint rate, expressed as the minimum gap between two
 * worksheet redraw requests originating from this node.  100 ms
 * (10 Hz) is well below the perception threshold for a streaming
 * plot of this size and stops a high-frequency feed (e.g. a
 * busy WebSocket stream pumping out hundreds of messages
 * per second) from forcing a full plplot re-render every display
 * frame. */
#define PN_GRAPH_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

/* Maximum number of distinct topics tracked as separate series.
 * Beyond this, further topics are dropped (silently) so a runaway
 * feed that synthesises a new topic per message can't grow the
 * hashtable unbounded.  Twelve is more than the eye can usefully
 * distinguish along a stacked Z axis on a 280 px plot anyway. */
#define PN_GRAPH_MAX_SERIES  12

/* Sentinel topic used when a message arrives without one.  Stored
 * in the hashtable like any other topic key so the empty-topic case
 * shares the same per-series machinery as the named-topic one. */
#define PN_GRAPH_DEFAULT_TOPIC  ""

/* PnGraphBin, PnGraphSample, PnGraphSeries and PnGraphSeriesView are
 * published in pn-graph.h: they are plain data (no GTK / no PLplot) and
 * the gui-tier painter has to walk the very same rings receive() fills,
 * so the structs cross the tier boundary through the public header. */

struct _PnGraph
{
    PnNode parent_instance;

    gchar             *key;          /* JSON path to the value */
    PnGraphResolution  resolution;   /* X-axis time span */
    guint              n_bins;       /* active X buckets (<= PN_GRAPH_MAX_BINS) */
    PnGraphView        view;         /* time-series vs distribution */
    PnGraphStyle       style;        /* points / lines / bars */

    /* Appearance — these only affect how the plot is drawn; flipping
     * any of them schedules a repaint but never touches the rings. */
    PnColor    line_color;
    PnColor    axis_color;
    PnColor    background_color;
    guint      line_width;
    gboolean   show_grid;
    gboolean   log_y;
    gboolean   y_from_zero;

    /* Write the collected samples into the worksheet on save and read
     * them back on load (see the "save-data" / "saved-data" property
     * pair).  Off by default: a graph is a live view, and most documents
     * should not carry a data set around. */
    gboolean   save_data;

    /* One-shot idle that rebuilds the time-bucket rings from the samples
     * restored by the "saved-data" setter.  Deferred because the bin
     * width depends on "resolution" / "x-buckets", and the loader applies
     * the properties of a node in an unspecified order — by the time the
     * idle runs, every property from the file is in place. */
    guint      restore_id;

    /* topic (gchar *) -> PnGraphSeries *.  Owns both keys and values
     * (the hashtable's value-destroy frees the series struct).  The
     * empty-topic sentinel "" sits alongside named topics so a feed
     * that never sets msg.topic still ends up in exactly one slot. */
    GHashTable    *series;
    guint          next_arrival_idx;

    /* Repaint throttle.  Every receive() updates the rings but only
     * asks the worksheet to repaint at most once per
     * PN_GRAPH_MIN_REPAINT_INTERVAL_US.  If a request lands inside
     * the cooldown a single g_timeout source is armed for the
     * remaining time — that way the most recent message is always
     * reflected within the throttle interval, but a 1000 msg/s storm
     * does not push the worksheet to repaint at the display refresh
     * rate (where each paint re-runs the whole plplot stream-init
     * dance for every graph node on the canvas). */
    gint64  last_repaint_us;
    guint   pending_repaint_id;

    /* Recurring refresh so the time axis scrolls (and aged-out samples
     * drop) even when no new messages arrive — the plot's "now" edge is
     * read at paint time, so without this the graph would freeze between
     * messages and only redraw when something else dirtied its area.
     * Runs at the bin-width cadence while there is data; stops when the
     * graph empties out. */
    guint   tick_id;

    /* The per-instance PLplot stream is owned by the gui tier
     * (pn-graph-gui.c) — allocated lazily on the first paint and stashed
     * on this GObject with a destroy-notify — so the GTK-free core never
     * references PLplot. */
};

G_DEFINE_TYPE (PnGraph, pn_graph, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_KEY,
    PROP_RESOLUTION,
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
    PROP_SAVE_DATA,
    PROP_SAVED_DATA,    /* hidden: the on-disk sample store */
    PROP_MODE,          /* legacy, write-only — see PnGraphMode */
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Resolution enum boxed type                                         */
/* ------------------------------------------------------------------ */

GType
pn_graph_resolution_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_GRAPH_RES_MINUTE,     "PN_GRAPH_RES_MINUTE",     "1 minute"   },
            { PN_GRAPH_RES_15_MINUTES, "PN_GRAPH_RES_15_MINUTES", "15 minutes" },
            { PN_GRAPH_RES_HOUR,       "PN_GRAPH_RES_HOUR",       "1 hour"     },
            { PN_GRAPH_RES_DAY,        "PN_GRAPH_RES_DAY",        "1 day"      },
            { PN_GRAPH_RES_WEEK,       "PN_GRAPH_RES_WEEK",       "1 week"     },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnGraphResolution", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

GType
pn_graph_view_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_GRAPH_VIEW_TIME_SERIES,  "PN_GRAPH_VIEW_TIME_SERIES",  "Time series"  },
            { PN_GRAPH_VIEW_DISTRIBUTION, "PN_GRAPH_VIEW_DISTRIBUTION", "Distribution" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnGraphView", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

GType
pn_graph_style_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_GRAPH_STYLE_POINTS,     "PN_GRAPH_STYLE_POINTS",     "Points"     },
            { PN_GRAPH_STYLE_LINES,      "PN_GRAPH_STYLE_LINES",      "Lines"      },
            { PN_GRAPH_STYLE_BARS,       "PN_GRAPH_STYLE_BARS",       "Bars"       },
            { PN_GRAPH_STYLE_ERROR_BARS, "PN_GRAPH_STYLE_ERROR_BARS", "Error bars" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnGraphStyle", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

GType
pn_graph_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_GRAPH_MODE_LINE,      "PN_GRAPH_MODE_LINE",      "line"      },
            { PN_GRAPH_MODE_HISTOGRAM, "PN_GRAPH_MODE_HISTOGRAM", "histogram" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnGraphMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Window length in seconds for @r.  Exported (shared with the gui-tier
 *  painter, which converts the X axis to seconds-before-now). */
guint
pn_graph_resolution_seconds (PnGraphResolution r)
{
    switch (r)
    {
    case PN_GRAPH_RES_MINUTE:     return 60;
    case PN_GRAPH_RES_15_MINUTES: return 15 * 60;
    case PN_GRAPH_RES_HOUR:       return 3600;
    case PN_GRAPH_RES_DAY:        return 86400;
    case PN_GRAPH_RES_WEEK:       return 7 * 86400;
    }
    return 60;
}

/** Bin width in microseconds for the active resolution.  Always at
 *  least 1 us so the bin-index modulo never divides by zero.  Exported
 *  (the gui-tier painter walks the rings at this cadence). */
gint64
pn_graph_bin_width_us (PnGraph *self)
{
    gint64 w = (gint64) pn_graph_resolution_seconds (self->resolution)
             * G_TIME_SPAN_SECOND
             / (gint64) self->n_bins;
    return w > 0 ? w : 1;
}

/** Reinitialise @bin to represent epoch @epoch with zero samples.
 *  Called lazily when a stale slot is about to be touched. */
static void
reset_bin (
        PnGraphBin *bin,
        gint64      epoch)
{
    bin->epoch  = epoch;
    bin->count  = 0;
    bin->sum    = 0.0;
    bin->sum_sq = 0.0;
    bin->min    = 0.0;
    bin->max    = 0.0;
}

static PnGraphSeries *
series_new (guint arrival_idx)
{
    PnGraphSeries *s = g_new0 (PnGraphSeries, 1);
    guint          i;

    for (i = 0; i < PN_GRAPH_MAX_BINS; i++)
        s->ring[i].epoch = G_MININT64;

    s->arrival_idx = arrival_idx;
    return s;
}

static void
series_free (gpointer data)
{
    PnGraphSeries *s = data;

    if (s != NULL)
        g_free (s->from);
    g_free (s);
}

/** Lookup an existing series for @topic, or create one on first
 *  sight.  Returns NULL once PN_GRAPH_MAX_SERIES distinct topics have
 *  been seen — the message is silently dropped in that case so a
 *  pathological feed cannot grow the table without bound. */
static PnGraphSeries *
series_get_or_create (
        PnGraph     *self,
        const gchar *topic)
{
    PnGraphSeries *s;
    const gchar   *key = topic != NULL ? topic : PN_GRAPH_DEFAULT_TOPIC;

    s = g_hash_table_lookup (self->series, key);
    if (s != NULL)
        return s;

    if (g_hash_table_size (self->series) >= PN_GRAPH_MAX_SERIES)
        return NULL;

    s = series_new (self->next_arrival_idx++);
    g_hash_table_insert (self->series, g_strdup (key), s);
    return s;
}

/** Fold @value, observed at monotonic time @time_us, into @series'
 *  time-bucket ring.  Shared by receive() and the restore path so a
 *  reloaded sample lands in exactly the bucket it would have landed in
 *  live. */
static void
series_bin_sample (
        PnGraph       *self,
        PnGraphSeries *series,
        gint64         time_us,
        gdouble        value)
{
    gint64      width     = pn_graph_bin_width_us (self);
    gint64      cur_epoch = time_us / width;
    guint       slot      = (guint) (cur_epoch % self->n_bins);
    PnGraphBin *bin       = &series->ring[slot];

    if (bin->epoch != cur_epoch)
        reset_bin (bin, cur_epoch);

    if (bin->count == 0)
    {
        bin->min = value;
        bin->max = value;
    }
    else
    {
        if (value < bin->min) bin->min = value;
        if (value > bin->max) bin->max = value;
    }
    bin->sum    += value;
    bin->sum_sq += value * value;
    bin->count  += 1;
}

/** Append one raw observation to @series' sample ring.  The ring is fed
 *  unconditionally — toggling the view property never has to wait for a
 *  fresh sample to start showing the other view — and the distribution
 *  view bins these directly. */
static void
series_push_sample (
        PnGraphSeries *series,
        gint64         time_us,
        gdouble        value)
{
    series->samples[series->sample_head].time_us = time_us;
    series->samples[series->sample_head].value   = value;
    series->sample_head = (series->sample_head + 1) % PN_GRAPH_SAMPLES;
    if (series->sample_count < PN_GRAPH_SAMPLES)
        series->sample_count += 1;
}

/* PnGraphSeriesView (the sortable topic+series pair walked in arrival
 * order on every paint) is published in pn-graph.h. */

static gint
series_view_cmp (
        gconstpointer a,
        gconstpointer b)
{
    const PnGraphSeriesView *va = a;
    const PnGraphSeriesView *vb = b;
    if (va->series->arrival_idx < vb->series->arrival_idx) return -1;
    if (va->series->arrival_idx > vb->series->arrival_idx) return  1;
    return 0;
}

/** Build a sorted snapshot of (topic, series) pairs in arrival
 *  order.  The returned array is heap-allocated and the caller
 *  owns it; the pointed-to strings and series structs remain owned
 *  by the hashtable.  Length is written to @out_n.  Exported as the
 *  gui-tier painter's data-access seam. */
PnGraphSeriesView *
pn_graph_collect_series_sorted (
        PnGraph *self,
        guint   *out_n)
{
    GHashTableIter     it;
    gpointer           k, v;
    guint              n = g_hash_table_size (self->series);
    PnGraphSeriesView *out;
    guint              i = 0;

    if (n == 0)
    {
        if (out_n) *out_n = 0;
        return NULL;
    }

    out = g_new0 (PnGraphSeriesView, n);
    g_hash_table_iter_init (&it, self->series);
    while (g_hash_table_iter_next (&it, &k, &v))
    {
        out[i].topic  = (const gchar *)         k;
        out[i].series = (const PnGraphSeries *) v;
        out[i].from   = ((const PnGraphSeries *) v)->from;
        i += 1;
    }

    g_qsort_with_data (out, n, sizeof (*out),
                       (GCompareDataFunc) series_view_cmp, NULL);

    if (out_n) *out_n = n;
    return out;
}

/** Parse a numeric string into a double.  Accepts an optional leading
 *  sign, an optional "0x"/"0X" prefix for base-16, and otherwise falls
 *  back to base-10 with a fractional part.  Hex parsing goes through
 *  g_ascii_strtoull so values up to 2^64 (well beyond a double's 53-bit
 *  exact range, but still finite) survive — hex-encoded quantities
 *  often arrive as "0x…" and have to be coerced this way.
 *  Decimal parsing uses g_ascii_strtod so "1.5e3" and the like still
 *  work for ordinary numeric feeds. Returns FALSE if the string is empty,
 *  has trailing junk, or yields a non-finite result. */
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
 *  non-scalar / NaN / infinite values; the bin is then left untouched.
 *  Strings are accepted so JSON-RPC quantities encoded as "0x…"
 *  hex literals can be plotted directly. */
static gboolean
node_to_finite_double (
        JsonNode *node,
        gdouble  *out)
{
    GType vtype;
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

/** Deferred-repaint timer: fires once per cooldown when receive()
 *  lands inside the throttle interval.  Clearing pending_repaint_id
 *  before the actual signal emit lets a subsequent receive() arm a
 *  new timer if more samples arrive after this paint. */
static gboolean
on_pending_repaint (gpointer user_data)
{
    PnGraph *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

/** Throttled wrapper around pn_node_request_repaint.  Emits
 *  immediately if more than PN_GRAPH_MIN_REPAINT_INTERVAL_US has
 *  elapsed since the last paint request; otherwise arms a single
 *  one-shot timer for the residual time and coalesces every
 *  intervening request into that one paint. */
static void
schedule_repaint (PnGraph *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    /* A timer is already armed — its eventual firing will pick up
     * whatever the rings look like at that point, including this
     * sample.  Nothing more to do. */
    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_GRAPH_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_GRAPH_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Time-driven refresh                                                */
/* ------------------------------------------------------------------ */

/** TRUE while any series still holds at least one sample. */
static gboolean
graph_has_data (PnGraph *self)
{
    GHashTableIter iter;
    gpointer       val;

    g_hash_table_iter_init (&iter, self->series);
    while (g_hash_table_iter_next (&iter, NULL, &val))
        if (((PnGraphSeries *) val)->sample_count > 0)
            return TRUE;
    return FALSE;
}

/** Refresh cadence: one bin width, clamped so fine resolutions don't
 *  spin and coarse ones still drop aged-out samples reasonably soon. */
static guint
graph_tick_interval_ms (PnGraph *self)
{
    gint64 ms = pn_graph_bin_width_us (self) / G_TIME_SPAN_MILLISECOND;

    if (ms < 250)   ms = 250;
    if (ms > 5000)  ms = 5000;
    return (guint) ms;
}

static gboolean
on_refresh_tick (gpointer user_data)
{
    PnGraph *self = PN_GRAPH (user_data);

    /* Nothing left to scroll — let the timer lapse; receive() re-arms it
     * when fresh data arrives. */
    if (!graph_has_data (self))
    {
        self->tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    schedule_repaint (self);
    return G_SOURCE_CONTINUE;
}

/** Arm the refresh timer if it is not already running. */
static void
graph_ensure_tick (PnGraph *self)
{
    if (self->tick_id == 0)
        self->tick_id = g_timeout_add (graph_tick_interval_ms (self),
                                       on_refresh_tick, self);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_graph_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnGraph       *self = PN_GRAPH (node);
    JsonObject    *root;
    JsonNode      *value_node;
    gdouble        value;
    gint64         now_us;
    const gchar   *topic;
    PnGraphSeries *series;

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

    topic  = pn_message_get_topic (message);
    series = series_get_or_create (self, topic);
    if (series == NULL)
        return;  /* PN_GRAPH_MAX_SERIES cap hit; drop silently. */

    /* Track the message's "from" label for the colour key.  This is the
     * feeding node's name (its class name when unnamed), the same value
     * pn_message_serialize writes into the top-level "from" field.  A
     * topic is normally fed by a single source; if that ever changes we
     * simply keep the most recent label. */
    {
        PnNode *source = pn_message_get_source (message);

        if (source != NULL)
        {
            const gchar *name = pn_node_get_name (source);

            if (name == NULL || *name == '\0')
                name = pn_node_get_class_name (source);

            if (g_strcmp0 (series->from, name) != 0)
            {
                g_free (series->from);
                series->from = g_strdup (name);
            }
        }
    }

    now_us = g_get_monotonic_time ();

    series_bin_sample  (self, series, now_us, value);
    series_push_sample (series, now_us, value);

    /* Throttled: a high-rate feed will not push the worksheet to
     * repaint per message.  Property changes from the dialog still
     * call pn_node_request_repaint directly because they are
     * intrinsically low-frequency. */
    schedule_repaint (self);

    /* Keep the time axis live between messages. */
    graph_ensure_tick (self);

    (void) node;
}

/* ------------------------------------------------------------------ */
/*  Persisted sample store                                             */
/*                                                                     */
/*  With "save-data" on, the samples behind the plot travel with the   */
/*  worksheet: the "saved-data" property serialises them on save and   */
/*  restores them on load, so reopening a document brings the graph    */
/*  back with its history instead of an empty plot.  Two things keep   */
/*  the document bounded: only the newest PN_GRAPH_PERSIST_SAMPLES per */
/*  series are written, and anything already older than the active     */
/*  resolution window (i.e. no longer drawable) is dropped first.      */
/*                                                                     */
/*  The rings are stamped with the monotonic clock, which restarts     */
/*  with the process, so the wire format stores each sample as its age */
/*  in milliseconds at save time plus the wall clock of the save.  On  */
/*  load the two are added back into an age relative to *now* and      */
/*  mapped onto the current monotonic clock — a file that sat on disk  */
/*  for a day therefore restores nothing into a one-hour window, which */
/*  is exactly what a live graph would have shown.                     */
/* ------------------------------------------------------------------ */

#define PN_GRAPH_STORE_VERSION  1

/** Wipe every series' time-bucket ring and refold the raw samples into
 *  it at the current bin width.  Used by the restore path only: live
 *  receives fold each sample in as it arrives. */
static void
graph_rebuild_bins (PnGraph *self)
{
    GHashTableIter it;
    gpointer       v;

    g_hash_table_iter_init (&it, self->series);
    while (g_hash_table_iter_next (&it, NULL, &v))
    {
        PnGraphSeries *s = v;
        guint          first;
        guint          i;

        for (i = 0; i < PN_GRAPH_MAX_BINS; i++)
            s->ring[i].epoch = G_MININT64;

        if (s->sample_count == 0)
            continue;

        first = (s->sample_head + PN_GRAPH_SAMPLES - s->sample_count)
                % PN_GRAPH_SAMPLES;
        for (i = 0; i < s->sample_count; i++)
        {
            const PnGraphSample *smp =
                    &s->samples[(first + i) % PN_GRAPH_SAMPLES];

            series_bin_sample (self, s, smp->time_us, smp->value);
        }
    }
}

/** Deferred half of a restore: the bin width is derived from
 *  "resolution" and "x-buckets", and the loader applies a node's
 *  properties in an unspecified order, so the rings are refolded once
 *  more from the idle — by which point every property in the file has
 *  been applied. */
static gboolean
on_restore_bins (gpointer user_data)
{
    PnGraph *self = user_data;

    self->restore_id = 0;
    graph_rebuild_bins (self);
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

/** Serialise the in-window samples of every series.  Returns %NULL
 *  (write nothing) when there is no data worth carrying. */
static gchar *
graph_encode_data (PnGraph *self)
{
    PnGraphSeriesView *views;
    guint              n_views = 0;
    guint              i;
    gint64             now_mono  = g_get_monotonic_time ();
    gint64             now_real  = g_get_real_time ();
    gint64             window_us =
            (gint64) pn_graph_resolution_seconds (self->resolution)
            * G_TIME_SPAN_SECOND;
    JsonBuilder       *b;
    JsonNode          *root;
    JsonGenerator     *gen;
    gchar             *out;
    gboolean           any = FALSE;

    views = pn_graph_collect_series_sorted (self, &n_views);
    if (views == NULL)
        return NULL;

    b = json_builder_new ();
    json_builder_begin_object (b);

    json_builder_set_member_name (b, "version");
    json_builder_add_int_value (b, PN_GRAPH_STORE_VERSION);
    json_builder_set_member_name (b, "saved");
    json_builder_add_int_value (b, now_real);
    json_builder_set_member_name (b, "series");
    json_builder_begin_array (b);

    for (i = 0; i < n_views; i++)
    {
        const PnGraphSeries *s    = views[i].series;
        guint                keep = MIN (s->sample_count,
                                         (guint) PN_GRAPH_PERSIST_SAMPLES);
        guint                first;
        guint                k;
        gboolean             opened = FALSE;

        if (keep == 0)
            continue;

        first = (s->sample_head + PN_GRAPH_SAMPLES - keep) % PN_GRAPH_SAMPLES;

        for (k = 0; k < keep; k++)
        {
            const PnGraphSample *smp =
                    &s->samples[(first + k) % PN_GRAPH_SAMPLES];
            gint64 age_us = now_mono - smp->time_us;

            /* Already scrolled off the plot — no point carrying it. */
            if (age_us > window_us)
                continue;
            if (age_us < 0)
                age_us = 0;

            if (!opened)
            {
                json_builder_begin_object (b);
                json_builder_set_member_name (b, "topic");
                json_builder_add_string_value (
                        b, views[i].topic != NULL ? views[i].topic : "");
                if (views[i].from != NULL)
                {
                    json_builder_set_member_name (b, "from");
                    json_builder_add_string_value (b, views[i].from);
                }
                json_builder_set_member_name (b, "samples");
                json_builder_begin_array (b);
                opened = TRUE;
                any    = TRUE;
            }

            /* Flat [age_ms, value, age_ms, value, …]: half the brackets
             * of an array of pairs, and the ages are small integers
             * because they are relative to the save. */
            json_builder_add_int_value (b, age_us / G_TIME_SPAN_MILLISECOND);
            json_builder_add_double_value (b, smp->value);
        }

        if (opened)
        {
            json_builder_end_array (b);
            json_builder_end_object (b);
        }
    }

    json_builder_end_array (b);
    json_builder_end_object (b);
    g_free (views);

    if (!any)
    {
        g_object_unref (b);
        return NULL;
    }

    root = json_builder_get_root (b);
    gen  = json_generator_new ();
    json_generator_set_root (gen, root);
    out = json_generator_to_data (gen, NULL);

    g_object_unref (gen);
    json_node_unref (root);
    g_object_unref (b);

    return out;
}

/** Read one array slot as a finite double, accepting both the integer
 *  ages and the double values the encoder writes. */
static gboolean
array_get_finite (
        JsonArray *arr,
        guint      idx,
        gdouble   *out)
{
    JsonNode *node = json_array_get_element (arr, idx);
    GType     type;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    type = json_node_get_value_type (node);
    if (type == G_TYPE_INT64)
        *out = (gdouble) json_node_get_int (node);
    else if (type == G_TYPE_DOUBLE)
        *out = json_node_get_double (node);
    else
        return FALSE;

    return isfinite (*out);
}

/** Repopulate the series table from a store written by
 *  graph_encode_data().  Replaces whatever the node holds (a restore is
 *  a load, not a merge); malformed or stale input leaves an empty
 *  graph rather than a half-filled one. */
static void
graph_decode_data (
        PnGraph     *self,
        const gchar *text)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *series_arr;
    GError     *error = NULL;
    gint64      saved_real, now_real, now_mono, elapsed_us, window_us;
    guint       i, n;
    gboolean    any = FALSE;

    if (text == NULL || *text == '\0')
        return;

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, text, -1, &error))
    {
        g_warning ("pn-graph: saved data is not valid JSON: %s",
                   error != NULL ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_object_unref (parser);
        return;
    }

    obj = json_node_get_object (root);
    if (json_object_get_int_member_with_default (obj, "version", 0)
            != PN_GRAPH_STORE_VERSION
        || !json_object_has_member (obj, "series")
        || !JSON_NODE_HOLDS_ARRAY (json_object_get_member (obj, "series")))
    {
        g_object_unref (parser);
        return;
    }

    series_arr = json_object_get_array_member (obj, "series");
    saved_real = json_object_get_int_member_with_default (obj, "saved", 0);

    now_real   = g_get_real_time ();
    now_mono   = g_get_monotonic_time ();
    elapsed_us = (saved_real > 0) ? now_real - saved_real : 0;
    if (elapsed_us < 0)          /* clock moved backwards — treat as "just now" */
        elapsed_us = 0;
    window_us  = (gint64) pn_graph_resolution_seconds (self->resolution)
               * G_TIME_SPAN_SECOND;

    g_hash_table_remove_all (self->series);
    self->next_arrival_idx = 0;

    n = json_array_get_length (series_arr);
    for (i = 0; i < n; i++)
    {
        JsonNode      *elem = json_array_get_element (series_arr, i);
        JsonObject    *so;
        JsonArray     *samples;
        PnGraphSeries *series;
        const gchar   *from;
        guint          len, j;

        if (elem == NULL || !JSON_NODE_HOLDS_OBJECT (elem))
            continue;

        so = json_node_get_object (elem);
        if (!json_object_has_member (so, "samples")
            || !JSON_NODE_HOLDS_ARRAY (json_object_get_member (so, "samples")))
            continue;

        series = series_get_or_create (
                self,
                json_object_get_string_member_with_default (so, "topic", ""));
        if (series == NULL)
            break;              /* PN_GRAPH_MAX_SERIES reached */

        from = json_object_get_string_member_with_default (so, "from", NULL);
        if (from != NULL && *from != '\0')
        {
            g_free (series->from);
            series->from = g_strdup (from);
        }

        samples = json_object_get_array_member (so, "samples");
        len     = json_array_get_length (samples);

        /* Oldest first, exactly as written — pushing in that order keeps
         * the restored ring in chronological order. */
        for (j = 0; j + 1 < len; j += 2)
        {
            gdouble age_ms, value;
            gint64  age_us;

            if (!array_get_finite (samples, j,     &age_ms) ||
                !array_get_finite (samples, j + 1, &value))
                continue;
            if (age_ms < 0.0)
                age_ms = 0.0;

            age_us = elapsed_us + (gint64) (age_ms * G_TIME_SPAN_MILLISECOND);
            if (age_us > window_us)
                continue;       /* aged out while the file sat on disk */

            series_push_sample (series, now_mono - age_us, value);
            any = TRUE;
        }
    }

    g_object_unref (parser);

    if (!any)
        return;

    /* Fold what we restored into the buckets straight away so a headless
     * caller that never spins the main loop still sees a populated ring,
     * then again from an idle in case "resolution" / "x-buckets" are
     * still to be applied (see #PnGraph.restore_id). */
    graph_rebuild_bins (self);

    if (self->restore_id == 0)
        self->restore_id = g_idle_add (on_restore_bins, self);

    graph_ensure_tick (self);
    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Snapshot the scalar drawing configuration the gui-tier painter     */
/*  (pn-graph-gui.c) reads each frame.  The per-series rings are read   */
/*  separately through pn_graph_collect_series_sorted (the structs are  */
/*  published in the header).                                          */
/* ------------------------------------------------------------------ */

void
pn_graph_get_paint_state (PnGraph *self, PnGraphPaintState *out)
{
    g_return_if_fail (PN_IS_GRAPH (self));
    g_return_if_fail (out != NULL);

    out->resolution       = self->resolution;
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

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_graph_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    (void) node;
    if (out_width  != NULL) *out_width  = PN_GRAPH_WIDTH;
    if (out_height != NULL) *out_height = PN_GRAPH_TOTAL_HEIGHT;
}

static double
pn_graph_get_header_height (PnNode *node)
{
    (void) node;
    return PN_GRAPH_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_graph_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnGraph *self = PN_GRAPH (object);

    switch (prop_id)
    {
    case PROP_KEY:
        g_value_set_string (value, self->key);
        break;
    case PROP_RESOLUTION:
        g_value_set_enum (value, self->resolution);
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
    case PROP_SAVE_DATA:
        g_value_set_boolean (value, self->save_data);
        break;
    case PROP_SAVED_DATA:
        {
            /* Serialised on demand: the document serialiser reads this
             * property while saving, and with "save-data" off the empty
             * string keeps the store out of the file entirely. */
            gchar *text = self->save_data ? graph_encode_data (self) : NULL;

            g_value_take_string (value, text != NULL ? text : g_strdup (""));
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_graph_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnGraph *self = PN_GRAPH (object);

    switch (prop_id)
    {
    case PROP_KEY:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->key, s) != 0)
            {
                g_free (self->key);
                self->key = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_KEY]);
            }
        }
        break;
    case PROP_RESOLUTION:
        {
            PnGraphResolution r = g_value_get_enum (value);
            if (self->resolution != r)
            {
                self->resolution = r;
                /* Old bin epochs are computed against the previous
                 * bin width, so under the new width every slot looks
                 * stale.  Lazy reset handles that on the next touch
                 * — no need to wipe the rings here.  The raw-sample
                 * rings are unaffected: histogram mode just filters
                 * by the new cutoff on its next paint. */
                /* Bin width changed → re-arm the refresh at the new
                 * cadence (only if it was running). */
                if (self->tick_id != 0)
                {
                    g_source_remove (self->tick_id);
                    self->tick_id = 0;
                    graph_ensure_tick (self);
                }
                g_object_notify_by_pspec (object, props[PROP_RESOLUTION]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_X_BUCKETS:
        {
            guint b = g_value_get_uint (value);
            if (b != self->n_bins)
            {
                /* Changing the bucket count changes the bin width, so
                 * every existing slot's epoch looks stale under the new
                 * width and lazy reset rebuilds the ring over the next
                 * window — same reasoning as PROP_RESOLUTION.  The cap
                 * is enforced by the param-spec's max (PN_GRAPH_MAX_BINS)
                 * so the fixed-size ring can never overflow. */
                self->n_bins = b;
                if (self->tick_id != 0)
                {
                    g_source_remove (self->tick_id);
                    self->tick_id = 0;
                    graph_ensure_tick (self);
                }
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
    case PROP_MODE:
        /* Legacy load path: translate the old single "mode" into the
         * equivalent view + style pair (line = time series drawn as a
         * polyline; histogram = distribution drawn as bars). */
        switch (g_value_get_enum (value))
        {
        case PN_GRAPH_MODE_HISTOGRAM:
            self->view  = PN_GRAPH_VIEW_DISTRIBUTION;
            self->style = PN_GRAPH_STYLE_BARS;
            break;
        case PN_GRAPH_MODE_LINE:
        default:
            self->view  = PN_GRAPH_VIEW_TIME_SERIES;
            self->style = PN_GRAPH_STYLE_LINES;
            break;
        }
        g_object_notify_by_pspec (object, props[PROP_DATA_VIEW]);
        g_object_notify_by_pspec (object, props[PROP_DRAW_STYLE]);
        pn_node_request_repaint (PN_NODE (self));
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
    case PROP_SAVE_DATA:
        {
            gboolean b = g_value_get_boolean (value);
            if (self->save_data != b)
            {
                self->save_data = b;
                g_object_notify_by_pspec (object, props[PROP_SAVE_DATA]);
            }
        }
        break;
    case PROP_SAVED_DATA:
        /* Load-time only: the loader hands back whatever the getter
         * wrote.  An empty string means "nothing was saved" and leaves
         * the live rings alone, so toggling "save-data" off in the
         * dialog never wipes the plot on the screen. */
        graph_decode_data (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_graph_finalize (GObject *object)
{
    PnGraph *self = PN_GRAPH (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }
    if (self->tick_id != 0)
    {
        g_source_remove (self->tick_id);
        self->tick_id = 0;
    }
    if (self->restore_id != 0)
    {
        g_source_remove (self->restore_id);
        self->restore_id = 0;
    }

    /* The PLplot stream (if one was ever allocated) is torn down by its
     * GObject-data destroy-notify in the gui tier — the core never calls
     * plend1. */

    g_clear_pointer (&self->series, g_hash_table_unref);
    g_clear_pointer (&self->key,    g_free);

    G_OBJECT_CLASS (pn_graph_parent_class)->finalize (object);
}

static void
pn_graph_class_init (PnGraphClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_graph_get_property;
    object_class->set_property = pn_graph_set_property;
    object_class->finalize     = pn_graph_finalize;

    node_class->receive           = pn_graph_receive;
    node_class->get_size          = pn_graph_get_size;
    node_class->get_header_height = pn_graph_get_header_height;
    /* The PLplot/cairo plot painter (paint_plot) and the two-tab
     * settings dialog (build_class_tabs) are installed onto this class by
     * the gui tier — pn_graph_gui_install() in pn-graph-gui.c — so the
     * headless core carries no GTK/cairo/PLplot. */

    node_class->class_name        = "Graph";
    node_class->icon              = "\xef\x87\xbe";  /* fa-area-chart U+F1FE */
    node_class->color             = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_KEY] = g_param_spec_string (
            "key", "Key",
            "JSON path (\"/\"-separated, e.g. data/value) to the "
            "numeric value plotted on the Y axis",
            "data/value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RESOLUTION] = g_param_spec_enum (
            "resolution", "Resolution",
            "Total time span summarised by the plot.  In line mode it "
            "is the X-axis time span; in histogram mode it is the "
            "rolling window of samples that get binned.",
            PN_TYPE_GRAPH_RESOLUTION,
            PN_GRAPH_RES_MINUTE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_X_BUCKETS] = g_param_spec_uint (
            "x-buckets", "X buckets",
            "Number of time buckets the window is split into along the X "
            "axis.  Each bucket aggregates the samples that land in its "
            "time slice (their count, mean, spread); the time-series view "
            "draws one point — or one error-bar box-and-whisker — per "
            "bucket.  Fewer, wider buckets fold more samples together (a "
            "smoother line, a fuller spread per error bar); more, narrower "
            "buckets track finer detail.  Does not affect the distribution "
            "view, which bins by value rather than by time.",
            2, PN_GRAPH_MAX_BINS, PN_GRAPH_DEF_BINS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DATA_VIEW] = g_param_spec_enum (
            "data-view", "Data view",
            "What the plot represents.  Time series plots each value "
            "against time (X axis is time, most recent on the right); "
            "Distribution bins the in-window samples by value and plots "
            "how often each value occurs (X axis is value, Y axis is "
            "the count) to show where they cluster.  Both views "
            "auto-switch to a 3D projection when messages from more "
            "than one topic arrive, with each topic stacked along the "
            "Z axis as a separate series.",
            PN_TYPE_GRAPH_VIEW,
            PN_GRAPH_VIEW_TIME_SERIES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DRAW_STYLE] = g_param_spec_enum (
            "draw-style", "Draw as",
            "How the data view is drawn: Points (a discrete marker per "
            "sample), Lines (a polyline through the samples), Bars (a "
            "filled bar rising from the baseline), or Error bars (a "
            "box-and-whisker per time bucket — whiskers to the bucket "
            "min/max and a box centred on the mean spanning ±1 standard "
            "deviation; each bucket is green when its mean rose versus "
            "the previous bucket and red when it fell).  Honoured for "
            "single-topic plots; multi-topic 3D plots fall back to the "
            "polyline form for the time-series view and to bars for the "
            "distribution view, and the Error-bars style draws as bars "
            "in the distribution view.",
            PN_TYPE_GRAPH_STYLE,
            PN_GRAPH_STYLE_LINES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_COLOR] = g_param_spec_boxed (
            "line-color", "Line colour",
            "Colour of the data polyline.  In multi-topic 3D mode it "
            "is the colour of the first series (lowest Z); subsequent "
            "series walk the hue wheel in golden-angle steps from "
            "this base.",
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
            "Fill colour of the plot rectangle behind the polyline",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_GRID] = g_param_spec_boolean (
            "show-grid", "Show grid",
            "Draw a thin grid through every major tick on both axes",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LOG_Y] = g_param_spec_boolean (
            "log-y", "Logarithmic Y axis",
            "Plot the Y axis on a base-10 logarithmic scale.  "
            "Currently honoured only in histogram mode, where it is "
            "useful for long-tailed distributions whose tallest bars "
            "would otherwise crush the smaller ones flat.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y_FROM_ZERO] = g_param_spec_boolean (
            "y-from-zero", "Y starts from 0",
            "Anchor the Y axis at zero instead of auto-fitting it "
            "tightly around the data.  Without this, a series of large "
            "values that wobbles only slightly fills the whole height, "
            "exaggerating tiny changes; anchoring at zero keeps the "
            "movement in proportion to the absolute value.  Honoured "
            "in line mode (linear Y only — it does not apply on a "
            "logarithmic axis).",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Legacy migration shim.  Worksheets saved before the view/style
     * split persisted a single "mode" enum; accept it on load and map
     * it onto data-view + draw-style.  Write-only on purpose: being
     * unreadable keeps it out of node_to_json (which only serialises
     * read+write properties), so re-saving a migrated worksheet drops
     * "mode" and writes the new pair instead.  No nick/blurb because it
     * never surfaces in the settings dialog. */
    props[PROP_SAVE_DATA] = g_param_spec_boolean (
            "save-data", "Save data with the worksheet",
            "Write the plotted samples into the worksheet file when it is "
            "saved and read them back when it is opened, so the graph "
            "returns with its history instead of an empty plot.  Bounded "
            "by design: samples already older than the resolution window "
            "are dropped, and at most the newest 512 per series are "
            "written, so the document cannot grow without end.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* The store itself.  Deliberately absent from the settings schema
     * below, so it never shows up as a dialog row — it exists purely so
     * the generic property serialiser carries the samples in and out of
     * the document. */
    props[PROP_SAVED_DATA] = g_param_spec_string (
            "saved-data", "Saved data",
            "Serialised sample store written when \"save-data\" is on: a "
            "JSON object holding, per series, the topic and the in-window "
            "samples as (age in ms at save time, value) pairs.  Read on "
            "load to repopulate the plot; not user-editable.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODE] = g_param_spec_enum (
            "mode", NULL, NULL,
            PN_TYPE_GRAPH_MODE,
            PN_GRAPH_MODE_LINE,
            G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema (Phase 7.4): two themed pages
     * (Appearance | Data) instead of one tall auto tab.  GTK-free data
     * held on the class; the GUI tier's renderer turns it into the same
     * pages the old pn-graph-gui.c build_class_tabs produced, every
     * editor still the type-driven default (PN_EDITOR_AUTO — enum combos
     * for draw-style / data-view, a colour button for each *-color row,
     * check buttons for the booleans). */
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
        pn_settings_schema_row (schema, "key",         PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "resolution",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "x-buckets",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "data-view",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "log-y",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "y-from-zero", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "save-data",   PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_graph_init (PnGraph *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->key        = g_strdup ("data/value");
    self->resolution = PN_GRAPH_RES_MINUTE;
    self->n_bins     = PN_GRAPH_DEF_BINS;
    self->view       = PN_GRAPH_VIEW_TIME_SERIES;
    self->style      = PN_GRAPH_STYLE_LINES;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;
    self->tick_id            = 0;

    /* Defaults match the original hard-coded palette so existing
     * worksheets keep the same look until the user picks their own
     * colours. */
    self->line_color       = (PnColor) {  30.0/255.0,  60.0/255.0, 140.0/255.0, 1.0 };
    self->axis_color       = (PnColor) {  70.0/255.0,  70.0/255.0,  70.0/255.0, 1.0 };
    self->background_color = (PnColor) { 1.0, 1.0, 1.0, 1.0 };
    self->line_width       = 2;
    self->show_grid        = FALSE;
    self->log_y            = FALSE;
    self->y_from_zero      = FALSE;
    self->save_data        = FALSE;
    self->restore_id       = 0;

    self->series = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, series_free);
    self->next_arrival_idx = 0;

    /* The per-instance PLplot stream is allocated lazily by the gui tier
     * on the first paint (and torn down via a GObject-data destroy-
     * notify), so the GTK-free core never calls plmkstrm / plend1. */

    pn_node_set_class_name (node, "Graph");
    pn_node_set_icon       (node, "\xef\x87\xbe");  /* fa-area-chart U+F1FE */
    pn_node_set_color (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnGraph *
pn_graph_new (void)
{
    return g_object_new (PN_TYPE_GRAPH, NULL);
}

guint
pn_graph_get_series_count (PnGraph *self)
{
    g_return_val_if_fail (PN_IS_GRAPH (self), 0);
    return (self->series != NULL) ? g_hash_table_size (self->series) : 0;
}
