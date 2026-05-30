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

#include "pn-topic.h"
#include "pn-message.h"

struct _PnTopic
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnTopic, pn_topic, PN_TYPE_NODE)

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_topic_receive (
        PnNode    *node,
        PnMessage *message)
{
    /* Stamp the node's own topic onto the message, resolving the
     * ${nodeclass} / ${nodename} / ${hostname} placeholders the same
     * way every emitter does.  The node carries no property of its own:
     * the template is the base #PnNode "topic" field the user edits in
     * the settings dialog. */
    gchar *topic = pn_node_resolve_topic (node);

    pn_message_set_topic (message, topic);
    g_free (topic);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_topic_class_init (PnTopicClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_topic_receive;

    node_class->class_name = "Topic";
    node_class->icon       = "\xef\x80\xab";  /* fa-tag U+F02B */
    node_class->color      = (PnColor){ 0.80, 0.55, 0.35, 1.0 };
    node_class->category   = "Filters/Reshape";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_topic_init (PnTopic *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  orange = { 0.80, 0.55, 0.35, 1.0 };

    pn_node_set_class_name (node, "Topic");
    pn_node_set_icon       (node, "\xef\x80\xab");  /* fa-tag U+F02B */
    pn_node_set_color      (node, &orange);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTopic *
pn_topic_new (void)
{
    return g_object_new (PN_TYPE_TOPIC, NULL);
}
