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
#include "pn-node-dialog-helpers.h"

#include <math.h>
#include <plplot/plplot.h>

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

/* Capacity of the per-series time-bucket ring: the largest number of X
 * buckets the configured window can be split into.  Each bin uses a few
 * tens of bytes, so the ring stays under 10 KB per series regardless of
 * resolution.  The *active* bucket count is the runtime "x-buckets"
 * property (PnGraph.n_bins), which must always stay within this cap so
 * the fixed-size ring and the stack arrays sized to it never overflow. */
#define PN_GRAPH_MAX_BINS  200

/* Default active X-bucket count — the full ring, matching the value the
 * window was hard-split into before the count became configurable, so
 * worksheets saved without an "x-buckets" property render unchanged. */
#define PN_GRAPH_DEF_BINS  200

/* Raw sample ring used by histogram mode.  Each entry is 16 bytes so
 * the whole ring is 32 KB per series — well above the time-bucket
 * footprint, but a frequency distribution genuinely needs
 * un-aggregated samples, there is no value-bin equivalent of the 200
 * time-bins that doesn't either pin the value range up front or
 * trade complexity for very little.  In long-window modes (1 day,
 * 1 week) the ring shows the most recent PN_GRAPH_SAMPLES values
 * that fell inside the window rather than every value seen, which
 * is documented on the help page. */
#define PN_GRAPH_SAMPLES  2048

/* Number of value bins drawn in histogram mode.  40 is a comfortable
 * default for the 2D plot — enough resolution to read the shape of a
 * typical distribution, few enough that each bar is several pixels
 * wide on the canonical 280-px-wide plot. */
#define PN_GRAPH_HIST_BINS  40

/* Number of value bins drawn in 3D histogram mode.  Each topic gets
 * its own ribbon of bars, and the 3D projection shrinks the X axis
 * (it shares screen width with the Y/Z axes of the cube), so the
 * full 40 bins would render as 1-2 px-wide slivers per bar that hide
 * the 3D shading.  12 bins keeps the histogram shape readable while
 * giving each bar enough screen X-width that its X-extent isn't an
 * order of magnitude smaller than its Y (ribbon-depth) projection. */
#define PN_GRAPH_3D_HIST_BINS  12

/* Fraction of a bin's X-width occupied by the bar itself; the
 * remainder splits as equal gaps on either side.  Without a gap,
 * adjacent bars in the same series share an edge and the row reads
 * as a continuous slab; 0.8 leaves a clear 20 % horizontal gutter
 * between neighbouring bars. */
#define PN_GRAPH_3D_BAR_FILL  0.8

/* Half-width of each topic ribbon in world Y-units (so each ribbon
 * occupies 2 × this fraction of its 1.0-unit series slot, with the
 * remainder as a gap between series).  0.2 → 0.4 ribbon width,
 * 0.6 gap. */
#define PN_GRAPH_3D_RIBBON_HALF  0.20

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

/* Viewing angles for the 3D projection.  alt = 45° puts the camera
 * high enough that the top face of every bar reads as a clearly
 * visible parallelogram (at the original 30° it was only a few px
 * tall and the bars came out looking nearly 2D); az = 300° rotates
 * the cube so the value axis runs to the front-left and the topic
 * stack to the front-right, matching the gnuplot 3D-histogram
 * convention the eye is trained on. */
#define PN_GRAPH_3D_ALT  45.0
#define PN_GRAPH_3D_AZ   300.0

/* Sentinel topic used when a message arrives without one.  Stored
 * in the hashtable like any other topic key so the empty-topic case
 * shares the same per-series machinery as the named-topic one. */
#define PN_GRAPH_DEFAULT_TOPIC  ""

/* ------------------------------------------------------------------ */
/*  Per-bin state                                                      */
/*                                                                     */
/*  Same fixed-bucket pattern as PnStats: a circular buffer indexed by */
/*  monotonic_time / bin_width_us.  Each slot holds an absolute epoch  */
/*  index so stale slots can be detected and lazily reset before they  */
/*  are touched again.                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Absolute bin index this slot represents.  Initialised to
     * G_MININT64 so an uninitialised slot can never match a real
     * (always-positive) epoch. */
    gint64   epoch;

    /* Number of samples folded into this bin. */
    guint64  count;

    /* Tallies over the samples in this bin.  sum_sq (the running sum
     * of value²) lets the Error-bars draw-style recover the per-bucket
     * standard deviation as sqrt(sum_sq/count - mean²) without keeping
     * the individual samples. */
    gdouble  sum;
    gdouble  sum_sq;
    gdouble  min;
    gdouble  max;
} PnGraphBin;

/* One raw observation kept verbatim for histogram mode.  Stamped with
 * the same monotonic clock the time-bin ring uses so a single cutoff
 * filters both stores consistently. */
typedef struct
{
    gint64  time_us;
    gdouble value;
} PnGraphSample;

/* ------------------------------------------------------------------ */
/*  Per-topic series                                                   */
/*                                                                     */
/*  Each distinct message topic gets its own bin ring + raw-sample     */
/*  ring + arrival index.  Arrival index gives every series a stable   */
/*  Z-axis slot in 3D mode (first-seen topic is closest to the         */
/*  viewer); rebuilding from arrival order on every paint keeps the    */
/*  layout deterministic even when the hashtable iteration order is    */
/*  not.                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnGraphBin     ring[PN_GRAPH_MAX_BINS];

    PnGraphSample  samples[PN_GRAPH_SAMPLES];
    guint          sample_head;
    guint          sample_count;

    /* Stable arrival-order index; used as the Z-axis position in 3D
     * mode and to seed the auto-assigned colour. */
    guint          arrival_idx;
} PnGraphSeries;

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
    GdkRGBA    line_color;
    GdkRGBA    axis_color;
    GdkRGBA    background_color;
    guint      line_width;
    gboolean   show_grid;
    gboolean   log_y;
    gboolean   y_from_zero;

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

    /* Per-instance PLplot stream id.  PLplot keeps a process-wide
     * stream table; reusing one id across multiple PnGraph nodes
     * would cross their axis state, so each node owns its own id
     * allocated in init and freed in finalize. */
    gboolean    plstream_valid;
    PLINT       plstream;
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

/** Window length in seconds for @r. */
static guint
resolution_seconds (PnGraphResolution r)
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
 *  least 1 us so the bin-index modulo never divides by zero. */
static gint64
bin_width_us (PnGraph *self)
{
    gint64 w = (gint64) resolution_seconds (self->resolution)
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
    g_free (data);
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

/* Sortable wrapper used to walk the series in arrival order on every
 * paint.  Storing the topic alongside the series saves a second
 * hashtable lookup when the painter wants the label. */
typedef struct
{
    const gchar         *topic;
    const PnGraphSeries *series;
} PnGraphSeriesView;

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
 *  by the hashtable.  Length is written to @out_n. */
static PnGraphSeriesView *
collect_series_sorted (
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
        i += 1;
    }

    g_qsort_with_data (out, n, sizeof (*out),
                       (GCompareDataFunc) series_view_cmp, NULL);

    if (out_n) *out_n = n;
    return out;
}

/** Deterministic colour for series @idx.  Index 0 uses the user-
 *  picked line colour exactly so single-topic feeds look identical
 *  to the pre-multi-topic version of this node.  Higher indices walk
 *  the hue wheel in golden-angle steps (≈137.5°) starting from the
 *  base hue, which is the classic trick for generating any number
 *  of distinct, well-spaced colours without runs of similar hues. */
static void
color_for_series (
        const PnGraph *self,
        guint          idx,
        GdkRGBA       *out)
{
    if (idx == 0)
    {
        *out = self->line_color;
        return;
    }

    {
        PLFLT  h_base, l_base, s_base;
        PLFLT  h, l, s;
        PLFLT  r, g, b;

        plrgbhls ((PLFLT) self->line_color.red,
                  (PLFLT) self->line_color.green,
                  (PLFLT) self->line_color.blue,
                  &h_base, &l_base, &s_base);

        /* Hue lives on 0..360 in PLplot's HLS.  Step by the golden
         * angle so consecutive topics never end up next to each
         * other on the colour wheel.  Lightness/saturation are
         * pinned to comfortable values so a too-pale or too-grey
         * base colour still yields readable series. */
        h = fmod (h_base + 137.508 * (gdouble) idx, 360.0);
        if (h < 0.0) h += 360.0;
        l = 0.50;
        s = 0.65;

        plhlsrgb (h, l, s, &r, &g, &b);

        out->red   = r;
        out->green = g;
        out->blue  = b;
        out->alpha = self->line_color.alpha;
    }
}

/** Format @value into @label as inline scientific notation when its
 *  magnitude is far enough from 1 that "%g" would otherwise emit
 *  "1e+17" or trigger PLplot's multiplier extraction.  Within the
 *  everyday range (10⁻³ ≤ |v| < 10⁶) we just hand off to "%g" so
 *  small integers and percentages keep their plain form.  Outside
 *  it we render "{mantissa}×10#uN#d" — PLplot's #u/#d escapes set a
 *  superscript on the exponent so the cairo driver paints "2×10¹⁷"
 *  inline at every tick instead of bare "2"s sitting under a shared
 *  "(×10^17)" multiplier label. */
