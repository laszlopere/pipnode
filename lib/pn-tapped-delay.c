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

#include "pn-tapped-delay.h"
#include "pn-message.h"

struct _PnTappedDelay
{
    PnNode parent_instance;

    gint        n_outputs;  /* configurable tap count, 2..16            */
    guint       step_ms;    /* delay between consecutive taps, in ms    */

    /* Live one-shot timers: source id (GUINT_TO_POINTER) -> #Pending,
     * one per message copy still waiting.  The #Pending is owned by its
     * source (freed by the source's #GDestroyNotify, fired or
     * cancelled); the table only borrows it so a shrink can find the
     * copies bound for removed outputs.  Main thread only. */
    GHashTable *pending;
};

G_DEFINE_TYPE (PnTappedDelay, pn_tapped_delay, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OUTPUTS,
    PROP_STEP_MS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* One message copy waiting on its timer.  @self is borrowed (the source
 * does not keep the node alive); @message is a strong ref shared by the
 * copies of one arriving message. */
typedef struct
{
    PnTappedDelay *self;
    PnMessage     *message;
    gint           output;
    guint          source_id;
} Pending;

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

/* The copy's delay elapsed: send a fresh clone out of its output (so a
 * "message" handler that edits what it is given cannot reach the copies
 * still waiting for later outputs), then forget the source. */
static gboolean
tapped_delay_fire (gpointer data)
{
    Pending       *p    = data;
    PnTappedDelay *self = p->self;
    PnMessage     *out  = pn_message_clone (p->message);

    g_hash_table_remove (self->pending, GUINT_TO_POINTER (p->source_id));

    pn_message_set_source (out, PN_NODE (self));
    pn_node_emit_message_on_output (PN_NODE (self), out, p->output);
    g_object_unref (out);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tapped_delay_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTappedDelay *self   = PN_TAPPED_DELAY (node);
    PnMessage     *stored = pn_message_clone (message);
    gint           k;

    /* The stored clone never keeps the upstream node alive. */
    pn_message_set_source (stored, NULL);

    for (k = 0; k < self->n_outputs; k++)
    {
        Pending *p = g_new0 (Pending, 1);

        p->self    = self;
        p->message = g_object_ref (stored);
        p->output  = k;

        /* Single-threaded main loop: the source cannot fire before we
         * return, so filling in source_id after scheduling is safe.
         * Timers with equal deadlines fire in the order they were added,
         * which keeps a burst in order on each output. */
        p->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT,
                                           self->step_ms * (guint) (k + 1),
                                           tapped_delay_fire, p,
                                           pending_free);
        g_hash_table_insert (self->pending,
                             GUINT_TO_POINTER (p->source_id), p);
    }

    g_object_unref (stored);
}

/* ------------------------------------------------------------------ */
/*  Output names                                                       */
/* ------------------------------------------------------------------ */

gchar *
pn_tapped_delay_format_delay (guint ms)
{
    if (ms < 1000u)
        return g_strdup_printf ("%u ms", ms);

    return g_strdup_printf ("%g s", ms / 1000.0);
}

/* Label every output with its delay ("250 ms", "500 ms", ...). */
static void
update_output_names (PnTappedDelay *self)
{
    gint k;

    for (k = 0; k < self->n_outputs; k++)
    {
        gchar *label = pn_tapped_delay_format_delay (
                self->step_ms * (guint) (k + 1));

        pn_node_set_output_name (PN_NODE (self), k, label);
        g_free (label);
    }
}

/* ------------------------------------------------------------------ */
/*  Pending-copy bookkeeping                                           */
/* ------------------------------------------------------------------ */

/* Cancel every waiting copy bound for output @first or later.  The ids
 * are collected before any source is removed: g_source_remove() runs
 * pending_free synchronously, which would leave the iterator on a freed
 * value. */
