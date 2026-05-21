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

#include "pn-stats.h"
#include "pn-message.h"

#define PN_STATS_WINDOW_MIN  1u
#define PN_STATS_WINDOW_MAX  86400u    /* 24 hours */
#define PN_STATS_WINDOW_DEF  60u

#define PN_STATS_BINS_MIN    1u
#define PN_STATS_BINS_MAX    3600u
#define PN_STATS_BINS_DEF    60u

/* ------------------------------------------------------------------ */
/*  Per-bin state                                                      */
/*                                                                     */
/*  The window is split into a fixed number of equal-sized bins kept   */
/*  in a circular buffer.  Each bin only stores summary tallies — we   */
/*  never retain individual messages or even individual timestamps —   */
/*  so memory use is O(bins) regardless of arrival rate or payload     */
/*  size.  Tradeoff: the window slides one bin at a time, not one      */
/*  message at a time.                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Absolute bin index (now_us / bin_width_us) the slot represents.
     * Lets a single circular buffer outlive many full window passes:
     * before touching a slot we compare its epoch to the current one
     * and reset it lazily if it is stale.  Initialised to G_MININT64
     * so an uninitialised slot never matches a real epoch. */
    gint64   epoch;

    /* Messages whose arrival timestamp landed in this bin. */
    guint64  count;

    /* Inter-arrival gaps whose *second* arrival landed in this bin.
     * Equals @count except in the very first bin of the node's
     * lifetime, where the very first message has no preceding gap. */
    guint64  gap_count;

    /* Tallies over the gaps counted in @gap_count, in microseconds. */
    gint64   sum_gap_us;
    gint64   min_gap_us;
    gint64   max_gap_us;
} PnStatsBin;

struct _PnStats
{
    PnNode parent_instance;

    /* Window length in seconds. */
    guint        window;

    /* Number of bins the window is split into.  Memory footprint is
     * dominated by @bins × sizeof(PnStatsBin); the value range is
     * deliberately capped so even a misconfigured node stays well
     * under a megabyte. */
    guint        bins;

    /* Most recent arrival's monotonic timestamp.  Carries across bins
     * so a gap that exceeds a single bin's width is still recorded as
     * a single value at the bin where its second arrival landed —
     * including gaps that exceed the entire window. */
    gboolean     has_last;
    gint64       last_us;

    /* Circular buffer of @bins entries.  Owned by the node; freed and
     * reallocated whenever @window or @bins changes. */
    PnStatsBin  *ring;
};