static void
format_inline_scientific (
        char    *label,
        PLINT    length,
        gdouble  value)
{
    gdouble abs_v = fabs (value);
    gint    exp10;
    gdouble mantissa;
    gchar   mant_buf[32];

    if (abs_v == 0.0)
    {
        g_snprintf (label, length, "0");
        return;
    }

    if (abs_v >= 1e-3 && abs_v < 1e6)
    {
        g_snprintf (label, length, "%g", value);
        return;
    }

    exp10    = (gint) floor (log10 (abs_v));
    mantissa = value / pow (10.0, exp10);

    /* "%g" already strips trailing zeros, so 2.0 prints as "2", 1.5
     * as "1.5", and the very-occasional FP residue from the divide
     * (1.9999999) is rounded back to its nominal value because PLplot
     * always picks tick positions whose mantissa is exactly nice
     * relative to the multiplier. */
    g_snprintf (mant_buf, sizeof (mant_buf), "%g", mantissa);

    g_snprintf (label, length, "%s×10#u%d#d", mant_buf, exp10);
}

/** PLplot tick-label formatter.  Branches on @axis and the active
 *  data view: in the time-series view the X axis carries
 *  seconds-before-now, presented as a
 *  positive duration with the unit natural to the active resolution
 *  ("60s" for the minute window, "60m" for the hour, "24h" for the
 *  day, "7d" for the week).  Every other value-bearing axis goes
 *  through format_inline_scientific so large magnitudes (e.g.
 *  a byte count in the billions) read as "2×10¹⁷" right at the tick
 *  rather than as a bare "2" with a shared multiplier label tucked
 *  beside the axis.  In 3D mode the topic-index axis (PL_Y_AXIS for
 *  line, PL_Y_AXIS for histogram) is squashed to integer ticks
 *  upstream and we never see fractional values here, so the same
 *  "%g" path renders "0", "1", "2" cleanly. */
static void
graph_label_format (
        PLINT     axis,
        PLFLT     value,
        char     *label,
        PLINT     length,
        PLPointer user)
{
    PnGraph *self = user;

    if (axis == PL_X_AXIS && self->view == PN_GRAPH_VIEW_TIME_SERIES)
    {
        gdouble secs = fabs ((gdouble) value);

        switch (self->resolution)
        {
        case PN_GRAPH_RES_MINUTE:
            g_snprintf (label, length, "%gs", secs);
            break;
        case PN_GRAPH_RES_15_MINUTES:
        case PN_GRAPH_RES_HOUR:
            g_snprintf (label, length, "%gm", secs / 60.0);
            break;
        case PN_GRAPH_RES_DAY:
            g_snprintf (label, length, "%gh", secs / 3600.0);
            break;
        case PN_GRAPH_RES_WEEK:
            g_snprintf (label, length, "%gd", secs / 86400.0);
            break;
        }
        return;
    }

    format_inline_scientific (label, length, (gdouble) value);
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
    gint64 ms = bin_width_us (self) / G_TIME_SPAN_MILLISECOND;

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
    gint64         width;
    gint64         cur_epoch;
    guint          slot;
    PnGraphBin    *bin;
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

    now_us    = g_get_monotonic_time ();
    width     = bin_width_us (self);
    cur_epoch = now_us / width;
    slot      = (guint) (cur_epoch % self->n_bins);
    bin       = &series->ring[slot];

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

    /* Mirror the value into the per-series raw-sample ring so
     * histogram mode has something to bin.  The ring is fed
     * unconditionally — toggling the mode property never has to
     * wait for a fresh sample to start showing the other view. */
    series->samples[series->sample_head].time_us = now_us;
    series->samples[series->sample_head].value   = value;
    series->sample_head = (series->sample_head + 1) % PN_GRAPH_SAMPLES;
    if (series->sample_count < PN_GRAPH_SAMPLES)
        series->sample_count += 1;

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
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Common PLplot setup shared by every painter.  Initialises the
 *  extcairo device against @cr, advances to a fresh subpage, sets up
 *  the user-chosen axis colour in cmap0 slot 2, and picks a character-
 *  height multiplier that suits the rectangle size.  Series colours
 *  are pinned into cmap0 slots 8…(8 + series_count - 1) by the
 *  painters that draw multiple series — slot 1 still holds the
 *  default (single-series) line colour for the 2D case.  The caller
 *  still has to set its own viewport (plvpor) and world coordinates
 *  (plwind / plw3d) because each painter needs different ones. */
static void
paint_setup_page (
        PnGraph *self,
        cairo_t *cr,
        double   w,
        double   h)
{
    plsstrm (self->plstream);
    plsdev  ("extcairo");
    plspage (0.0, 0.0,
             (PLINT) w,
             (PLINT) h,
             0, 0);
    plinit  ();
    pl_cmd  (PLESC_DEVINIT, cr);

    /* Advance to a fresh subpage.  Without this, plvpor / plwind /
     * plline all bail out with "please call pladv or plenv to go to
     * a subpage" because plinit alone only sets up the device, not
     * a drawable page. */
    pladv (0);

    /* Pin user-chosen colours into cmap0 — "color 15" is white in the
     * default cmap0 used by the cairo driver, so anything we draw
     * with the stock palette would vanish against our (typically
     * white) plot background. */
    plscol0 (1,
             (PLINT) (self->line_color.red   * 255.0 + 0.5),
             (PLINT) (self->line_color.green * 255.0 + 0.5),
             (PLINT) (self->line_color.blue  * 255.0 + 0.5));
    plscol0 (2,
             (PLINT) (self->axis_color.red   * 255.0 + 0.5),
             (PLINT) (self->axis_color.green * 255.0 + 0.5),
             (PLINT) (self->axis_color.blue  * 255.0 + 0.5));

    /* Tick labels at roughly 1.8× the default character height —
     * comfortably readable on the small 280×144 plot.  The viewport
     * margins set by the painter are sized to fit labels at this
     * scale.  When the worksheet hands us a noticeably larger
     * rectangle (the plot has been clicked to enlarge it) PLplot
     * would otherwise scale the labels up with the page, swelling
     * them out of proportion; halve the multiplier in that case so
     * the enlarged plot devotes the extra real estate to the data,
     * not the axis text. */
    {
        const gboolean enlarged = (w > PN_GRAPH_WIDTH * 1.2);
        plschr (0, enlarged ? 0.9 : 1.8);
    }

    /* Disable PLplot's automatic multiplier-extraction for both
     * value axes.  By default, when the tick magnitudes exceed a
     * 4-digit budget PLplot factors out a common 10ⁿ multiplier and
     * prints it as a separate "(×10ⁿ)" label tucked beside the axis;
     * graph_label_format already inlines the exponent on every tick
     * via #u…#d superscripts, so the shared offset label is just
     * redundant clutter that the small plot rectangle has no room
     * for.  Setting digmax to 20 makes PLplot accept any reasonable
     * tick value without scaling. */
    plsxax (20, 0);
    plsyax (20, 0);
}

/** Pin colour @rgba into cmap0 slot @slot.  Used by the 3D painters
 *  to register one cmap0 entry per series; plcol0(slot) then selects
 *  that series' colour for the next stroke/fill. */
static void
paint_set_cmap0 (
        PLINT          slot,
        const GdkRGBA *rgba)
{
    plscol0 (slot,
             (PLINT) (rgba->red   * 255.0 + 0.5),
             (PLINT) (rgba->green * 255.0 + 0.5),
             (PLINT) (rgba->blue  * 255.0 + 0.5));
}

/** Re-shade an RGB triple in HLS space: keep hue and saturation,
 *  but set lightness to @target (clamped to [0, 1]).  Used to derive
 *  the per-face shades that give a 3D bar its sense of volume — the
 *  top face uses a lightness well above the base, the side faces
 *  well below, so the three visible faces of every bar read as three
 *  clearly distinct intensities of the same hue.  RGB-only scaling
 *  (the previous shade()) drifts the apparent hue and only spans
 *  half the perceived range for the same numeric factor, which is
 *  why the first multi-topic 3D output looked nearly flat. */
static GdkRGBA
shade_l (
        GdkRGBA c,
        gdouble target_l)
{
    PLFLT h, l, s;
    PLFLT r, g, b;
    GdkRGBA out;

    plrgbhls ((PLFLT) c.red, (PLFLT) c.green, (PLFLT) c.blue, &h, &l, &s);

    if (target_l < 0.0) target_l = 0.0;
    if (target_l > 1.0) target_l = 1.0;

    /* Pin saturation to at least 0.55 so a near-grey base colour
     * still yields readable per-face contrast — without it the
     * shaded faces of a grey-blue base would collapse back to
     * indistinguishable greys. */
    if (s < 0.55) s = 0.55;

    plhlsrgb (h, (PLFLT) target_l, s, &r, &g, &b);

    out.red   = r;
    out.green = g;
    out.blue  = b;
    out.alpha = c.alpha;
    return out;
}

/* ------------------------------------------------------------------ */
/*  3D projection helper                                               */
/*                                                                     */
/*  PLplot's plw3d sets up an internal 3D-to-2D projection used by     */
/*  plline3 / plbox3 etc., but those functions only stroke lines —     */
/*  plpoly3 is documented as "polyline" too and does not fill.  To     */
/*  draw solid shaded bar faces we have to project the four corners    */
/*  of each face into the same 2D-world coordinates plw3d uses and     */
/*  hand them to plfill (which works in 2D world coords, independent   */
/*  of plw3d).  This struct caches the trig values once per paint so   */
/*  the inner loop over (series × bins × faces) stays cheap.           */
/*                                                                     */
/*  The projection formula is the one PLplot itself applies inside     */
/*  plw3d (see plot3d.c in PLplot sources):                            */
/*                                                                     */
/*     xn = (x - xmin) / (xmax - xmin) * basex                         */
/*     yn = (y - ymin) / (ymax - ymin) * basey                         */
/*     zn = (z - zmin) / (zmax - zmin) * height                        */
/*     sx = (xn - basex/2) * cos(az) + (yn - basey/2) * sin(az)        */
/*     sy = -(xn - basex/2) * sin(az) * sin(alt)                       */
/*           + (yn - basey/2) * cos(az) * sin(alt)                     */
/*           + zn * cos(alt)                                           */
/*                                                                     */
/*  Matching it exactly means the filled faces line up pixel-perfect   */
/*  with the plbox3 / plline3 wireframe overlaid on top.               */
/* ------------------------------------------------------------------ */

typedef struct
{
    PLFLT basex, basey, height;
    PLFLT xmin, xmax;
    PLFLT ymin, ymax;
    PLFLT zmin, zmax;
    PLFLT caz, saz, calt, salt;
} Pn3DProj;

static void
proj_init (
        Pn3DProj *p,
        PLFLT     basex,
        PLFLT     basey,
        PLFLT     height,
        PLFLT     xmin,
        PLFLT     xmax,
        PLFLT     ymin,
        PLFLT     ymax,
        PLFLT     zmin,
        PLFLT     zmax,
        PLFLT     alt_deg,
        PLFLT     az_deg)
{
    p->basex  = basex;  p->basey  = basey;  p->height = height;
    p->xmin   = xmin;   p->xmax   = xmax;
    p->ymin   = ymin;   p->ymax   = ymax;
    p->zmin   = zmin;   p->zmax   = zmax;
    p->caz    = (PLFLT) cos (M_PI * az_deg  / 180.0);
    p->saz    = (PLFLT) sin (M_PI * az_deg  / 180.0);
    p->calt   = (PLFLT) cos (M_PI * alt_deg / 180.0);
    p->salt   = (PLFLT) sin (M_PI * alt_deg / 180.0);
}

static inline void
proj_xy (
        const Pn3DProj *p,
        PLFLT           x,
        PLFLT           y,
        PLFLT           z,
        PLFLT          *sx,
        PLFLT          *sy)
{
    PLFLT xn = (x - p->xmin) / (p->xmax - p->xmin) * p->basex - p->basex * 0.5f;
    PLFLT yn = (y - p->ymin) / (p->ymax - p->ymin) * p->basey - p->basey * 0.5f;
    PLFLT zn = (z - p->zmin) / (p->zmax - p->zmin) * p->height;

    *sx = xn * p->caz + yn * p->saz;
    *sy = -xn * p->saz * p->salt + yn * p->caz * p->salt + zn * p->calt;
}

/** Depth of a 3D point under the projection: higher means further
 *  from the viewer (i.e. drawn first by the painter's algorithm).
 *
 *  Derivation: PLplot's azimuth convention is "at az=0 the y axis
 *  faces the viewer", which puts the camera at the +y direction
 *  with CCW rotation around z as az grows.  The viewer position is
 *  therefore
 *      camera = (-sin(az)·cos(alt), +cos(az)·cos(alt), +sin(alt))
 *  and the depth-into-screen of a point is the negative of its dot
 *  product with that camera vector, i.e.
 *      depth = +xn·sin(az)·cos(alt) - yn·cos(az)·cos(alt) - zn·sin(alt)
 *  An earlier version had the y-term sign flipped because it assumed
 *  the camera sat at -y for az=0; that produced a back-to-front sort
 *  that was reversed for any bar pair separated along y, and it made
 *  the painter draw the world-y_min face as the "front" face when
 *  the viewer was actually looking at the world-y_max face. */
static inline PLFLT
proj_depth (
        const Pn3DProj *p,
        PLFLT           x,
        PLFLT           y,
        PLFLT           z)
{
    PLFLT xn = (x - p->xmin) / (p->xmax - p->xmin) * p->basex - p->basex * 0.5f;
    PLFLT yn = (y - p->ymin) / (p->ymax - p->ymin) * p->basey - p->basey * 0.5f;
    PLFLT zn = (z - p->zmin) / (p->zmax - p->zmin) * p->height;

    return xn * p->saz * p->calt - yn * p->caz * p->calt - zn * p->salt;
}

/** Project four 3D corners and fill the resulting 2D quadrilateral
 *  with @fill, then stroke its outline with @stroke (axis colour).
 *  Caller is responsible for setting line width before calling.
 *  Cmap0 slots 3 (fill) and 4 (stroke) are used as scratch — they
 *  do not collide with the per-series slots (8 +). */
static void
fill_3d_quad (
        const Pn3DProj *p,
        const PLFLT     x[4],
        const PLFLT     y[4],
        const PLFLT     z[4],
        const GdkRGBA  *fill,
        const GdkRGBA  *stroke)
{
    PLFLT xs[4], ys[4];
    PLFLT ox[5], oy[5];
    guint i;

    for (i = 0; i < 4; i++)
        proj_xy (p, x[i], y[i], z[i], &xs[i], &ys[i]);

    paint_set_cmap0 (3, fill);
    plcol0 (3);
    plfill (4, xs, ys);

    for (i = 0; i < 4; i++)
    {
        ox[i] = xs[i];
        oy[i] = ys[i];
    }
    ox[4] = xs[0];
    oy[4] = ys[0];

    paint_set_cmap0 (4, stroke);
    plcol0 (4);
    plline (5, ox, oy);
}

/* ------------------------------------------------------------------ */
/*  Bar sorting helper                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    PLFLT    xl, xr;        /* X (value) bin edges */
    PLFLT    y0, y1;        /* Y (series) ribbon edges */
    PLFLT    zb, zt;        /* Z (count) base and top */
    guint    series_idx;    /* index into views[] for colour lookup */
    PLFLT    depth;         /* center-depth for back-to-front sort */
} PnGraph3DBar;