static void
cancel_from_output (PnTappedDelay *self, gint first)
{
    GHashTableIter  it;
    gpointer        key, value;
    GArray         *ids = g_array_new (FALSE, FALSE, sizeof (guint));
    guint           i;

    g_hash_table_iter_init (&it, self->pending);
    while (g_hash_table_iter_next (&it, &key, &value))
    {
        if (((Pending *) value)->output >= first)
        {
            guint id = GPOINTER_TO_UINT (key);

            g_array_append_val (ids, id);
            g_hash_table_iter_remove (&it);
        }
    }

    for (i = 0; i < ids->len; i++)
        g_source_remove (g_array_index (ids, guint, i));

    g_array_unref (ids);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
tapped_delay_set_outputs (PnTappedDelay *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    if (n < self->n_outputs)
        cancel_from_output (self, n);

    self->n_outputs = n;
    pn_node_set_n_outputs (node, n);
    update_output_names (self);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
tapped_delay_set_step_ms (PnTappedDelay *self, guint ms)
{
    if (ms == self->step_ms)
        return;

    /* Copies already on their timers keep their original schedule. */
    self->step_ms = ms;
    update_output_names (self);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STEP_MS]);
}

static void
pn_tapped_delay_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTappedDelay *self = PN_TAPPED_DELAY (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        g_value_set_int (value, self->n_outputs);
        break;
    case PROP_STEP_MS:
        g_value_set_uint (value, self->step_ms);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tapped_delay_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTappedDelay *self = PN_TAPPED_DELAY (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        tapped_delay_set_outputs (self, g_value_get_int (value));
        break;
    case PROP_STEP_MS:
        tapped_delay_set_step_ms (self, g_value_get_uint (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tapped_delay_dispose (GObject *object)
{
    PnTappedDelay *self = PN_TAPPED_DELAY (object);

    /* Cancel every waiting copy so nothing lands on a destroyed node. */
    if (self->pending != NULL)
    {
        cancel_from_output (self, 0);
        g_clear_pointer (&self->pending, g_hash_table_destroy);
    }

    G_OBJECT_CLASS (pn_tapped_delay_parent_class)->dispose (object);
}

static void
pn_tapped_delay_class_init (PnTappedDelayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tapped_delay_get_property;
    object_class->set_property = pn_tapped_delay_set_property;
    object_class->dispose      = pn_tapped_delay_dispose;
    node_class->receive        = pn_tapped_delay_receive;

    node_class->class_name     = "Tapped Delay";
    node_class->icon           = "\xef\x89\x91";  /* fa-hourglass-start U+F251 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Timing";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of outputs (taps). Every arriving message is sent out "
            "of each output, output 1 after one step, output 2 after two "
            "steps, and so on. Lowering the count drops the copies still "
            "waiting for the removed outputs.",
            PN_TAPPED_DELAY_MIN_OUTPUTS, PN_TAPPED_DELAY_MAX_OUTPUTS,
            PN_TAPPED_DELAY_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_STEP_MS] = g_param_spec_uint (
            "step-ms", "Step (ms)",
            "Milliseconds between consecutive outputs: output k forwards "
            "each message k times this long after it arrived. A change "
            "applies to messages that arrive afterwards.",
            PN_TAPPED_DELAY_STEP_MS_MIN, PN_TAPPED_DELAY_STEP_MS_MAX,
            PN_TAPPED_DELAY_STEP_MS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tapped_delay_init (PnTappedDelay *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->n_outputs = PN_TAPPED_DELAY_DEF_OUTPUTS;
    self->step_ms   = PN_TAPPED_DELAY_STEP_MS_DEF;
    self->pending   = g_hash_table_new (g_direct_hash, g_direct_equal);

    pn_node_set_class_name (node, "Tapped Delay");
    pn_node_set_icon       (node, "\xef\x89\x91");  /* fa-hourglass-start U+F251 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_n_outputs  (node, self->n_outputs);
    update_output_names (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTappedDelay *
pn_tapped_delay_new (void)
{
    return g_object_new (PN_TYPE_TAPPED_DELAY, NULL);
}

guint
pn_tapped_delay_get_n_pending (PnTappedDelay *self)
{
    g_return_val_if_fail (PN_IS_TAPPED_DELAY (self), 0);
    return g_hash_table_size (self->pending);
}
