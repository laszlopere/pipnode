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

#include "pn-wire.h"
#include "pn-message.h"

struct _PnWire
{
    GObject parent_instance;

    PnNode *source;
    PnNode *target;

    /** Stable per-wire handle, minted at construction.  Unlike a node's
     *  UUID this is a *session* identity: it is the handle the D-Bus
     *  automation API hands back from Connect and addresses in
     *  Disconnect/ListWires (TODO #40.5), and is NOT serialized — a wire
     *  is fully described on disk by its endpoints + target_input, so a
     *  reloaded wire simply gets a fresh handle. */
    gchar  *uuid;

    /** Which of the target's input ports this wire feeds.  0 for the
     *  single-input nodes that make up almost every flow; >0 only when
     *  the target is a multi-input node (see pn_node_get_n_inputs()). */
    gint    target_input;

    /** Handler id of the "message" signal connected on @source, or
     *  0 when not currently subscribed. */
    gulong  source_handler;
};

G_DEFINE_TYPE (PnWire, pn_wire, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_SOURCE,
    PROP_TARGET,
    PROP_TARGET_INPUT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum {
    SIG_MESSAGE_PASSED,
    N_SIGNALS,
};

static guint sigs[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  Routing                                                            */
/* ------------------------------------------------------------------ */

/** Forward each emission of source's "message" signal to the target.
 *
 *  A single source may have several wires hanging off its output and
 *  each wire receives the same #PnMessage* from the GSignal fan-out.
 *  If a target's receive handler mutates the message in place (Table
 *  Model attaching `data.table`, Set rewriting members, Format
 *  rewriting `data.output`, …) the mutation is visible to every
 *  sibling wire whose handler runs after this one — a Debug Print
 *  attached to the same source would then render the mutated state
 *  instead of what was actually emitted.  Hand the target its own
 *  deep-copy so each fan-out branch is independent. */
static void
on_source_message (
        PnNode    *source,
        PnMessage *message,
        gpointer   user_data)
{
    PnWire    *self = PN_WIRE (user_data);
    PnMessage *copy;

    (void) source;

    if (self->target == NULL)
        return;

    copy = pn_message_clone (message);

    /* Announce the crossing *before* delivering downstream.  Delivery is
     * synchronous and depth-first, so emitting first means handlers fire
     * in travel order (root hop first) and observe the dispatch depth of
     * THIS hop rather than the deepest one already unwound — the view
     * relies on both to stagger the animation hop by hop.  The handler
     * reads the original (caller-owned) message but must not retain it;
     * emission is near-free when nobody is listening. */
    g_signal_emit (self, sigs[SIG_MESSAGE_PASSED], 0, message);

    pn_node_receive_message_on_input (self->target, copy, self->target_input);
    g_object_unref (copy);
}

/** Wire up (or tear down) the source-side signal subscription so that
 *  messages start flowing as soon as both endpoints are present. */
static void
rewire_source (
        PnWire *self,
        PnNode *new_source)
{
    if (self->source == new_source)
        return;

    if (self->source != NULL && self->source_handler != 0)
    {
        g_signal_handler_disconnect (self->source, self->source_handler);
        self->source_handler = 0;
    }

    g_set_object (&self->source, new_source);

    if (self->source != NULL)
        self->source_handler = g_signal_connect (
                self->source, "message",
                G_CALLBACK (on_source_message), self);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_wire_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnWire *self = PN_WIRE (object);

    switch (prop_id)
    {
    case PROP_SOURCE:
        g_value_set_object (value, self->source);
        break;
    case PROP_TARGET:
        g_value_set_object (value, self->target);
        break;
    case PROP_TARGET_INPUT:
        g_value_set_int (value, self->target_input);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_wire_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnWire *self = PN_WIRE (object);

    switch (prop_id)
    {
    case PROP_SOURCE:
        pn_wire_set_source (self, g_value_get_object (value));
        break;
    case PROP_TARGET:
        pn_wire_set_target (self, g_value_get_object (value));
        break;
    case PROP_TARGET_INPUT:
        pn_wire_set_target_input (self, g_value_get_int (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_wire_dispose (GObject *object)
{
    PnWire *self = PN_WIRE (object);

    /* Tear down the subscription before releasing the source ref. */
    rewire_source (self, NULL);
    g_clear_object (&self->target);

    G_OBJECT_CLASS (pn_wire_parent_class)->dispose (object);
}

static void
pn_wire_finalize (GObject *object)
{
    PnWire *self = PN_WIRE (object);

    g_clear_pointer (&self->uuid, g_free);

    G_OBJECT_CLASS (pn_wire_parent_class)->finalize (object);
}

static void
pn_wire_class_init (PnWireClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_wire_get_property;
    object_class->set_property = pn_wire_set_property;
    object_class->dispose      = pn_wire_dispose;
    object_class->finalize     = pn_wire_finalize;

    props[PROP_SOURCE] = g_param_spec_object (
            "source", "Source",
            "Node whose output emits the messages routed by this wire",
            PN_TYPE_NODE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TARGET] = g_param_spec_object (
            "target", "Target",
            "Node whose input receives the routed messages",
            PN_TYPE_NODE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TARGET_INPUT] = g_param_spec_int (
            "target-input", "Target input",
            "Index of the target's input port this wire feeds (0 unless "
            "the target is a multi-input node)",
            0, G_MAXINT, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Emitted each time a message is forwarded across this wire, after
     * it has been delivered to the target.  Carries the #PnMessage that
     * crossed (borrowed — do not retain).  Wired one source to several
     * targets each fire independently, so a fan-out lights every branch. */
    sigs[SIG_MESSAGE_PASSED] = g_signal_new (
            "message-passed",
            PN_TYPE_WIRE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            PN_TYPE_MESSAGE);
}

static void
pn_wire_init (PnWire *self)
{
    self->source         = NULL;
    self->target         = NULL;
    self->uuid           = g_uuid_string_random ();
    self->target_input   = 0;
    self->source_handler = 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWire *
pn_wire_new (
        PnNode *source,
        PnNode *target)
{
    return g_object_new (PN_TYPE_WIRE,
                         "source", source,
                         "target", target,
                         NULL);
}

PnWire *
pn_wire_new_full (
        PnNode *source,
        PnNode *target,
        gint    target_input)
{
    return g_object_new (PN_TYPE_WIRE,
                         "source",       source,
                         "target",       target,
                         "target-input", target_input,
                         NULL);
}

gint
pn_wire_get_target_input (PnWire *self)
{
    g_return_val_if_fail (PN_IS_WIRE (self), 0);
    return self->target_input;
}

void
pn_wire_set_target_input (
        PnWire *self,
        gint    target_input)
{
    g_return_if_fail (PN_IS_WIRE (self));
    g_return_if_fail (target_input >= 0);

    if (self->target_input == target_input)
        return;

    self->target_input = target_input;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TARGET_INPUT]);
}

PnNode *
pn_wire_get_source (PnWire *self)
{
    g_return_val_if_fail (PN_IS_WIRE (self), NULL);
    return self->source;
}

void
pn_wire_set_source (
        PnWire *self,
        PnNode *source)
{
    g_return_if_fail (PN_IS_WIRE (self));
    g_return_if_fail (source == NULL || PN_IS_NODE (source));

    if (self->source == source)
        return;

    rewire_source (self, source);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SOURCE]);
}

PnNode *
pn_wire_get_target (PnWire *self)
{
    g_return_val_if_fail (PN_IS_WIRE (self), NULL);
    return self->target;
}

void
pn_wire_set_target (
        PnWire *self,
        PnNode *target)
{
    g_return_if_fail (PN_IS_WIRE (self));
    g_return_if_fail (target == NULL || PN_IS_NODE (target));

    if (self->target == target)
        return;

    g_set_object (&self->target, target);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TARGET]);
}

void
pn_wire_disconnect (PnWire *self)
{
    g_return_if_fail (PN_IS_WIRE (self));

    pn_wire_set_source (self, NULL);
    pn_wire_set_target (self, NULL);
}

const gchar *
pn_wire_get_uuid (PnWire *self)
{
    g_return_val_if_fail (PN_IS_WIRE (self), NULL);
    return self->uuid;
}

void
pn_wire_set_uuid (
        PnWire      *self,
        const gchar *uuid)
{
    g_return_if_fail (PN_IS_WIRE (self));

    /* %NULL/empty means "regenerate", mirroring pn_node_set_uuid(). */
    g_free (self->uuid);
    self->uuid = (uuid != NULL && *uuid != '\0')
        ? g_strdup (uuid)
        : g_uuid_string_random ();
}