static gint
bar_depth_cmp (
        gconstpointer a,
        gconstpointer b)
{
    const PnGraph3DBar *pa = a;
    const PnGraph3DBar *pb = b;
    /* Higher depth (further from viewer) first. */
    if (pa->depth < pb->depth) return  1;
    if (pa->depth > pb->depth) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Line-mode painters                                                 */
/* ------------------------------------------------------------------ */

/** Walk @s's bin ring and emit (seconds_ago, mean_value) pairs in
 *  left-to-right time order.  Returns the number of points written
 *  to @xs/@ys, and tracks the y-range in @inout_ymin/@inout_ymax
 *  (only updates them when @have_range_io was already TRUE, otherwise
 *  seeds them with the first value seen). */
static guint
collect_line_points (
        const PnGraphSeries *s,
        guint                n_bins,
        gint64               width,
        gint64               cur_epoch,
        gint64               oldest_valid_epoch,
        PLFLT               *xs,
        PLFLT               *ys,
        gdouble             *inout_ymin,
        gdouble             *inout_ymax,
        gboolean            *have_range_io)
{
    guint  n = 0;
    guint  i;

    for (i = 0; i < n_bins; i++)
    {
        gint64 epoch = oldest_valid_epoch + (gint64) i;
        gint64 slot  = epoch % (gint64) n_bins;
        if (slot < 0) slot += (gint64) n_bins;
        {
            const PnGraphBin *b = &s->ring[slot];
            gdouble           mean;
            gdouble           seconds_ago;

            if (b->epoch != epoch || b->count == 0)
                continue;

            mean = b->sum / (gdouble) b->count;
            seconds_ago = (gdouble) (cur_epoch - epoch)
                        * (gdouble) width
                        / (gdouble) G_TIME_SPAN_SECOND;

            xs[n] = (PLFLT) (-seconds_ago);
            ys[n] = (PLFLT) mean;
            n += 1;

            if (!*have_range_io)
            {
                *inout_ymin = *inout_ymax = mean;
                *have_range_io = TRUE;
            }
            else
            {
                if (b->min < *inout_ymin) *inout_ymin = b->min;
                if (b->max > *inout_ymax) *inout_ymax = b->max;
            }
        }
    }

    return n;
}

/** Like collect_line_points, but for the Error-bars draw-style: walk
 *  @s's bin ring left-to-right (oldest first) and, for every occupied
 *  bucket, emit the box-and-whisker statistics — the bucket mean
 *  (@mean), the mean ±1 standard deviation box edges (@sd_lo/@sd_hi)
 *  and the min/max whisker ends (@w_lo/@w_hi).  The standard deviation
 *  is recovered from the running sum and sum-of-squares as
 *  sqrt(sum_sq/count - mean²), clamped at zero to absorb rounding.
 *  The visible Y range is widened to the whisker extent (min..max) so
 *  the same range logic as the line path applies.  Returns the number
 *  of buckets written. */
static guint
collect_error_bars (
        const PnGraphSeries *s,
        guint                n_bins,
        gint64               width,
        gint64               cur_epoch,
        gint64               oldest_valid_epoch,
        PLFLT               *xs,
        PLFLT               *mean,
        PLFLT               *sd_lo,
        PLFLT               *sd_hi,
        PLFLT               *w_lo,
        PLFLT               *w_hi,
        gdouble             *inout_ymin,
        gdouble             *inout_ymax,
        gboolean            *have_range_io)
{
    guint  n = 0;
    guint  i;

    for (i = 0; i < n_bins; i++)
    {
        gint64 epoch = oldest_valid_epoch + (gint64) i;
        gint64 slot  = epoch % (gint64) n_bins;
        if (slot < 0) slot += (gint64) n_bins;
        {
            const PnGraphBin *b = &s->ring[slot];
            gdouble           m, var, sd, seconds_ago;

            if (b->epoch != epoch || b->count == 0)
                continue;

            m   = b->sum / (gdouble) b->count;
            var = b->sum_sq / (gdouble) b->count - m * m;
            if (var < 0.0) var = 0.0;       /* float noise on flat bins */
            sd  = sqrt (var);

            seconds_ago = (gdouble) (cur_epoch - epoch)
                        * (gdouble) width
                        / (gdouble) G_TIME_SPAN_SECOND;

            xs[n]    = (PLFLT) (-seconds_ago);
            mean[n]  = (PLFLT) m;
            sd_lo[n] = (PLFLT) (m - sd);
            sd_hi[n] = (PLFLT) (m + sd);
            w_lo[n]  = (PLFLT) b->min;
            w_hi[n]  = (PLFLT) b->max;
            n += 1;

            if (!*have_range_io)
            {
                *inout_ymin = b->min;
                *inout_ymax = b->max;
                *have_range_io = TRUE;
            }
            else
            {
                if (b->min < *inout_ymin) *inout_ymin = b->min;
                if (b->max > *inout_ymax) *inout_ymax = b->max;
            }
        }
    }

    return n;
}

/* PLplot plpoin glyph used for the Points draw-style: a filled
 * circle, the most legible marker on the small plot rectangle. */
#define PN_GRAPH_POINT_SYMBOL  17

/** Draw @n (@xs, @ys) points into the current 2D plot window using the
 *  active draw-style.  cmap0 index 1 is the data colour and index 2 the
 *  axis/outline colour (both pinned by paint_setup_page).  For the Bars
 *  style each point becomes a filled rectangle of half-width @bar_hw
 *  rising from @baseline; Points draws one marker per point; Lines a
 *  single polyline.  @baseline / @bar_hw are ignored by the non-bar
 *  styles. */
static void
draw_series_2d (
        PnGraph *self,
        guint    n,
        PLFLT   *xs,
        PLFLT   *ys,
        PLFLT    baseline,
        PLFLT    bar_hw)
{
    guint i;

    switch (self->style)
    {
    case PN_GRAPH_STYLE_POINTS:
        plcol0 (1);
        plssym (0.0, 1.5 + 0.5 * (gdouble) (self->line_width - 1));
        plpoin ((PLINT) n, xs, ys, PN_GRAPH_POINT_SYMBOL);
        break;

    case PN_GRAPH_STYLE_BARS:
        for (i = 0; i < n; i++)
        {
            PLFLT fx[4] = { xs[i] - bar_hw, xs[i] + bar_hw,
                            xs[i] + bar_hw, xs[i] - bar_hw };
            PLFLT fy[4] = { baseline, baseline, ys[i], ys[i] };

            plcol0 (1);
            plfill (4, fx, fy);

            plcol0  (2);
            plwidth (1);
            {
                PLFLT ox[5] = { fx[0], fx[1], fx[2], fx[3], fx[0] };
                PLFLT oy[5] = { fy[0], fy[1], fy[2], fy[3], fy[0] };
                plline (5, ox, oy);
            }
        }
        break;

    case PN_GRAPH_STYLE_LINES:
    default:
        plcol0  (1);
        plwidth ((PLINT) self->line_width);
        plline  ((PLINT) n, xs, ys);
        break;
    }
}

/* Trend colours for the Error-bars draw-style.  A bucket is green when
 * its mean rose versus the previous (older) bucket and red when it fell;
 * the first bucket — which has no predecessor — and a perfectly flat
 * step use a neutral grey so "no change" never reads as a direction. */
static const GdkRGBA pn_graph_trend_up   = { 0.18, 0.70, 0.34, 1.0 };
static const GdkRGBA pn_graph_trend_down = { 0.85, 0.27, 0.27, 1.0 };
static const GdkRGBA pn_graph_trend_flat = { 0.55, 0.55, 0.55, 1.0 };

/** Draw @n buckets as box-and-whisker glyphs for the Error-bars
 *  draw-style.  Each glyph is centred on its bucket's time @xs[i]: a
 *  vertical whisker spans min..max (@w_lo/@w_hi) with short end caps,
 *  a box of half-width @box_hw spans the mean ±1σ (@sd_lo/@sd_hi) filled
 *  in the bucket's trend colour, and a tick marks the mean (@mean).  The
 *  trend colour is green when this bucket's mean exceeds the previous
 *  bucket's, red when it is lower, grey for the first bucket or a flat
 *  step.  cmap0 slots 5/6/7 are pinned to the up/down/flat colours;
 *  slot 2 (the axis colour) is reused for the box outline and mean tick
 *  so they stay legible against either fill. */
static void
draw_error_bars_2d (
        PnGraph     *self,
        guint        n,
        const PLFLT *xs,
        const PLFLT *mean,
        const PLFLT *sd_lo,
        const PLFLT *sd_hi,
        const PLFLT *w_lo,
        const PLFLT *w_hi,
        PLFLT        box_hw)
{
    const PLFLT cap_hw = box_hw * 0.55;
    guint       i;

    paint_set_cmap0 (5, &pn_graph_trend_up);
    paint_set_cmap0 (6, &pn_graph_trend_down);
    paint_set_cmap0 (7, &pn_graph_trend_flat);

    for (i = 0; i < n; i++)
    {
        const PLFLT x = xs[i];
        PLINT       col;

        if (i == 0 || mean[i] == mean[i - 1])
            col = 7;
        else
            col = (mean[i] > mean[i - 1]) ? 5 : 6;

        /* Whisker (min..max) with end caps, drawn first so the box fill
         * covers the segment behind it and only the tails stay visible. */
        plcol0  (col);
        plwidth ((PLINT) self->line_width);
        {
            PLFLT wx[2]   = { x, x };
            PLFLT wy[2]   = { w_lo[i], w_hi[i] };
            PLFLT capx[2] = { x - cap_hw, x + cap_hw };
            PLFLT lo[2]   = { w_lo[i], w_lo[i] };
            PLFLT hi[2]   = { w_hi[i], w_hi[i] };

            plline (2, wx, wy);
            plline (2, capx, lo);
            plline (2, capx, hi);
        }

        /* Box (mean ±1σ) filled in the trend colour, then outlined. */
        {
            PLFLT bx[4] = { x - box_hw, x + box_hw, x + box_hw, x - box_hw };
            PLFLT by[4] = { sd_lo[i], sd_lo[i], sd_hi[i], sd_hi[i] };
            PLFLT ox[5] = { bx[0], bx[1], bx[2], bx[3], bx[0] };
            PLFLT oy[5] = { by[0], by[1], by[2], by[3], by[0] };

            plcol0  (col);
            plfill  (4, bx, by);

            plcol0  (2);
            plwidth (1);
            plline  (5, ox, oy);
        }

        /* Mean tick across the box. */
        {
            PLFLT mx[2] = { x - box_hw, x + box_hw };
            PLFLT my[2] = { mean[i], mean[i] };

            plcol0  (2);
            plwidth ((PLINT) self->line_width);
            plline  (2, mx, my);
        }
    }
}

/** 2D time-series painter — single-series fast path.  Used when only
 *  one topic has ever been seen; with the Lines draw-style it matches
 *  the pre-multi-topic rendering byte-for-byte.  Points and Bars draw
 *  the same (seconds-ago, value) samples as markers / columns. */
static void
paint_timeseries_2d (
        PnGraph             *self,
        cairo_t             *cr,
        double               w,
        double               h,
        const PnGraphSeries *s)
{
    PLFLT     xs[PN_GRAPH_MAX_BINS];
    PLFLT     ys[PN_GRAPH_MAX_BINS];
    /* Error-bars draw-style only: per-bucket ±1σ box edges and
     * min/max whisker ends, filled in parallel with xs/ys. */
    PLFLT     e_sdlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_sdhi[PN_GRAPH_MAX_BINS];
    PLFLT     e_wlo[PN_GRAPH_MAX_BINS];
    PLFLT     e_whi[PN_GRAPH_MAX_BINS];
    gboolean  error_bars = (self->style == PN_GRAPH_STYLE_ERROR_BARS);
    guint     n_bins            = self->n_bins;
    guint     n;
    gint64    width             = bin_width_us (self);
    gint64    cur_epoch         = g_get_monotonic_time () / width;
    gint64    oldest_valid      = cur_epoch - (gint64) n_bins + 1;
    gdouble   ymin = 0.0, ymax = 1.0;
    gboolean  have_range = FALSE;

    if (error_bars)
        n = collect_error_bars (s, n_bins, width, cur_epoch, oldest_valid,
                                xs, ys, e_sdlo, e_sdhi, e_wlo, e_whi,
                                &ymin, &ymax, &have_range);
    else
        n = collect_line_points (s, n_bins, width, cur_epoch, oldest_valid,
                                 xs, ys, &ymin, &ymax, &have_range);

    if (n < 1 || !have_range)
        return;

    if (ymax - ymin < 1e-9)
    {
        gdouble pad = (fabs (ymax) > 1e-9) ? fabs (ymax) * 0.05 : 0.5;
        ymin -= pad;
        ymax += pad;
    }
    else
    {
        gdouble pad = (ymax - ymin) * 0.05;
        ymin -= pad;
        ymax += pad;
    }

    /* Anchor the axis at zero when asked.  Done after padding so the
     * baseline lands exactly on 0 rather than a padded-below value;
     * the data-side bound keeps its headroom.  For a series that
     * straddles zero this is a no-op (0 is already inside the range). */
    if (self->y_from_zero)
    {
        if (ymin > 0.0) ymin = 0.0;
        if (ymax < 0.0) ymax = 0.0;
    }

    paint_setup_page (self, cr, w, h);

    plvpor (0.10, 0.97, 0.18, 0.93);
    plwind ((PLFLT) -(gdouble) resolution_seconds (self->resolution),
            0.0f,
            (PLFLT) ymin, (PLFLT) ymax);

    plslabelfunc (graph_label_format, self);
    plcol0  (2);
    plwidth (1);
    {
        const char *xopts = self->show_grid ? "bcgnsto" : "bcnsto";
        const char *yopts = self->show_grid ? "bcgnsto" : "bcnsto";
        plbox (xopts, 0.0, 0,
               yopts, 0.0, 0);
    }
    plslabelfunc (NULL, NULL);

    /* Bars rise from zero when zero is on screen, otherwise from the
     * bottom of the (auto-ranged) window so they stay visible; columns
     * are centred on each evenly-spaced time bin and given most of the
     * bin width. */
    {
        const PLFLT baseline = (ymin <= 0.0 && 0.0 <= ymax)
                             ? 0.0f : (PLFLT) ymin;
        const PLFLT bar_hw   = (PLFLT) ((gdouble) width
                                        / (gdouble) G_TIME_SPAN_SECOND
                                        * 0.40);
        if (error_bars)
            draw_error_bars_2d (self, n, xs, ys, e_sdlo, e_sdhi,
                                e_wlo, e_whi, bar_hw);
        else
            draw_series_2d (self, n, xs, ys, baseline, bar_hw);
    }

    pleop  ();
    plend1 ();
}

/** 3D line painter — one polyline per series, stacked along the Y
 *  axis at integer slot positions (0, 1, 2, …) in arrival order.  X
 *  is time-before-now (negative), Z is the value.  The first-seen
 *  topic sits closest to the viewer thanks to the azimuth chosen
 *  in PN_GRAPH_3D_AZ. */
static void
paint_line_3d (
        PnGraph                 *self,
        cairo_t                 *cr,
        double                   w,
        double                   h,
        const PnGraphSeriesView *views,
        guint                    nseries)
{
    guint     n_bins            = self->n_bins;
    gint64    width             = bin_width_us (self);
    gint64    cur_epoch         = g_get_monotonic_time () / width;
    gint64    oldest_valid      = cur_epoch - (gint64) n_bins + 1;
    gdouble   zmin = 0.0, zmax = 1.0;
    gboolean  have_range = FALSE;
    guint     i;

    /* Pre-scan every series once so the 3D box has a stable Z range
     * before we draw any polyline.  Without this, the first series
     * would set the box and later series might overflow. */
    PLFLT  *all_xs = g_new (PLFLT, nseries * n_bins);
    PLFLT  *all_ys = g_new (PLFLT, nseries * n_bins);
    PLFLT  *all_zs = g_new (PLFLT, nseries * n_bins);
    guint  *counts = g_new0 (guint, nseries);

    for (i = 0; i < nseries; i++)
    {
        PLFLT *xs = all_xs + i * n_bins;
        PLFLT *zs = all_zs + i * n_bins;
        PLFLT *ys = all_ys + i * n_bins;
        guint  n;
        guint  j;

        /* collect_line_points writes (x=time, y=value) — for 3D we
         * want (x=time, y=topic_index, z=value), so we shuffle the
         * y values into z and overwrite y with the topic-index
         * constant for the whole series. */
        n = collect_line_points (views[i].series, n_bins, width, cur_epoch,
                                 oldest_valid, xs, zs,
                                 &zmin, &zmax, &have_range);
        counts[i] = n;
        for (j = 0; j < n; j++)
            ys[j] = (PLFLT) i;
    }

    if (!have_range)
    {
        g_free (all_xs); g_free (all_ys); g_free (all_zs); g_free (counts);
        return;
    }

    /* Pad the Z (value) range so a flat series doesn't sit on the
     * box floor; matches the 2D padding scheme. */
    if (zmax - zmin < 1e-9)
    {
        gdouble pad = (fabs (zmax) > 1e-9) ? fabs (zmax) * 0.05 : 0.5;
        zmin -= pad;
        zmax += pad;
    }
    else
    {
        gdouble pad = (zmax - zmin) * 0.05;
        zmin -= pad;
        zmax += pad;
    }

    paint_setup_page (self, cr, w, h);

    /* Tighter viewport in 3D mode — plw3d's cube has to fit inside
     * including the deepest series, which would otherwise have its
     * back face clipped against the right edge. */
    plvpor (0.05, 0.97, 0.10, 0.95);

    /* plw3d needs an existing 2D window first — its job is to set
     * up the 3D-to-2D projection inside the already-defined
     * viewport+window pair, so without a preceding plwind the
     * downstream plline3/plpoly3/plbox3 calls bail out with
     * "Please set up window first".  The 2D bounds (-1.0..1.0,
     * -0.9..1.1) are the conventional envelope around a unit
     * isometric cube under the standard alt/az; they leave a sliver
     * of headroom for the back-top axis labels. */
    plwind (-1.0f, 1.0f, -0.9f, 1.1f);

    /* plw3d basex / basey are the floor-cube side lengths in world
     * units; the cube is scaled to fit the viewport.  height is the
     * vertical extent of the cube relative to those sides — picking
     * 1.0 / 0.8 / 1.0 produces a slightly squat box that leaves room
     * for the value-axis labels on the back-left face. */
    plw3d (1.0, 0.8, 1.0,
           (PLFLT) -(gdouble) resolution_seconds (self->resolution),
           0.0f,
           -0.5f, (PLFLT) nseries - 0.5f,
           (PLFLT) zmin, (PLFLT) zmax,
           PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

    /* Axis box for the 3D cube.  "b" draws the bottom-left edge,
     * "n" labels it, "s" adds sub-ticks.  "u" tells plbox3 to draw
     * the axis caption (we leave the caption strings empty so the
     * frame stays clean on the small plot).  X gets time labels via
     * graph_label_format, Y gets integer topic indices (linear ticks
     * spaced at 1 by passing xtick = 1), Z gets value labels via
     * the same formatter. */
    plslabelfunc (graph_label_format, self);
    plcol0  (2);
    plwidth (1);
    {
        const char *xopts = self->show_grid ? "bnstugo" : "bnstuo";
        const char *yopts = "bnstu";
        const char *zopts = self->show_grid ? "bcdmnstuvo" : "bcdmnstuvo";
        plbox3 (xopts, "", 0.0, 0,
                yopts, "", 1.0, 0,
                zopts, "", 0.0, 0);
    }
    plslabelfunc (NULL, NULL);

    /* One polyline per series.  Colour is pinned into cmap0 slot
     * 8 + i before each stroke so plcol0 (slot) picks it up cleanly
     * — cmap0 slot 1 is still occupied by the user's base line
     * colour, slot 2 by the axis colour. */
    plwidth ((PLINT) self->line_width);
    for (i = 0; i < nseries; i++)
    {
        GdkRGBA c;
        if (counts[i] < 2)
            continue;

        color_for_series (self, i, &c);
        paint_set_cmap0 (8 + (PLINT) i, &c);
        plcol0 (8 + (PLINT) i);

        plline3 ((PLINT) counts[i],
                 all_xs + i * n_bins,
                 all_ys + i * n_bins,
                 all_zs + i * n_bins);
    }

    g_free (all_xs); g_free (all_ys); g_free (all_zs); g_free (counts);

    pleop  ();
    plend1 ();
}

/* ------------------------------------------------------------------ */
/*  Histogram-mode painters                                            */
/* ------------------------------------------------------------------ */

/** Count in-window samples for series @s and write the kept values to
 *  @out_values.  Tracks the value range in @inout_vmin / @inout_vmax
 *  the same way collect_line_points tracks the y range — useful for
 *  3D mode where a single shared X (value) axis spans every series. */
static guint
collect_window_values (
        const PnGraphSeries *s,
        gint64               cutoff,
        PLFLT               *out_values,
        guint                cap,
        gdouble             *inout_vmin,
        gdouble             *inout_vmax,
        gboolean            *have_range_io)
{
    guint n = 0;
    guint i;

    for (i = 0; i < s->sample_count && n < cap; i++)
    {
        const PnGraphSample *p = &s->samples[i];
        if (p->time_us < cutoff)
            continue;

        out_values[n++] = (PLFLT) p->value;

        if (!*have_range_io)
        {
            *inout_vmin = *inout_vmax = p->value;
            *have_range_io = TRUE;
        }
        else
        {
            if (p->value < *inout_vmin) *inout_vmin = p->value;
            if (p->value > *inout_vmax) *inout_vmax = p->value;
        }
    }

    return n;
}

/** 2D distribution painter — single-series fast path.  Bins the
 *  in-window samples by value (same bins, same log-y treatment, same
 *  axis options as before) and draws them with the active draw-style:
 *  Bars matches the old histogram byte-for-byte, Lines draws a
 *  frequency polygon through the bin centres, Points marks each
 *  non-empty bin. */
static void
paint_distribution_2d (
        PnGraph             *self,
        cairo_t             *cr,
        double               w,
        double               h,
        const PnGraphSeries *s)
{
    PLFLT    data[PN_GRAPH_SAMPLES];
    guint    n;
    guint    i;
    gint64   now_us  = g_get_monotonic_time ();
    gint64   cutoff  = now_us
                    - (gint64) resolution_seconds (self->resolution)
                    * G_TIME_SPAN_SECOND;
    gdouble  vmin = 0.0, vmax = 0.0;
    gboolean have_range = FALSE;
    gdouble  bin_w;
    guint    counts[PN_GRAPH_HIST_BINS] = { 0 };
    guint    max_count = 0;

    n = collect_window_values (s, cutoff, data, PN_GRAPH_SAMPLES,
                               &vmin, &vmax, &have_range);

    if (n < 2 || !have_range)
        return;

    if (vmax - vmin < 1e-9)
    {
        gdouble pad = (fabs (vmax) > 1e-9) ? fabs (vmax) * 0.05 : 0.5;
        vmin -= pad;
        vmax += pad;
    }

    bin_w = (vmax - vmin) / (gdouble) PN_GRAPH_HIST_BINS;

    for (i = 0; i < n; i++)
    {
        gdouble  rel = (data[i] - vmin) / bin_w;
        gint     b   = (gint) floor (rel);
        if (b < 0)                          b = 0;
        if (b >= (gint) PN_GRAPH_HIST_BINS) b = PN_GRAPH_HIST_BINS - 1;
        counts[b] += 1;
        if (counts[b] > max_count)
            max_count = counts[b];
    }

    paint_setup_page (self, cr, w, h);

    {
        const PLFLT ybot = self->log_y
                         ? (PLFLT) log10 (0.5)
                         : 0.0f;
        const PLFLT ytop = self->log_y
                         ? (PLFLT) log10 ((gdouble) max_count * 1.1 + 1.0)
                         : (PLFLT) ((gdouble) max_count * 1.1 + 1.0);

        plvpor (0.10, 0.97, 0.18, 0.93);
        plwind ((PLFLT) vmin, (PLFLT) vmax, ybot, ytop);

        plslabelfunc (graph_label_format, self);
        plcol0  (2);
        plwidth (1);
        {
            const char *xopts = self->show_grid ? "bcgnsto" : "bcnsto";
            const char *yopts;
            if (self->log_y)
                yopts = self->show_grid ? "bcgnstl"  : "bcnstl";
            else
                yopts = self->show_grid ? "bcgnsto" : "bcnsto";
            plbox (xopts, 0.0, 0,
                   yopts, 0.0, 0);
        }
        plslabelfunc (NULL, NULL);

        /* Error bars are a time-series glyph; in the distribution view
         * there is no per-bucket spread to draw, so fall back to bars. */
        if (self->style == PN_GRAPH_STYLE_BARS
            || self->style == PN_GRAPH_STYLE_ERROR_BARS)
        {
            for (i = 0; i < PN_GRAPH_HIST_BINS; i++)
            {
                PLFLT xl, xr, yt;
                PLFLT xs[4], ys[4];

                if (counts[i] == 0)
                    continue;

                xl = (PLFLT) (vmin + (gdouble) i * bin_w);
                xr = (PLFLT) (vmin + (gdouble) (i + 1) * bin_w);
                yt = self->log_y
                   ? (PLFLT) log10 ((gdouble) counts[i])
                   : (PLFLT) counts[i];

                xs[0] = xl; ys[0] = ybot;
                xs[1] = xr; ys[1] = ybot;
                xs[2] = xr; ys[2] = yt;
                xs[3] = xl; ys[3] = yt;

                plcol0  (1);
                plfill  (4, xs, ys);

                plcol0  (2);
                plwidth (1);
                {
                    PLFLT ox[5] = { xl, xr, xr, xl, xl };
                    PLFLT oy[5] = { ybot, ybot, yt, yt, ybot };
                    plline (5, ox, oy);
                }
            }
        }
        else
        {
            /* Points / Lines: one (bin-centre, height) sample per bin.
             * The frequency polygon (Lines) keeps the empty bins at the
             * floor so the curve dips between clusters; Points skips
             * them so only occupied bins get a marker. */
            PLFLT  cx[PN_GRAPH_HIST_BINS];
            PLFLT  cy[PN_GRAPH_HIST_BINS];
            guint  m = 0;

            for (i = 0; i < PN_GRAPH_HIST_BINS; i++)
            {
                if (self->style == PN_GRAPH_STYLE_POINTS && counts[i] == 0)
                    continue;

                cx[m] = (PLFLT) (vmin + ((gdouble) i + 0.5) * bin_w);
                cy[m] = (counts[i] > 0)
                      ? (self->log_y ? (PLFLT) log10 ((gdouble) counts[i])
                                     : (PLFLT) counts[i])
                      : ybot;
                m += 1;
            }

            if (self->style == PN_GRAPH_STYLE_POINTS)
            {
                plcol0 (1);
                plssym (0.0, 1.5 + 0.5 * (gdouble) (self->line_width - 1));
                plpoin ((PLINT) m, cx, cy, PN_GRAPH_POINT_SYMBOL);
            }
            else
            {
                plcol0  (1);
                plwidth ((PLINT) self->line_width);
                plline  ((PLINT) m, cx, cy);
            }
        }
    }

    pleop  ();
    plend1 ();
}

/** 3D histogram painter — one ribbon of bars per series, stacked
 *  along the Y axis at integer slot positions.  Bins span a single
 *  global [vmin, vmax] computed over every series' in-window samples
 *  so bars from different topics line up vertically and the eye can
 *  compare densities at the same value across series. */
static void
paint_histogram_3d (
        PnGraph                 *self,
        cairo_t                 *cr,
        double                   w,
        double                   h,
        const PnGraphSeriesView *views,
        guint                    nseries)
{
    gint64   now_us = g_get_monotonic_time ();
    gint64   cutoff = now_us
                  - (gint64) resolution_seconds (self->resolution)
                  * G_TIME_SPAN_SECOND;
    gdouble  vmin = 0.0, vmax = 0.0;
    gboolean have_range = FALSE;
    guint    i, j;
    PLFLT   *per_series_data = g_new (PLFLT, nseries * PN_GRAPH_SAMPLES);
    guint   *per_series_n    = g_new0 (guint, nseries);
    guint   *all_counts      = g_new0 (guint, nseries * PN_GRAPH_3D_HIST_BINS);
    guint    max_count       = 0;
    gdouble  bin_w;

    /* Pass 1: collect every in-window sample per series, keeping a
     * single global [vmin, vmax] across the whole plot. */
    for (i = 0; i < nseries; i++)
    {
        per_series_n[i] = collect_window_values (views[i].series, cutoff,
                                                 per_series_data + i * PN_GRAPH_SAMPLES,
                                                 PN_GRAPH_SAMPLES,
                                                 &vmin, &vmax, &have_range);
    }

    if (!have_range)
    {
        g_free (per_series_data); g_free (per_series_n); g_free (all_counts);
        return;
    }

    if (vmax - vmin < 1e-9)
    {
        gdouble pad = (fabs (vmax) > 1e-9) ? fabs (vmax) * 0.05 : 0.5;
        vmin -= pad;
        vmax += pad;
    }

    bin_w = (vmax - vmin) / (gdouble) PN_GRAPH_3D_HIST_BINS;

    /* Pass 2: bin every series independently, but using the shared
     * bin edges from above so bin index N has the same value range
     * in every ribbon. */
    for (i = 0; i < nseries; i++)
    {
        guint  n  = per_series_n[i];
        PLFLT *vs = per_series_data + i * PN_GRAPH_SAMPLES;
        guint *cs = all_counts      + i * PN_GRAPH_3D_HIST_BINS;

        for (j = 0; j < n; j++)
        {
            gdouble rel = ((gdouble) vs[j] - vmin) / bin_w;
            gint    b   = (gint) floor (rel);
            if (b < 0)                             b = 0;
            if (b >= (gint) PN_GRAPH_3D_HIST_BINS) b = PN_GRAPH_3D_HIST_BINS - 1;
            cs[b] += 1;
            if (cs[b] > max_count)
                max_count = cs[b];
        }
    }

    if (max_count == 0)
    {
        g_free (per_series_data); g_free (per_series_n); g_free (all_counts);
        return;
    }

    paint_setup_page (self, cr, w, h);

    {
        const PLFLT  zbot = self->log_y
                          ? (PLFLT) log10 (0.5)
                          : 0.0f;
        const PLFLT  ztop = self->log_y
                          ? (PLFLT) log10 ((gdouble) max_count * 1.1 + 1.0)
                          : (PLFLT) ((gdouble) max_count * 1.1 + 1.0);
        const PLFLT  ymin = -0.5f;
        const PLFLT  ymax = (PLFLT) nseries - 0.5f;
        Pn3DProj     proj;
        GArray      *bars;
        guint        b;

        plvpor (0.05, 0.97, 0.10, 0.95);
        /* Same plwind-before-plw3d pattern as paint_line_3d — without
         * the 2D window, plbox3/plpoly3/plline3 all abort with
         * "Please set up window first". */
        plwind (-1.0f, 1.0f, -0.9f, 1.1f);
        plw3d (1.0, 0.6, 1.0,
               (PLFLT) vmin, (PLFLT) vmax,
               ymin, ymax,
               zbot, ztop,
               PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

        /* Cache the same projection PLplot uses internally so the
         * manually-filled face quads land in the exact same 2D world
         * coordinates as the plbox3 / plline3 wireframe overlaid on
         * top.  Without an exact match the box edges would float
         * slightly off the bar corners. */
        proj_init (&proj, 1.0, 0.6, 1.0,
                   (PLFLT) vmin, (PLFLT) vmax,
                   ymin, ymax,
                   zbot, ztop,
                   PN_GRAPH_3D_ALT, PN_GRAPH_3D_AZ);

        /* Floor: filled light-grey rectangle at z = zbot.  This is
         * what gives the cube its "ground plane" — without it the
         * bars look like they're floating in space because nothing
         * defines the bottom of the box. */
        {
            const GdkRGBA floor = { 0.92, 0.92, 0.92, 1.0 };
            PLFLT fx[4] = { (PLFLT) vmin, (PLFLT) vmax,
                            (PLFLT) vmax, (PLFLT) vmin };
            PLFLT fy[4] = { ymin, ymin, ymax, ymax };
            PLFLT fz[4] = { zbot, zbot, zbot, zbot };
            PLFLT sx[4], sy[4];
            guint k;

            for (k = 0; k < 4; k++)
                proj_xy (&proj, fx[k], fy[k], fz[k], &sx[k], &sy[k]);
            paint_set_cmap0 (5, &floor);
            plcol0 (5);
            plfill (4, sx, sy);
        }

        /* Axes go down FIRST so the bars cover the box edges that
         * pass behind them — matches gnuplot's convention where the
         * back-face axis grid reads as "behind" the bars rather than
         * cutting through them.  The front-edge axis lines that the
         * bars do not occlude remain visible because they sit on the
         * far side of the cube from the bar fills.
         *
         * Options: 'bc' draws both the front-bottom and back-bottom
         * edge of the X and Y axes so the floor reads as a closed
         * square instead of an L-shape; 'bcd' on the Z axis draws
         * three of the four vertical cube edges (the fourth, at
         * x_max y_min, is drawn manually below for completeness).
         * 'm' (mirror labels on opposite side) is intentionally
         * omitted from the Z options — the previous render duplicated
         * the count labels on both left and right, which read as a
         * second axis floating off to the right of the cube. */
        plslabelfunc (graph_label_format, self);
        plcol0  (2);
        plwidth (1);
        {
            const char *xopts = self->show_grid ? "bcnstugo" : "bcnstuo";
            const char *yopts = self->show_grid ? "bcnstug"  : "bcnstu";
            const char *zopts;
            if (self->log_y)
                zopts = self->show_grid ? "bcdnstuvl" : "bcdnstuvl";
            else
                zopts = self->show_grid ? "bcdnstuvo" : "bcdnstuvo";
            plbox3 (xopts, "", 0.0, 0,
                    yopts, "", 1.0, 0,
                    zopts, "", 0.0, 0);
        }
        plslabelfunc (NULL, NULL);

        /* Build the bar list with depth keys, then sort back-to-
         * front.  Painter's algorithm: drawing the deepest bar first
         * lets the closer bars overpaint anything that would have
         * peeked through.  Without sorting, a back-row bar can end
         * up drawn on top of a front-row bar simply because of
         * iteration order.
         *
         * Ribbon half-width PN_GRAPH_3D_RIBBON_HALF (so each topic's
         * ribbon takes 2× that fraction of its 1.0-unit Y slot, with
         * the remainder as a gap between adjacent topics).  Each bar
         * also takes only PN_GRAPH_3D_BAR_FILL of its bin's X-width,
         * with the remainder split as a horizontal gutter on either
         * side — without this gutter, adjacent bars in the same
         * series share an edge and the row reads as a continuous
         * slab rather than separate boxes. */
        bars = g_array_new (FALSE, FALSE, sizeof (PnGraph3DBar));
        for (i = 0; i < nseries; i++)
        {
            const PLFLT y0 = (PLFLT) i - (PLFLT) PN_GRAPH_3D_RIBBON_HALF;
            const PLFLT y1 = (PLFLT) i + (PLFLT) PN_GRAPH_3D_RIBBON_HALF;
            const gdouble gap_half = (1.0 - PN_GRAPH_3D_BAR_FILL) * 0.5;
            guint      *cs = all_counts + i * PN_GRAPH_3D_HIST_BINS;

            for (j = 0; j < PN_GRAPH_3D_HIST_BINS; j++)
            {
                PnGraph3DBar bar;
                PLFLT        xc, yc;

                if (cs[j] == 0)
                    continue;

                bar.xl = (PLFLT) (vmin + ((gdouble) j + gap_half) * bin_w);
                bar.xr = (PLFLT) (vmin + ((gdouble) (j + 1) - gap_half) * bin_w);
                bar.y0 = y0;
                bar.y1 = y1;
                bar.zb = zbot;
                bar.zt = self->log_y
                       ? (PLFLT) log10 ((gdouble) cs[j])
                       : (PLFLT) cs[j];
                bar.series_idx = i;

                xc = (bar.xl + bar.xr) * 0.5f;
                yc = (bar.y0 + bar.y1) * 0.5f;
                bar.depth = proj_depth (&proj, xc, yc,
                                        (bar.zb + bar.zt) * 0.5f);

                g_array_append_val (bars, bar);
            }
        }

        g_array_sort (bars, bar_depth_cmp);

        /* Draw only the THREE visible faces per bar — the y_min,
         * x_min, and z_min faces are occluded by the bar itself
         * under the fixed PN_GRAPH_3D_ALT/AZ view.  PLplot's azimuth
         * convention puts the camera at the +y axis for az=0 and
         * rotates CCW around z as az grows, so the viewer for our
         * az=300° / alt=45° sits in the (+x, +y, +z) octant and the
         * faces that point toward the viewer are
         *   x = x_max (the bar's "right" face),
         *   y = y_max (the bar's "back" face — i.e. the world-y_max
         *              face is the one closest to a viewer at +y),
         *   z = z_max (the top).
         * An earlier version painted the y_min face by mistake (it
         * had the y-direction of the camera vector inverted), which
         * made the visible y-side of every bar look like the "back"
         * of the bar — the dark face the user shouldn't have been
         * seeing.  Drawing only these three avoids the stray
         * hidden-edge artifacts that an all-five-faces render
         * produces, since hidden-face outlines would extend past
         * the visible silhouette and survive as bogus lines that
         * don't align with the cube axes.
         *
         * Within the three visible faces the draw order is
         *   right (x=xr) → back-but-visible (y=y1) → top (z=zt)
         * which is the depth-sorted back-to-front order for the
         * current alt/az (xr is the deepest of the three, top the
         * shallowest), so the side and y faces stitch together along
         * their shared vertical edge and the top sits on both.
         *
         * Shading uses absolute HLS lightness: top L=0.75 (brightest,
         * the lit face), close-y face L=0.55 (catches a sidelight),
         * right L=0.40 (more shadowed).  Picking absolute lightnesses
         * rather than multiplying the base colour gives consistent
         * three-face contrast regardless of how light or dark the
         * user's chosen line-color is.
         *
         * BETWEEN bars, the depth sort above puts higher-depth (far)
         * bars first, so painter's algorithm runs back-to-front and
         * close bars correctly overpaint far ones where the screen
         * footprints overlap. */
        plwidth (1);
        for (b = 0; b < bars->len; b++)
        {
            const PnGraph3DBar *bar = &g_array_index (bars, PnGraph3DBar, b);
            GdkRGBA             base;
            GdkRGBA             c_top, c_yface, c_right;

            color_for_series (self, bar->series_idx, &base);
            c_top   = shade_l (base, 0.75);
            c_yface = shade_l (base, 0.55);
            c_right = shade_l (base, 0.40);

            /* Right face (x = xr): the close-x face for viewer at +x. */
            {
                PLFLT fx[4] = { bar->xr, bar->xr, bar->xr, bar->xr };
                PLFLT fy[4] = { bar->y0, bar->y1, bar->y1, bar->y0 };
                PLFLT fz[4] = { bar->zb, bar->zb, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_right, &self->axis_color);
            }
            /* Y-max face (y = y1): the close-y face for viewer at +y.
             * Drawn after the right face so the shared back-right
             * vertical edge is stroked in this face's outline. */
            {
                PLFLT fx[4] = { bar->xl, bar->xr, bar->xr, bar->xl };
                PLFLT fy[4] = { bar->y1, bar->y1, bar->y1, bar->y1 };
                PLFLT fz[4] = { bar->zb, bar->zb, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_yface, &self->axis_color);
            }
            /* Top face (z = zt): drawn last so it sits on top of the
             * shared top edges of the two side faces. */
            {
                PLFLT fx[4] = { bar->xl, bar->xr, bar->xr, bar->xl };
                PLFLT fy[4] = { bar->y0, bar->y0, bar->y1, bar->y1 };
                PLFLT fz[4] = { bar->zt, bar->zt, bar->zt, bar->zt };
                fill_3d_quad (&proj, fx, fy, fz, &c_top, &self->axis_color);
            }
        }

        g_array_unref (bars);
    }

    g_free (per_series_data); g_free (per_series_n); g_free (all_counts);

    pleop  ();
    plend1 ();
}

/* ------------------------------------------------------------------ */
/*  Top-level dispatch                                                 */
/* ------------------------------------------------------------------ */

/** Render the plot rectangle (background, axes, polyline or bars)
 *  into @cr, anchored at (@x, @y) with size @w × @h.  Wired as the
 *  PnNodeClass::paint_plot vfunc and dispatched by the worksheet's
 *  draw_node post-pass and zoom overlay; safe to call when no data
 *  is available — the rectangle is filled and the frame stroked,
 *  but no curve or bars are drawn.
 *
 *  Series count drives the 2D-vs-3D split: a single topic (which
 *  includes the never-set-topic case, since the sentinel "" is just
 *  another key) renders identically to the pre-multi-topic version
 *  of the node; two or more topics switch to the 3D painter so each
 *  series gets its own Y-axis slot. */
static void
pn_graph_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnGraph           *self    = PN_GRAPH (node);
    PnGraphSeriesView *views   = NULL;
    guint              nseries = 0;

    /* Plot background and frame are user-tunable.  Even with no data
     * the rectangle is still painted so the empty plot reads as a
     * deliberate, visible area rather than a hole in the worksheet. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, &self->background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, &self->axis_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    views = collect_series_sorted (self, &nseries);
    if (nseries == 0)
        return;

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    switch (self->view)
    {
    case PN_GRAPH_VIEW_DISTRIBUTION:
        if (nseries == 1)
            paint_distribution_2d (self, cr, w, h, views[0].series);
        else
            paint_histogram_3d (self, cr, w, h, views, nseries);
        break;
    case PN_GRAPH_VIEW_TIME_SERIES:
    default:
        if (nseries == 1)
            paint_timeseries_2d (self, cr, w, h, views[0].series);
        else
            paint_line_3d (self, cr, w, h, views, nseries);
        break;
    }

    cairo_restore (cr);

    g_free (views);
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
            const GdkRGBA *c = g_value_get_boxed (value);
            if (c != NULL && !gdk_rgba_equal (c, &self->line_color))
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
            const GdkRGBA *c = g_value_get_boxed (value);
            if (c != NULL && !gdk_rgba_equal (c, &self->axis_color))
            {
                self->axis_color = *c;
                g_object_notify_by_pspec (object, props[PROP_AXIS_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BACKGROUND_COLOR:
        {
            const GdkRGBA *c = g_value_get_boxed (value);
            if (c != NULL && !gdk_rgba_equal (c, &self->background_color))
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

    if (self->plstream_valid)
    {
        plsstrm (self->plstream);
        plend1 ();
        self->plstream_valid = FALSE;
    }

    g_clear_pointer (&self->series, g_hash_table_unref);
    g_clear_pointer (&self->key,    g_free);

    G_OBJECT_CLASS (pn_graph_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: two themed pages instead of one tall tab          */
/* ------------------------------------------------------------------ */

/* Build one property grid from a NULL-terminated list of property
 * names, skipping any that are missing or read-only.  Mirrors the
 * per-page helper used by the other multi-page nodes (e.g. PnDial). */
static GtkWidget *
build_graph_page (GObject *target, const gchar *const *names)
{
    GtkWidget    *grid  = pn_node_dialog_new_property_grid ();
    GObjectClass *klass = G_OBJECT_GET_CLASS (target);
    guint         row   = 0;
    guint         i;

    for (i = 0; names[i] != NULL; i++)
    {
        GParamSpec  *pspec = g_object_class_find_property (klass, names[i]);
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
pn_graph_build_class_tabs (
        PnNode      *self,
        GtkNotebook *notebook,
        GtkWindow   *parent G_GNUC_UNUSED)
{
    GObject *target = G_OBJECT (self);

    /* How the plot is drawn. */
    static const gchar *const appearance_props[] = {
        "draw-style",
        "line-color", "line-width",
        "axis-color", "background-color",
        "show-grid",
        NULL
    };
    /* What is plotted and how the value axis is framed. */
    static const gchar *const data_props[] = {
        "key", "resolution", "x-buckets", "data-view",
        "log-y", "y-from-zero",
        NULL
    };

    pn_node_dialog_append_page (notebook,
                                build_graph_page (target, appearance_props),
                                "Appearance");
    pn_node_dialog_append_page (notebook,
                                build_graph_page (target, data_props),
                                "Data");
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
    node_class->paint_plot        = pn_graph_paint_plot;
    node_class->build_class_tabs  = pn_graph_build_class_tabs;

    node_class->class_name        = "Graph";
    node_class->icon              = "\xef\x87\xbe";  /* fa-area-chart U+F1FE */
    node_class->color             = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
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
            "min/max, a box over the mean ±1 standard deviation, and a "
            "mean tick; each bucket is green when its mean rose versus "
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
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_WIDTH] = g_param_spec_uint (
            "line-width", "Line width",
            "Stroke width of the data polyline, in pixels",
            1, 8, 2,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_AXIS_COLOR] = g_param_spec_boxed (
            "axis-color", "Axis colour",
            "Colour of the plot frame, ticks, and tick labels",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the plot rectangle behind the polyline",
            GDK_TYPE_RGBA,
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
    props[PROP_MODE] = g_param_spec_enum (
            "mode", NULL, NULL,
            PN_TYPE_GRAPH_MODE,
            PN_GRAPH_MODE_LINE,
            G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_graph_init (PnGraph *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  yellow = { 0.92, 0.76, 0.27, 1.0 };

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
    self->line_color       = (GdkRGBA) {  30.0/255.0,  60.0/255.0, 140.0/255.0, 1.0 };
    self->axis_color       = (GdkRGBA) {  70.0/255.0,  70.0/255.0,  70.0/255.0, 1.0 };
    self->background_color = (GdkRGBA) { 1.0, 1.0, 1.0, 1.0 };
    self->line_width       = 2;
    self->show_grid        = FALSE;
    self->log_y            = FALSE;
    self->y_from_zero      = FALSE;

    self->series = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, series_free);
    self->next_arrival_idx = 0;

    /* Allocate a private PLplot stream so multiple graphs on the
     * same canvas don't share state.  The stream id stays valid for
     * the node's whole lifetime. */
    plmkstrm (&self->plstream);
    self->plstream_valid = TRUE;

    pn_node_set_class_name (node, "Graph");
    pn_node_set_icon       (node, "\xef\x87\xbe");  /* fa-area-chart U+F1FE */
    pn_node_set_color      (node, &yellow);
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