G_DEFINE_TYPE (PnStats, pn_stats, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_WINDOW,
    PROP_BINS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Width of a single bin in microseconds.  Always at least 1us so the
 *  modulo arithmetic in bin selection never divides by zero, even at
 *  the smallest legal window/bins combination. */
static gint64
bin_width_us (PnStats *self)
{
    gint64 w = (gint64) self->window * G_TIME_SPAN_SECOND
             / (gint64) self->bins;
    return w > 0 ? w : 1;
}

/** Bring @bin into a fresh state representing @epoch.  Called lazily
 *  when a stale slot is about to be touched, so bins go through one
 *  cheap memset per window pass instead of a sweep on every receive. */
static void
reset_bin (
        PnStatsBin *bin,
        gint64      epoch)
{
    bin->epoch      = epoch;
    bin->count      = 0;
    bin->gap_count  = 0;
    bin->sum_gap_us = 0;
    bin->min_gap_us = 0;
    bin->max_gap_us = 0;
}

/** Drop the existing ring (if any) and allocate a fresh one sized to
 *  the current @bins.  Sets every slot's epoch to a sentinel that no
 *  real epoch can match so aggregation skips them until they are
 *  legitimately filled.  Also clears @has_last because a window or
 *  bin-count change invalidates the gap that would otherwise straddle
 *  the boundary. */
static void
realloc_ring (PnStats *self)
{
    guint i;

    g_free (self->ring);
    self->ring = g_new0 (PnStatsBin, self->bins);

    for (i = 0; i < self->bins; i++)
        self->ring[i].epoch = G_MININT64;

    self->has_last = FALSE;
    self->last_us  = 0;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_stats_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnStats     *self = PN_STATS (node);
    PnMessage   *out;
    PnStatsBin  *bin;
    gint64       now;
    gint64       width;
    gint64       cur_epoch;
    gint64       oldest_valid_epoch;
    guint        slot;
    guint        i;

    guint64      total_count     = 0;
    guint64      total_gap_count = 0;
    gint64       total_sum_gap   = 0;
    gint64       min_gap_us      = 0;
    gint64       max_gap_us      = 0;
    gboolean     have_gap        = FALSE;

    gdouble      min_s = 0.0;
    gdouble      max_s = 0.0;
    gdouble      avg_s = 0.0;
    gdouble      per_minute = 0.0;
    gdouble      per_hour   = 0.0;
    gdouble      per_day    = 0.0;

    (void) message;

    now       = g_get_monotonic_time ();
    width     = bin_width_us (self);
    cur_epoch = now / width;
    slot      = (guint) (cur_epoch % self->bins);
    bin       = &self->ring[slot];

    /* Stale slot — its data belongs to a previous pass through the
     * ring.  Reset before tallying so we don't conflate windows. */
    if (bin->epoch != cur_epoch)
        reset_bin (bin, cur_epoch);

    bin->count += 1;

    /* Record the inter-arrival gap against the bin where the *second*
     * arrival landed.  Gaps that straddle bin boundaries are still a
     * single measurement, attributed to the ending bin. */
    if (self->has_last)
    {
        gint64 gap = now - self->last_us;

        if (bin->gap_count == 0 || gap < bin->min_gap_us)
            bin->min_gap_us = gap;
        if (gap > bin->max_gap_us)
            bin->max_gap_us = gap;
        bin->sum_gap_us += gap;
        bin->gap_count  += 1;
    }

    self->has_last = TRUE;
    self->last_us  = now;

    /* Aggregate across every bin whose epoch lies within the live
     * window — that is, the K most recent epochs.  Stale slots from
     * earlier passes are filtered out by the epoch range check. */
    oldest_valid_epoch = cur_epoch - (gint64) self->bins + 1;

    for (i = 0; i < self->bins; i++)
    {
        PnStatsBin *b = &self->ring[i];

        if (b->epoch < oldest_valid_epoch || b->epoch > cur_epoch)
            continue;

        total_count += b->count;

        if (b->gap_count > 0)
        {
            total_gap_count += b->gap_count;
            total_sum_gap   += b->sum_gap_us;

            if (!have_gap || b->min_gap_us < min_gap_us)
                min_gap_us = b->min_gap_us;
            if (!have_gap || b->max_gap_us > max_gap_us)
                max_gap_us = b->max_gap_us;
            have_gap = TRUE;
        }
    }

    if (have_gap)
    {
        min_s = (gdouble) min_gap_us / (gdouble) G_TIME_SPAN_SECOND;
        max_s = (gdouble) max_gap_us / (gdouble) G_TIME_SPAN_SECOND;
        avg_s = ((gdouble) total_sum_gap / (gdouble) G_TIME_SPAN_SECOND)
              / (gdouble) total_gap_count;
    }

    /* Rate is reported relative to the configured window length so a
     * downstream graph plots a directly meaningful "messages in the
     * last N seconds" curve, regardless of how full the window is. */
    {
        gdouble rate_per_s = (gdouble) total_count / (gdouble) self->window;
        per_minute = rate_per_s * 60.0;
        per_hour   = rate_per_s * 3600.0;
        per_day    = rate_per_s * 86400.0;
    }

    out = pn_message_new (node, NULL);
    pn_message_set_int64   (out, "count",           (gint64) total_count);
    pn_message_set_double  (out, "min_seconds",     min_s);
    pn_message_set_double  (out, "max_seconds",     max_s);
    pn_message_set_double  (out, "average_seconds", avg_s);
    pn_message_set_double  (out, "per_minute",      per_minute);
    pn_message_set_double  (out, "per_hour",        per_hour);
    pn_message_set_double  (out, "per_day",         per_day);

    pn_node_emit_message (node, out);
    g_object_unref (out);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_stats_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnStats *self = PN_STATS (object);

    switch (prop_id)
    {
    case PROP_WINDOW:
        g_value_set_uint (value, self->window);
        break;
    case PROP_BINS:
        g_value_set_uint (value, self->bins);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_stats_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnStats *self = PN_STATS (object);

    switch (prop_id)
    {
    case PROP_WINDOW:
        {
            guint v = g_value_get_uint (value);
            if (self->window != v)
            {
                self->window = v;
                /* The bin width is derived from the window, so every
                 * stored epoch is now in a different namespace.  Drop
                 * the ring rather than try to migrate per-bin tallies
                 * across the change. */
                realloc_ring (self);
                g_object_notify_by_pspec (object, props[PROP_WINDOW]);
            }
        }
        break;
    case PROP_BINS:
        {
            guint v = g_value_get_uint (value);
            if (self->bins != v)
            {
                self->bins = v;
                realloc_ring (self);
                g_object_notify_by_pspec (object, props[PROP_BINS]);
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
pn_stats_finalize (GObject *object)
{
    PnStats *self = PN_STATS (object);

    g_clear_pointer (&self->ring, g_free);

    G_OBJECT_CLASS (pn_stats_parent_class)->finalize (object);
}

static void
pn_stats_class_init (PnStatsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_stats_get_property;
    object_class->set_property = pn_stats_set_property;
    object_class->finalize     = pn_stats_finalize;
    node_class->receive        = pn_stats_receive;

    node_class->class_name     = "Stats";
    node_class->icon           = "\xef\x82\x80";  /* fa-bar-chart U+F080 */
    node_class->color          = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_WINDOW] = g_param_spec_uint (
            "window", "Window",
            "Length of the rolling window in seconds — every emitted "
            "statistics message summarises only arrivals during the "
            "previous @window seconds",
            PN_STATS_WINDOW_MIN,
            PN_STATS_WINDOW_MAX,
            PN_STATS_WINDOW_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BINS] = g_param_spec_uint (
            "bins", "Bins",
            "Number of equal-sized buckets the window is split into. "
            "Memory use is O(bins); the window slides one bin at a "
            "time rather than one message at a time",
            PN_STATS_BINS_MIN,
            PN_STATS_BINS_MAX,
            PN_STATS_BINS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_stats_init (PnStats *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->window   = PN_STATS_WINDOW_DEF;
    self->bins     = PN_STATS_BINS_DEF;
    self->ring     = NULL;
    self->has_last = FALSE;
    self->last_us  = 0;

    realloc_ring (self);

    pn_node_set_class_name (node, "Stats");
    pn_node_set_icon       (node, "\xef\x82\x80");  /* fa-bar-chart U+F080 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnStats *
pn_stats_new (void)
{
    return g_object_new (PN_TYPE_STATS, NULL);
}
