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

#include "pn-comparator.h"
#include "pn-message.h"

#define PN_COMPARATOR_N_INPUTS 2

/* Hysteresis is a non-negative dead-band width centered on equality,
 * defaulting to zero so a fresh node behaves like a plain "A >= B"
 * comparator until the user dials in some chatter suppression. */
#define PN_HYSTERESIS_MIN  ( 0.0)
#define PN_HYSTERESIS_MAX  ( 1e9)
#define PN_HYSTERESIS_DEF  ( 0.0)

struct _PnComparator
{
    PnNode parent_instance;

    /* Width of the dead-band centered on A == B. */
    gdouble  hysteresis;

    /* Most recent numeric value seen on each input.  @has_a / @has_b
     * stay FALSE until that input has carried at least one number, and
     * no comparison is attempted until both are TRUE. */
    gboolean has_a;
    gdouble  a;
    gboolean has_b;
    gdouble  b;

    /* Last state we forwarded.  Meaningless until @has_state flips on
     * the first message that completes the pair, so that message always
     * emits (there is no prior state to match). */
    gboolean has_state;
    gboolean state;
};

G_DEFINE_TYPE (PnComparator, pn_comparator, PN_TYPE_NODE)

enum {
    PROP_0,
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
pn_comparator_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnComparator *self = PN_COMPARATOR (node);
    gint          idx  = pn_node_current_input ();
    gdouble       value;
    gdouble       half = self->hysteresis / 2.0;
    gdouble       delta;
    gboolean      next;

    /* No usable number to compare — stay silent and keep our state. */
    if (!read_value (message, &value))
        return;

    /* Latch the value on whichever input it arrived on. */
    if (idx <= 0)
    {
        self->a     = value;
        self->has_a = TRUE;
    }
    else
    {
        self->b     = value;
        self->has_b = TRUE;
    }

    /* Cannot compare until both inputs have produced a number. */
    if (!self->has_a || !self->has_b)
        return;

    delta = self->a - self->b;

    if (!self->has_state)
    {
        /* No prior state to debounce against, so the band does not
         * apply yet: split cleanly at equality (A >= B). */
        next = (delta >= 0.0);
    }
    else if (self->state)
    {
        /* Currently ON (A >= B): fall back to OFF only once A drops
         * below the lower edge of the band, B - hysteresis/2. */
        next = (delta >= -half);
    }
    else
    {
        /* Currently OFF (A < B): rise to ON only once A reaches the
         * upper edge of the band, B + hysteresis/2. */
        next = (delta >= half);
    }

    /* Forward only when the state actually changes (the first message
     * that completes the pair always counts as a change). */
    if (self->has_state && next == self->state)
        return;

    self->has_state = TRUE;
    self->state     = next;

    /* Overwrite data.value with the clean boolean encoding and forward
     * the triggering message, leaving topic / id / other members
     * intact. */
    pn_message_set_double (message, "value", next ? 1.0 : 0.0);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_comparator_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnComparator *self = PN_COMPARATOR (object);

    switch (prop_id)
    {
    case PROP_HYSTERESIS:
        g_value_set_double (value, self->hysteresis);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_comparator_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnComparator *self = PN_COMPARATOR (object);

    switch (prop_id)
    {
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
pn_comparator_class_init (PnComparatorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_comparator_get_property;
    object_class->set_property = pn_comparator_set_property;
    node_class->receive        = pn_comparator_receive;

    node_class->class_name     = "Comparator";
    node_class->icon           = "\xef\x89\x8e";  /* fa-balance-scale U+F24E */
    node_class->color          = (PnColor){ 0.92, 0.78, 0.30, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_HYSTERESIS] = g_param_spec_double (
            "hysteresis", "Hysteresis",
            "Width of the dead-band centered on equality: A must rise "
            "half this above B to turn the output on and fall half below "
            "B to turn it off",
            PN_HYSTERESIS_MIN, PN_HYSTERESIS_MAX, PN_HYSTERESIS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_comparator_init (PnComparator *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.78, 0.30, 1.0 };

    self->hysteresis = PN_HYSTERESIS_DEF;
    self->has_a      = FALSE;
    self->has_b      = FALSE;
    self->has_state  = FALSE;
    self->state      = FALSE;

    pn_node_set_class_name (node, "Comparator");
    pn_node_set_icon       (node, "\xef\x89\x8e");  /* fa-balance-scale U+F24E */
    pn_node_set_color      (node, &yellow);
    pn_node_set_n_inputs   (node, PN_COMPARATOR_N_INPUTS);  /* two inputs */
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnComparator *
pn_comparator_new (void)
{
    return g_object_new (PN_TYPE_COMPARATOR, NULL);
}
