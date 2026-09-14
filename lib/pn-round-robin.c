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

#include "pn-round-robin.h"
#include "pn-message.h"

struct _PnRoundRobin
{
    PnNode parent_instance;

    gint n_outputs;   /* configurable output count, 2..16              */
    gint next;        /* 0-based output the next message leaves by;
                       * always 0 .. n_outputs-1                        */
};

G_DEFINE_TYPE (PnRoundRobin, pn_round_robin, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OUTPUTS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_round_robin_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRoundRobin *self = PN_ROUND_ROBIN (node);
    gint          out;

    if (pn_node_current_input () == PN_ROUND_ROBIN_IN_RESET)
    {
        /* Silent, so a reset wired from downstream forms no cycle. */
        self->next = 0;
        return;
    }

    /* Advance before emitting: a message that comes back through a
     * feedback wire during the synchronous emit takes the following
     * output, not this one again. */
    out        = self->next;
    self->next = (out + 1) % self->n_outputs;

    /* Forwarded untouched, like Topic Demux: exactly one output fires. */
    pn_node_emit_message_on_output (node, message, out);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
round_robin_set_outputs (PnRoundRobin *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    self->n_outputs = n;
    if (self->next >= n)
        self->next = 0;

    pn_node_set_n_outputs (node, n);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
pn_round_robin_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnRoundRobin *self = PN_ROUND_ROBIN (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        g_value_set_int (value, self->n_outputs);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_round_robin_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnRoundRobin *self = PN_ROUND_ROBIN (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        round_robin_set_outputs (self, g_value_get_int (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_round_robin_class_init (PnRoundRobinClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_round_robin_get_property;
    object_class->set_property = pn_round_robin_set_property;
    node_class->receive        = pn_round_robin_receive;

    node_class->class_name     = "Round Robin";
    node_class->icon           = "\xef\x80\xa1";  /* fa-refresh U+F021 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Gate";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of outputs. Every arriving message leaves by the next "
            "output in turn, wrapping back to output 1 after the last one. "
            "A message on the reset input makes output 1 the next again.",
            PN_ROUND_ROBIN_MIN_OUTPUTS, PN_ROUND_ROBIN_MAX_OUTPUTS,
            PN_ROUND_ROBIN_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_round_robin_init (PnRoundRobin *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->n_outputs = PN_ROUND_ROBIN_DEF_OUTPUTS;
    self->next      = 0;

    pn_node_set_class_name (node, "Round Robin");
    pn_node_set_icon       (node, "\xef\x80\xa1");  /* fa-refresh U+F021 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_n_inputs   (node, PN_ROUND_ROBIN_N_INPUTS);
    pn_node_set_input_name (node, PN_ROUND_ROBIN_IN_MESSAGE, "in");
    pn_node_set_input_name (node, PN_ROUND_ROBIN_IN_RESET,   "reset");
    pn_node_set_n_outputs  (node, self->n_outputs);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRoundRobin *
pn_round_robin_new (void)
{
    return g_object_new (PN_TYPE_ROUND_ROBIN, NULL);
}

gint
pn_round_robin_get_next (PnRoundRobin *self)
{
    g_return_val_if_fail (PN_IS_ROUND_ROBIN (self), 0);
    return self->next;
}

void
pn_round_robin_reset (PnRoundRobin *self)
{
    g_return_if_fail (PN_IS_ROUND_ROBIN (self));
    self->next = 0;
}
