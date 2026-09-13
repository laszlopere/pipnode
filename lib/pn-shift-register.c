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

#include "pn-shift-register.h"
#include "pn-message.h"

struct _PnShiftRegister
{
    PnNode parent_instance;

    gint       n_outputs;   /* configurable stage count, 2..16          */

    /* The stages, newest first: pdata[0] is stage 1 (output 1).  Holds
     * owned clones with their source cleared, so a stored message never
     * keeps an upstream node — or, through a feedback wire, this node —
     * alive.  Never longer than n_outputs. */
    GPtrArray *stages;
};

G_DEFINE_TYPE (PnShiftRegister, pn_shift_register, PN_TYPE_NODE)

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
pn_shift_register_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnShiftRegister *self = PN_SHIFT_REGISTER (node);
    PnMessage       *stored;
    gint             k;

    /* Shift: drop the last stage when full, then push the newcomer. */
    if ((gint) self->stages->len >= self->n_outputs)
        g_ptr_array_set_size (self->stages, self->n_outputs - 1);

    stored = pn_message_clone (message);
    pn_message_set_source (stored, NULL);
    g_ptr_array_insert (self->stages, 0, stored);

    /* Oldest stage first, output 1 last (see the header).  Each emission
     * is a fresh clone so a "message" handler that edits what it is given
     * cannot reach back into a stage. */
    for (k = (gint) self->stages->len - 1; k >= 0; k--)
    {
        PnMessage *out = pn_message_clone (self->stages->pdata[k]);

        pn_message_set_source (out, node);
        pn_node_emit_message_on_output (node, out, k);
        g_object_unref (out);
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
shift_register_set_outputs (PnShiftRegister *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    self->n_outputs = n;

    /* Keep the newest stages that still fit. */
    if ((gint) self->stages->len > n)
        g_ptr_array_set_size (self->stages, n);

    /* Resizes the ports; the core trims the per-output readouts of any
     * output that is gone, so the surviving stages keep theirs. */
    pn_node_set_n_outputs (node, n);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
pn_shift_register_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnShiftRegister *self = PN_SHIFT_REGISTER (object);

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
pn_shift_register_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnShiftRegister *self = PN_SHIFT_REGISTER (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        shift_register_set_outputs (self, g_value_get_int (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_shift_register_finalize (GObject *object)
{
    PnShiftRegister *self = PN_SHIFT_REGISTER (object);

    g_clear_pointer (&self->stages, g_ptr_array_unref);

    G_OBJECT_CLASS (pn_shift_register_parent_class)->finalize (object);
}

static void
pn_shift_register_class_init (PnShiftRegisterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_shift_register_get_property;
    object_class->set_property = pn_shift_register_set_property;
    object_class->finalize     = pn_shift_register_finalize;
    node_class->receive        = pn_shift_register_receive;

    node_class->class_name     = "Shift Register";
    node_class->icon           = "\xef\x83\x8b";  /* fa-list-ol U+F0CB */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of stages, one output each. Every arriving message "
            "enters stage 1 and pushes the stored ones one stage along; "
            "the message in the last stage falls off. The newest message "
            "then leaves on output 1, the previous one on output 2, and so "
            "on. Stages that have not been filled yet stay silent.",
            PN_SHIFT_REGISTER_MIN_OUTPUTS, PN_SHIFT_REGISTER_MAX_OUTPUTS,
            PN_SHIFT_REGISTER_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_shift_register_init (PnShiftRegister *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };

    self->n_outputs = PN_SHIFT_REGISTER_DEF_OUTPUTS;
    self->stages    = g_ptr_array_new_with_free_func (g_object_unref);

    pn_node_set_class_name (node, "Shift Register");
    pn_node_set_icon       (node, "\xef\x83\x8b");  /* fa-list-ol U+F0CB */
    pn_node_set_color      (node, &indigo);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_n_outputs  (node, self->n_outputs);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnShiftRegister *
pn_shift_register_new (void)
{
    return g_object_new (PN_TYPE_SHIFT_REGISTER, NULL);
}

guint
pn_shift_register_get_n_filled (PnShiftRegister *self)
{
    g_return_val_if_fail (PN_IS_SHIFT_REGISTER (self), 0);
    return self->stages->len;
}

void
pn_shift_register_clear (PnShiftRegister *self)
{
    g_return_if_fail (PN_IS_SHIFT_REGISTER (self));

    g_ptr_array_set_size (self->stages, 0);
    pn_node_clear_output_value_displays (PN_NODE (self));
}
