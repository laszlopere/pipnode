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

#include "pn-staircase.h"
#include "pn-message.h"

/* On-time bounds, in milliseconds: from 0 (defer the turn-off to the
 * next main-loop iteration) up to one hour, default five seconds. */
#define PN_STAIRCASE_MS_MIN  0u
#define PN_STAIRCASE_MS_MAX  3600000u
#define PN_STAIRCASE_MS_DEF  5000u

/* A staircase timer is the kind of thing you usually want to extend by
 * pressing the button again, so re-triggering is on out of the box. */
#define PN_STAIRCASE_RETRIGGER_DEF  TRUE

struct _PnStaircase
{
    PnNode parent_instance;

    /* How long the node stays on after a trigger before emitting the
     * turn-off message, in ms. */
    guint     on_time_ms;

    /* When TRUE, a trigger that arrives while we are already on restarts
     * the turn-off timer instead of being ignored. */
    gboolean  retriggerable;

    /* The live turn-off timer's source id, or 0 when the node is at
     * rest.  Non-zero is exactly the "on" state.  Touched only from the
     * main thread. */
    guint     off_source_id;

    /* Topic of the trigger that turned us on, dup'd so the turn-off
     * message we synthesise can be paired with it downstream.  NULL when
     * the trigger carried no topic (or we are at rest). */
    gchar    *topic;
};

G_DEFINE_TYPE (PnStaircase, pn_staircase, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_ON_TIME_MS,
    PROP_RETRIGGERABLE,
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
/*  Timer callback                                                     */
/* ------------------------------------------------------------------ */

/* The on-time elapsed: synthesise the turn-off message, drop back to
 * the resting state, and let GLib tear the source down. */
static gboolean
pn_staircase_fire (gpointer data)
{
    PnStaircase *self = PN_STAIRCASE (data);
    PnNode      *node = PN_NODE (self);
    PnMessage   *msg  = pn_message_new (node, self->topic);

    /* Returning to rest *before* emitting keeps our state consistent for
     * any node that re-enters us synchronously from the "message"
     * handler (e.g. a wire that loops back). */
    self->off_source_id = 0;
    g_clear_pointer (&self->topic, g_free);

    pn_message_set_double (msg, "value", 0.0);
    pn_node_emit_message (node, msg);
    g_object_unref (msg);

    return G_SOURCE_REMOVE;
}

/* Arm (or, when already on, re-arm) the one-shot turn-off timer. */
static void
arm_timer (PnStaircase *self)
{
    if (self->off_source_id != 0)
        g_source_remove (self->off_source_id);

    self->off_source_id = g_timeout_add (self->on_time_ms,
                                         pn_staircase_fire, self);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_staircase_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnStaircase *self = PN_STAIRCASE (node);
    gdouble      value;

    /* Only a numeric value greater than 0.5 is a trigger; everything
     * else (off edges, valueless annotations) is dropped so the timed
     * window stays quiet and the node owns its own off edge. */
    if (!read_value (message, &value) || value <= 0.5)
        return;

    if (self->off_source_id != 0)
    {
        /* Already on: a re-trigger either pushes the turn-off out
         * (retriggerable) or is swallowed whole.  Never re-forwarded —
         * downstream is already on. */
        if (self->retriggerable)
            arm_timer (self);
        return;
    }

    /* Resting -> on: let the trigger through unchanged, remember its
     * topic for the matching turn-off, and start the clock. */
    g_clear_pointer (&self->topic, g_free);
    self->topic = g_strdup (pn_message_get_topic (message));

    arm_timer (self);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_staircase_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnStaircase *self = PN_STAIRCASE (object);

    switch (prop_id)
    {
    case PROP_ON_TIME_MS:
        g_value_set_uint (value, self->on_time_ms);
        break;
    case PROP_RETRIGGERABLE:
        g_value_set_boolean (value, self->retriggerable);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_staircase_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnStaircase *self = PN_STAIRCASE (object);

    switch (prop_id)
    {
    case PROP_ON_TIME_MS:
        {
            guint v = g_value_get_uint (value);
            if (self->on_time_ms != v)
            {
                /* A timer already counting down keeps its original
                 * deadline; the new on-time governs the next trigger
                 * (and any retrigger, which re-arms from scratch). */
                self->on_time_ms = v;
                g_object_notify_by_pspec (object, props[PROP_ON_TIME_MS]);
            }
        }
        break;
    case PROP_RETRIGGERABLE:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->retriggerable != v)
            {
                self->retriggerable = v;
                g_object_notify_by_pspec (object, props[PROP_RETRIGGERABLE]);
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
pn_staircase_dispose (GObject *object)
{
    PnStaircase *self = PN_STAIRCASE (object);

    /* Cancel a still-armed timer so the turn-off never lands on a
     * destroyed node. */
    if (self->off_source_id != 0)
    {
        g_source_remove (self->off_source_id);
        self->off_source_id = 0;
    }

    g_clear_pointer (&self->topic, g_free);

    G_OBJECT_CLASS (pn_staircase_parent_class)->dispose (object);
}

static void
pn_staircase_class_init (PnStaircaseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_staircase_get_property;
    object_class->set_property = pn_staircase_set_property;
    object_class->dispose      = pn_staircase_dispose;
    node_class->receive        = pn_staircase_receive;

    node_class->class_name     = "Staircase";
    node_class->icon           = "\xef\x87\x9a";  /* fa-history U+F1DA */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Timing";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_ON_TIME_MS] = g_param_spec_uint (
            "on-time-ms", "On time (ms)",
            "Milliseconds the node stays on after a trigger before it "
            "emits the value = 0.0 turn-off message",
            PN_STAIRCASE_MS_MIN,
            PN_STAIRCASE_MS_MAX,
            PN_STAIRCASE_MS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RETRIGGERABLE] = g_param_spec_boolean (
            "retriggerable", "Retriggerable",
            "When on, treat a fresh trigger as a request to restart the "
            "timer (delaying the turn-off) rather than ignoring it",
            PN_STAIRCASE_RETRIGGER_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_staircase_init (PnStaircase *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->on_time_ms    = PN_STAIRCASE_MS_DEF;
    self->retriggerable = PN_STAIRCASE_RETRIGGER_DEF;
    self->off_source_id = 0;
    self->topic         = NULL;

    pn_node_set_class_name (node, "Staircase");
    pn_node_set_icon       (node, "\xef\x87\x9a");  /* fa-history U+F1DA */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnStaircase *
pn_staircase_new (void)
{
    return g_object_new (PN_TYPE_STAIRCASE, NULL);
}
