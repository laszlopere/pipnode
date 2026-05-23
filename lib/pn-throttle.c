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

#include "pn-throttle.h"
#include "pn-message.h"

#define PN_THROTTLE_INTERVAL_MIN  1u
#define PN_THROTTLE_INTERVAL_MAX  3600u
#define PN_THROTTLE_INTERVAL_DEF  1u

struct _PnThrottle
{
    PnNode parent_instance;

    /* Minimum seconds between forwarded messages. */
    guint    interval;

    /* Monotonic timestamp (microseconds) of the most recent forward.
     * Stays unset until the first message arrives — checked via
     * @has_last so an interval-of-zero start does not race a
     * coincidentally zero monotonic clock. */
    gboolean has_last;
    gint64   last_us;
};

G_DEFINE_TYPE (PnThrottle, pn_throttle, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INTERVAL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_throttle_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnThrottle *self = PN_THROTTLE (node);
    gint64      now  = g_get_monotonic_time ();

    if (self->has_last)
    {
        gint64 min_gap = (gint64) self->interval * G_TIME_SPAN_SECOND;
        if (now - self->last_us < min_gap)
            return;
    }

    self->has_last = TRUE;
    self->last_us  = now;
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_throttle_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnThrottle *self = PN_THROTTLE (object);

    switch (prop_id)
    {
    case PROP_INTERVAL:
        g_value_set_uint (value, self->interval);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_throttle_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnThrottle *self = PN_THROTTLE (object);

    switch (prop_id)
    {
    case PROP_INTERVAL:
        {
            guint v = g_value_get_uint (value);
            if (self->interval != v)
            {
                self->interval = v;
                g_object_notify_by_pspec (object, props[PROP_INTERVAL]);
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
pn_throttle_class_init (PnThrottleClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_throttle_get_property;
    object_class->set_property = pn_throttle_set_property;
    node_class->receive        = pn_throttle_receive;

    node_class->class_name     = "Throttle";
    node_class->icon           = "\xef\x83\xa4";  /* fa-tachometer U+F0E4 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_INTERVAL] = g_param_spec_uint (
            "interval", "Interval",
            "Minimum seconds between forwarded messages",
            PN_THROTTLE_INTERVAL_MIN,
            PN_THROTTLE_INTERVAL_MAX,
            PN_THROTTLE_INTERVAL_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_throttle_init (PnThrottle *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->interval = PN_THROTTLE_INTERVAL_DEF;
    self->has_last = FALSE;
    self->last_us  = 0;

    pn_node_set_class_name (node, "Throttle");
    pn_node_set_icon       (node, "\xef\x83\xa4");  /* fa-tachometer U+F0E4 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnThrottle *
pn_throttle_new (void)
{
    return g_object_new (PN_TYPE_THROTTLE, NULL);
}
