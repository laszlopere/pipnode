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

#include "pn-delay.h"
#include "pn-message.h"

/* Delay bounds, in milliseconds: from 0 (defer to the next main-loop
 * iteration) up to one hour, default one second. */
#define PN_DELAY_MS_MIN  0u
#define PN_DELAY_MS_MAX  3600000u
#define PN_DELAY_MS_DEF  1000u

struct _PnDelay
{
    PnNode parent_instance;

    /* How long to hold each message before forwarding it, in ms. */
    guint       delay_ms;

    /* Set of live one-shot timer source ids (GUINT_TO_POINTER), one per
     * message still waiting to be forwarded.  Each source carries a
     * #Pending as its user-data; the source's #GDestroyNotify releases
     * the held message ref whether the timer fired or was cancelled.
     * Touched only from the main thread. */
    GHashTable *pending;
};

G_DEFINE_TYPE (PnDelay, pn_delay, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_DELAY_MS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* One message waiting on its timer.  @self is borrowed (the source does
 * not keep the node alive); @message holds a strong ref. */
typedef struct
{
    PnDelay   *self;
    PnMessage *message;
    guint      source_id;
} Pending;

/* GDestroyNotify for a timer source: runs both when the timer fired
 * (after the callback returns G_SOURCE_REMOVE) and when the source is
 * cancelled in dispose.  Either way the held message ref is released. */
static void
pending_free (gpointer data)
{
    Pending *p = data;

    g_clear_object (&p->message);
    g_free (p);
}

/* ------------------------------------------------------------------ */
/*  Timer callback                                                     */
/* ------------------------------------------------------------------ */

/* The delay elapsed: forward the message, then drop this source's id
 * from the pending set and let GLib tear the source down (which calls
 * pending_free to release the message). */
static gboolean
pn_delay_fire (gpointer data)
{
    Pending *p    = data;
    PnDelay *self = p->self;

    pn_node_emit_message (PN_NODE (self), p->message);

    g_hash_table_remove (self->pending, GUINT_TO_POINTER (p->source_id));
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_delay_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnDelay *self = PN_DELAY (node);
    Pending *p    = g_new0 (Pending, 1);

    p->self    = self;
    p->message = g_object_ref (message);

    /* Single-threaded main loop: the source cannot fire before we
     * return, so filling in source_id after scheduling is race-free. */
    p->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT,
                                       self->delay_ms,
                                       pn_delay_fire, p, pending_free);
    g_hash_table_add (self->pending, GUINT_TO_POINTER (p->source_id));
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_delay_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDelay *self = PN_DELAY (object);

    switch (prop_id)
    {
    case PROP_DELAY_MS:
        g_value_set_uint (value, self->delay_ms);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_delay_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDelay *self = PN_DELAY (object);

    switch (prop_id)
    {
    case PROP_DELAY_MS:
        {
            guint v = g_value_get_uint (value);
            if (self->delay_ms != v)
            {
                /* Only newly-arriving messages use the new delay;
                 * already-armed timers keep their original schedule. */
                self->delay_ms = v;
                g_object_notify_by_pspec (object, props[PROP_DELAY_MS]);
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
pn_delay_dispose (GObject *object)
{
    PnDelay *self = PN_DELAY (object);

    /* Cancel every still-armed timer so a message never lands on a
     * destroyed node.  Steal the set first: g_source_remove() invokes
     * pending_free synchronously, and pn_delay_fire (the only other
     * editor of the set) cannot run while we are removing sources. */
    if (self->pending != NULL)
    {
        GHashTable     *set = g_steal_pointer (&self->pending);
        GHashTableIter  it;
        gpointer        key;

        g_hash_table_iter_init (&it, set);
        while (g_hash_table_iter_next (&it, &key, NULL))
            g_source_remove (GPOINTER_TO_UINT (key));

        g_hash_table_destroy (set);
    }

    G_OBJECT_CLASS (pn_delay_parent_class)->dispose (object);
}

static void
pn_delay_class_init (PnDelayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_delay_get_property;
    object_class->set_property = pn_delay_set_property;
    object_class->dispose      = pn_delay_dispose;
    node_class->receive        = pn_delay_receive;

    node_class->class_name     = "Delay";
    node_class->icon           = "\xef\x89\x92";  /* fa-hourglass-half U+F252 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_DELAY_MS] = g_param_spec_uint (
            "delay-ms", "Delay (ms)",
            "Milliseconds to hold each message before forwarding it",
            PN_DELAY_MS_MIN,
            PN_DELAY_MS_MAX,
            PN_DELAY_MS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_delay_init (PnDelay *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->delay_ms = PN_DELAY_MS_DEF;
    self->pending  = g_hash_table_new (g_direct_hash, g_direct_equal);

    pn_node_set_class_name (node, "Delay");
    pn_node_set_icon       (node, "\xef\x89\x92");  /* fa-hourglass-half U+F252 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDelay *
pn_delay_new (void)
{
    return g_object_new (PN_TYPE_DELAY, NULL);
}
