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

#include "pn-failure.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

struct _PnFailure
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnFailure, pn_failure, PN_TYPE_NODE)

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_failure_receive (
        PnNode    *node,
        PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *n;

    if (data == NULL || !json_object_has_member (data, "success"))
        return;

    n = json_object_get_member (data, "success");
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_BOOLEAN)
        return;

    if (json_node_get_boolean (n))
        return;

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_failure_class_init (PnFailureClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive = pn_failure_receive;

    node_class->class_name = "Failure";
    node_class->icon       = "\xef\x80\x8d";  /* fa-times U+F00D */
    node_class->color      = (PnColor){ 0.86, 0.30, 0.28, 1.0 };
    node_class->category   = "Filters/Gate";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_failure_init (PnFailure *self)
{
    PnNode *node = PN_NODE (self);

    /* Muted red to read as "failed" / "stop" at a glance, with the
     * cross glyph reinforcing the meaning.  Mirrors the green + check
     * of the companion #PnSuccess node. */
    PnColor red = { 0.86, 0.30, 0.28, 1.0 };

    pn_node_set_class_name (node, "Failure");
    pn_node_set_icon       (node, "\xef\x80\x8d");  /* fa-times U+F00D */
    pn_node_set_color      (node, &red);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFailure *
pn_failure_new (void)
{
    return g_object_new (PN_TYPE_FAILURE, NULL);
}
