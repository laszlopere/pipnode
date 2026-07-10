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

#include "pn-jump-out.h"
#include "pn-jump.h"
#include "pn-message.h"

struct _PnJumpOut
{
    PnNode  parent_instance;

    gchar  *tag;
};

G_DEFINE_TYPE (PnJumpOut, pn_jump_out, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_TAG,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* Shared with #PnJumpIn: a matched pair reads as one connection. */
static const PnColor jump_color = { 0.65, 0.22, 0.22, 1.0 };

/* ------------------------------------------------------------------ */
/*  Delivery                                                           */
/* ------------------------------------------------------------------ */

void
pn_jump_out_deliver (
        PnJumpOut *self,
        PnMessage *message)
{
    g_return_if_fail (PN_IS_JUMP_OUT (self));
    g_return_if_fail (PN_IS_MESSAGE (message));

    if (pn_node_get_disabled (PN_NODE (self)))
        return;

    pn_node_emit_message (PN_NODE (self), message);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_jump_out_get_size (
        PnNode *node,
        double *width,
        double *height)
{
    pn_jump_measure (PN_JUMP_OUT (node)->tag, width, height);
}

static double
pn_jump_out_get_header_height (PnNode *node)
{
    (void) node;
    return PN_JUMP_HEIGHT;
}

static void
pn_jump_out_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnJumpOut *self = PN_JUMP_OUT (object);

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
pn_jump_out_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnJumpOut *self = PN_JUMP_OUT (object);

    switch (prop_id)
    {
    case PROP_TAG:
        pn_jump_out_set_tag (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_jump_out_finalize (GObject *object)
{
    g_clear_pointer (&PN_JUMP_OUT (object)->tag, g_free);

    G_OBJECT_CLASS (pn_jump_out_parent_class)->finalize (object);
}

static void
pn_jump_out_class_init (PnJumpOutClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_jump_out_get_property;
    object_class->set_property = pn_jump_out_set_property;
    object_class->finalize     = pn_jump_out_finalize;

    /* Deliberately no ->receive: a jump-out has no input port and is
     * driven only by its #PnJumpIn partners. */
    node_class->get_size          = pn_jump_out_get_size;
    node_class->get_header_height = pn_jump_out_get_header_height;

    node_class->class_name = "Jump Out";
    node_class->icon       = "\xef\x82\x8b";  /* fa-sign-out U+F08B */
    node_class->color      = jump_color;
    node_class->category   = "Routing";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;

    /**
     * PnJumpOut:tag:
     *
     * The connection name.  Whatever arrives at any #PnJumpIn with the
     * same tag is emitted here.  Empty means unconnected, and is flagged
     * as an error.
     */
    props[PROP_TAG] =
        g_param_spec_string ("tag", "Tag",
                             "Name of the connection this flag joins",
                             "",
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_jump_out_init (PnJumpOut *self)
{
    PnNode *node = PN_NODE (self);

    self->tag = g_strdup ("");

    pn_node_set_class_name (node, "Jump Out");
    pn_node_set_icon       (node, "\xef\x82\x8b");  /* fa-sign-out U+F08B */
    pn_node_set_color      (node, &jump_color);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnJumpOut *
pn_jump_out_new (void)
{
    return g_object_new (PN_TYPE_JUMP_OUT, NULL);
}

const gchar *
pn_jump_out_get_tag (PnJumpOut *self)
{
    g_return_val_if_fail (PN_IS_JUMP_OUT (self), "");
    return self->tag;
}

void
pn_jump_out_set_tag (
        PnJumpOut   *self,
        const gchar *tag)
{
    g_return_if_fail (PN_IS_JUMP_OUT (self));

    if (tag == NULL)
        tag = "";

    if (g_strcmp0 (self->tag, tag) == 0)
        return;

    g_free (self->tag);
    self->tag = g_strdup (tag);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TAG]);
}
