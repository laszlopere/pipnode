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

#include "pn-threshold.h"
#include "pn-message.h"

/* Comparator bounds.  The threshold can sit anywhere on the number
 * line; the hysteresis is a non-negative width, defaulting to zero so
 * a fresh node behaves like a plain "value >= threshold" comparator
 * until the user dials in some chatter suppression. */
#define PN_THRESHOLD_MIN       (-1e9)
#define PN_THRESHOLD_MAX       ( 1e9)
#define PN_THRESHOLD_DEF       ( 0.0)
#define PN_HYSTERESIS_MIN      ( 0.0)
#define PN_HYSTERESIS_MAX      ( 1e9)
#define PN_HYSTERESIS_DEF      ( 0.0)

struct _PnThreshold
{
    PnNode parent_instance;

    /* Trip point and the dead-band width below it. */
    gdouble  threshold;
    gdouble  hysteresis;

    /* Last state we forwarded, as a boolean.  Stays meaningless until
     * @has_state flips on the first message that carries a numeric
     * value, so the very first such message always emits (there is no
     * prior state to match). */
    gboolean has_state;
    gboolean state;
};

G_DEFINE_TYPE (PnThreshold, pn_threshold, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_THRESHOLD,
    PROP_HYSTERESIS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Pull a number out of @message under "value".  Returns %TRUE only
 *  when the member exists and holds a numeric JSON value (int or
 *  double); @out is left untouched otherwise. */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_threshold_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnThreshold *self = PN_THRESHOLD (node);
    gdouble      value;
    gdouble      half = self->hysteresis / 2.0;
    gboolean     next;

    /* No usable number to compare — stay silent and keep our state. */
    if (!read_value (message, &value))
        return;

    if (!self->has_state)
    {
        /* No prior state to debounce against, so the hysteresis band
         * does not apply yet: split cleanly at the threshold. */
        next = (value >= self->threshold);
    }
    else if (self->state)
    {
        /* Currently ON: fall back to OFF only once the value drops
         * below the lower edge of the band (threshold - hysteresis/2);
         * otherwise hold. */
        next = (value >= self->threshold - half);
    }
    else
    {
        /* Currently OFF: rise to ON only once the value reaches the
         * upper edge of the band (threshold + hysteresis/2). */
        next = (value >= self->threshold + half);
    }

    /* Forward only when the state actually changes (the first numeric
     * message always counts as a change). */
    if (self->has_state && next == self->state)
        return;

    self->has_state = TRUE;
    self->state     = next;

    /* Overwrite data.value with the clean boolean encoding and forward
     * the message, leaving topic / id / other members intact. */
    pn_message_set_double (message, "value", next ? 1.0 : 0.0);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_threshold_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnThreshold *self = PN_THRESHOLD (object);

    switch (prop_id)
    {
    case PROP_THRESHOLD:
        g_value_set_double (value, self->threshold);
        break;
    case PROP_HYSTERESIS:
        g_value_set_double (value, self->hysteresis);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_threshold_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnThreshold *self = PN_THRESHOLD (object);

    switch (prop_id)
    {
    case PROP_THRESHOLD:
        {
            gdouble v = g_value_get_double (value);
            if (self->threshold != v)
            {
                self->threshold = v;
                g_object_notify_by_pspec (object, props[PROP_THRESHOLD]);
            }
        }
        break;
    case PROP_HYSTERESIS:
        {
            gdouble v = g_value_get_double (value);
            if (self->hysteresis != v)
            {
                self->hysteresis = v;
                g_object_notify_by_pspec (object, props[PROP_HYSTERESIS]);
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
pn_threshold_class_init (PnThresholdClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_threshold_get_property;
    object_class->set_property = pn_threshold_set_property;
    node_class->receive        = pn_threshold_receive;

    node_class->class_name     = "Threshold";
    node_class->icon           = "\xef\x85\x88";  /* fa-level-up U+F148 */
    node_class->color          = (PnColor){ 0.92, 0.78, 0.30, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_THRESHOLD] = g_param_spec_double (
            "threshold", "Threshold",
            "Value at or above which the output is on (1.0)",
            PN_THRESHOLD_MIN, PN_THRESHOLD_MAX, PN_THRESHOLD_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HYSTERESIS] = g_param_spec_double (
            "hysteresis", "Hysteresis",
            "Width of the dead-band centered on the threshold: the value "
            "must rise half this above the threshold to turn on and fall "
            "half below it to turn off",
            PN_HYSTERESIS_MIN, PN_HYSTERESIS_MAX, PN_HYSTERESIS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_threshold_init (PnThreshold *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.78, 0.30, 1.0 };

    self->threshold  = PN_THRESHOLD_DEF;
    self->hysteresis = PN_HYSTERESIS_DEF;
    self->has_state  = FALSE;
    self->state      = FALSE;

    pn_node_set_class_name (node, "Threshold");
    pn_node_set_icon       (node, "\xef\x85\x88");  /* fa-level-up U+F148 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnThreshold *
pn_threshold_new (void)
{
    return g_object_new (PN_TYPE_THRESHOLD, NULL);
}
