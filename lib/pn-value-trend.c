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

#include "pn-value-trend.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

/* The deadband is a non-negative width, defaulting to zero so a fresh
 * node splits on any change at all. */
#define PN_VALUE_TREND_BAND_MIN  ( 0.0)
#define PN_VALUE_TREND_BAND_MAX  ( 1e9)
#define PN_VALUE_TREND_BAND_DEF  ( 0.0)

struct _PnValueTrend
{
    PnNode parent_instance;

    /* Deadband width; moves smaller than this do not trip. */
    gdouble  min_change;

    /* Whether the third "unchanged" output exists. */
    gboolean unchanged_output;

    /* The last value FORWARDED (not the last seen), and whether we
     * have one yet.  Measuring from the forwarded value is what lets a
     * slow drift of sub-band steps accumulate until it trips. */
    gboolean has_reference;
    gdouble  reference;
};

G_DEFINE_TYPE (PnValueTrend, pn_value_trend, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_MIN_CHANGE,
    PROP_UNCHANGED_OUTPUT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Classification                                                     */
/* ------------------------------------------------------------------ */

PnValueTrendDirection
pn_value_trend_classify (
        gdouble prev,
        gdouble cur,
        gdouble band)
{
    gdouble delta = cur - prev;

    if (!(band > 0.0))
        band = 0.0;

    /* A move of exactly the band width counts: the band is what is
     * ignored, not what is required to be exceeded.  With band 0 the
     * strict delta test keeps an equal value "unchanged". */
    if (delta > 0.0 && delta >= band)
        return PN_VALUE_TREND_RISING;
    if (delta < 0.0 && -delta >= band)
        return PN_VALUE_TREND_FALLING;

    return PN_VALUE_TREND_UNCHANGED;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Pull the number out of @message under "value".  %TRUE only when the
 *  member exists and holds a JSON int, double or boolean (booleans read
 *  as 1.0 / 0.0, the way pipnode carries on/off state); @out is left
 *  untouched otherwise. */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt == G_TYPE_BOOLEAN)
        *out = json_node_get_boolean (node) ? 1.0 : 0.0;
    else if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
        *out = json_node_get_double (node);
    else
        return FALSE;

    return TRUE;
}

static void
sync_outputs (PnValueTrend *self)
{
    PnNode *node = PN_NODE (self);

    pn_node_set_n_outputs  (node, self->unchanged_output ? 3 : 2);
    pn_node_set_output_name (node, PN_VALUE_TREND_OUT_RISING,  "rising");
    pn_node_set_output_name (node, PN_VALUE_TREND_OUT_FALLING, "falling");
    if (self->unchanged_output)
        pn_node_set_output_name (node, PN_VALUE_TREND_OUT_UNCHANGED,
                                 "unchanged");
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_value_trend_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnValueTrend          *self = PN_VALUE_TREND (node);
    gdouble                value;
    PnValueTrendDirection  dir;

    /* Nothing usable to compare — stay silent and keep the reference,
     * so a stray text message does not break a running trend. */
    if (!read_value (message, &value))
        return;

    if (!self->has_reference)
    {
        /* Seeds the reference; there is no previous value this one
         * could be rising or falling against. */
        self->has_reference = TRUE;
        self->reference     = value;
        return;
    }

    dir = pn_value_trend_classify (self->reference, value, self->min_change);

    if (dir == PN_VALUE_TREND_UNCHANGED)
    {
        /* The reference deliberately stays put: the next message is
         * measured against the last value that actually left, so many
         * sub-band steps in one direction still add up to a trip. */
        if (self->unchanged_output)
            pn_node_emit_message_on_output (node, message,
                                            PN_VALUE_TREND_OUT_UNCHANGED);
        return;
    }

    self->reference = value;
    pn_node_emit_message_on_output (
            node, message,
            dir == PN_VALUE_TREND_RISING ? PN_VALUE_TREND_OUT_RISING
                                         : PN_VALUE_TREND_OUT_FALLING);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_value_trend_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnValueTrend *self = PN_VALUE_TREND (object);

    switch (prop_id)
    {
    case PROP_MIN_CHANGE:
        g_value_set_double (value, self->min_change);
        break;
    case PROP_UNCHANGED_OUTPUT:
        g_value_set_boolean (value, self->unchanged_output);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_value_trend_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnValueTrend *self = PN_VALUE_TREND (object);

    switch (prop_id)
    {
    case PROP_MIN_CHANGE:
        {
            gdouble v = g_value_get_double (value);
            if (self->min_change != v)
            {
                self->min_change = v;
                g_object_notify_by_pspec (object, props[PROP_MIN_CHANGE]);
            }
        }
        break;
    case PROP_UNCHANGED_OUTPUT:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->unchanged_output != v)
            {
                self->unchanged_output = v;
                sync_outputs (self);
                g_object_notify_by_pspec (object,
                                          props[PROP_UNCHANGED_OUTPUT]);
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
pn_value_trend_class_init (PnValueTrendClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_value_trend_get_property;
    object_class->set_property = pn_value_trend_set_property;
    node_class->receive        = pn_value_trend_receive;

    node_class->class_name     = "Value Trend";
    node_class->icon           = "\xef\x83\x9c";  /* fa-sort U+F0DC */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Gate";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_MIN_CHANGE] = g_param_spec_double (
            "min-change", "Minimum change",
            "Deadband: moves smaller than this are not a rise or a fall. "
            "Measured from the last forwarded value, so a slow drift of "
            "smaller steps still eventually trips. 0 splits on any change.",
            PN_VALUE_TREND_BAND_MIN,
            PN_VALUE_TREND_BAND_MAX,
            PN_VALUE_TREND_BAND_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    props[PROP_UNCHANGED_OUTPUT] = g_param_spec_boolean (
            "unchanged-output", "Unchanged output",
            "Add a third output carrying the messages that neither rose "
            "nor fell, instead of dropping them. The first message, which "
            "has nothing to compare against, is dropped either way.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_value_trend_init (PnValueTrend *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->min_change       = PN_VALUE_TREND_BAND_DEF;
    self->unchanged_output = FALSE;
    self->has_reference    = FALSE;
    self->reference        = 0.0;

    pn_node_set_class_name (node, "Value Trend");
    pn_node_set_icon       (node, "\xef\x83\x9c");  /* fa-sort U+F0DC */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    sync_outputs (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnValueTrend *
pn_value_trend_new (void)
{
    return g_object_new (PN_TYPE_VALUE_TREND, NULL);
}
