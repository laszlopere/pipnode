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

#include "pn-jump-in.h"
#include "pn-jump-out.h"
#include "pn-jump.h"
#include "pn-message.h"

struct _PnJumpIn
{
    PnNode  parent_instance;

    gchar  *tag;
};

G_DEFINE_TYPE (PnJumpIn, pn_jump_in, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_TAG,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* The schematic-label red the KiCad global label is drawn in, shared with
 * #PnJumpOut so a matched pair reads as one connection. */
static const PnColor jump_color = { 0.65, 0.22, 0.22, 1.0 };

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Clone to every same-tag jump-out.  The clone is what #PnWire does for
 *  its own fan-out (see pn-wire.c on_source_message): two flags sharing a
 *  tag are two independent branches and must not see each other's edits
 *  to the data bag.  An unmatched or untagged flag drops the message; the
 *  has-error state set by pn_jump_refresh_errors() is what tells the user
 *  so, since a g_warning is invisible under a desktop launcher. */
static void
pn_jump_in_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnJumpIn *self    = PN_JUMP_IN (node);
    GList    *targets = pn_jump_collect_outputs (pn_node_get_flow (node),
                                                 self->tag);
    GList    *l;

    for (l = targets; l != NULL; l = l->next)
    {
        PnMessage *copy = pn_message_clone (message);

        pn_jump_out_deliver (PN_JUMP_OUT (l->data), copy);
        g_object_unref (copy);
    }

    g_list_free (targets);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_jump_in_get_size (
        PnNode *node,
        double *width,
        double *height)
{
    pn_jump_measure (PN_JUMP_IN (node)->tag, width, height);
}

/* The pennant is all header: there is no client area below it, so the
 * whole shape stays grab-able and selectable like a comment box. */
static double
pn_jump_in_get_header_height (PnNode *node)
{
    (void) node;
    return PN_JUMP_HEIGHT;
}

static void
pn_jump_in_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnJumpIn *self = PN_JUMP_IN (object);

    switch (prop_id)
    {
    case PROP_TAG:
        g_value_set_string (value, self->tag);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_jump_in_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnJumpIn *self = PN_JUMP_IN (object);

    switch (prop_id)
    {
    case PROP_TAG:
        pn_jump_in_set_tag (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_jump_in_finalize (GObject *object)
{
    g_clear_pointer (&PN_JUMP_IN (object)->tag, g_free);

    G_OBJECT_CLASS (pn_jump_in_parent_class)->finalize (object);
}

static void
pn_jump_in_class_init (PnJumpInClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_jump_in_get_property;
    object_class->set_property = pn_jump_in_set_property;
    object_class->finalize     = pn_jump_in_finalize;

    node_class->receive           = pn_jump_in_receive;
    node_class->get_size          = pn_jump_in_get_size;
    node_class->get_header_height = pn_jump_in_get_header_height;

    node_class->class_name = "Jump In";
    node_class->icon       = "\xef\x82\x90";  /* fa-sign-in U+F090 */
    node_class->color      = jump_color;
    node_class->category   = "Routing";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    /**
     * PnJumpIn:tag:
     *
     * The connection name.  Every #PnJumpOut with the same tag receives
     * whatever arrives here.  Empty means unconnected, and is flagged as
     * an error rather than silently dropping messages.
     */
    props[PROP_TAG] =
        g_param_spec_string ("tag", "Tag",
                             "Name of the connection this flag joins",
                             "",
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_jump_in_init (PnJumpIn *self)
{
    PnNode *node = PN_NODE (self);

    self->tag = g_strdup ("");

    pn_node_set_class_name (node, "Jump In");
    pn_node_set_icon       (node, "\xef\x82\x90");  /* fa-sign-in U+F090 */
    pn_node_set_color      (node, &jump_color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnJumpIn *
pn_jump_in_new (void)
{
    return g_object_new (PN_TYPE_JUMP_IN, NULL);
}

const gchar *
pn_jump_in_get_tag (PnJumpIn *self)
{
    g_return_val_if_fail (PN_IS_JUMP_IN (self), "");
    return self->tag;
}

void
pn_jump_in_set_tag (
        PnJumpIn    *self,
        const gchar *tag)
{
    g_return_if_fail (PN_IS_JUMP_IN (self));

    if (tag == NULL)
        tag = "";

    if (g_strcmp0 (self->tag, tag) == 0)
        return;

    g_free (self->tag);
    self->tag = g_strdup (tag);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TAG]);
}
