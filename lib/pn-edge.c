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

#include "pn-edge.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

struct _PnEdge
{
    PnNode parent_instance;

    /* Latched copy of the most recently observed boolean "success".
     * Stays unset until the first well-formed message arrives, which
     * is also why that first message can never be forwarded. */
    gboolean has_previous;
    gboolean previous;
};

G_DEFINE_TYPE (PnEdge, pn_edge, PN_TYPE_NODE)

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Pull a boolean out of @data under @name.  Returns %TRUE only when
 *  the member exists and holds a boolean value; @out_value is left
 *  untouched in every other case. */
static gboolean
read_boolean_member (
        JsonObject  *data,
        const gchar *name,
        gboolean    *out_value)
{
    JsonNode *n;

    if (data == NULL || !json_object_has_member (data, name))
        return FALSE;

    n = json_object_get_member (data, name);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_BOOLEAN)
        return FALSE;

    *out_value = json_node_get_boolean (n);
    return TRUE;
}

static void
pn_edge_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnEdge   *self = PN_EDGE (node);
    gboolean  current;

    if (!read_boolean_member (pn_message_get_data (message),
                              "success", &current))
        return;

    /* First well-formed message: latch the value and stay silent --
     * we have no prior state to compare it to. */
    if (!self->has_previous)
    {
        self->has_previous = TRUE;
        self->previous     = current;
        return;
    }

    if (current == self->previous)
        return;

    self->previous = current;
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_edge_class_init (PnEdgeClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive = pn_edge_receive;

    node_class->class_name = "Edge";
    node_class->icon       = "\xef\x83\xa7";  /* fa-bolt U+F0E7 */
    node_class->color      = (PnColor){ 0.92, 0.78, 0.30, 1.0 };
    node_class->category   = "Filters";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_edge_init (PnEdge *self)
{
    PnNode *node = PN_NODE (self);

    /* A muted yellow body, lightning bolt to suggest edge detection. */
    PnColor yellow = { 0.92, 0.78, 0.30, 1.0 };

    self->has_previous = FALSE;
    self->previous     = FALSE;

    pn_node_set_class_name (node, "Edge");
    pn_node_set_icon       (node, "\xef\x83\xa7");  /* fa-bolt U+F0E7 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnEdge *
pn_edge_new (void)
{
    return g_object_new (PN_TYPE_EDGE, NULL);
}
