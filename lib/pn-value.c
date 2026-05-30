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

#include "pn-value.h"
#include "pn-message.h"

struct _PnValue
{
    PnNode parent_instance;

    /* The fixed number written to data.value on every message. */
    gdouble value;
};

G_DEFINE_TYPE (PnValue, pn_value, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_VALUE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_value_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnValue *self = PN_VALUE (node);

    pn_message_set_double (message, "value", self->value);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_value_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnValue *self = PN_VALUE (object);

    switch (prop_id)
    {
    case PROP_VALUE:
        g_value_set_double (value, self->value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_value_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnValue *self = PN_VALUE (object);

    switch (prop_id)
    {
    case PROP_VALUE:
        {
            gdouble v = g_value_get_double (value);
            if (self->value != v)
            {
                self->value = v;
                g_object_notify_by_pspec (object, props[PROP_VALUE]);
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
pn_value_class_init (PnValueClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_value_get_property;
    object_class->set_property = pn_value_set_property;
    node_class->receive        = pn_value_receive;

    node_class->class_name     = "Value";
    node_class->icon           = "\xef\x8a\x92";  /* fa-hashtag U+F292 */
    node_class->color          = (PnColor){ 0.78, 0.45, 0.50, 1.0 };
    node_class->category       = "Filters/Reshape";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Fixed number written to data.value on every message",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_value_init (PnValue *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  rose = { 0.78, 0.45, 0.50, 1.0 };

    self->value = 0.0;

    pn_node_set_class_name (node, "Value");
    pn_node_set_icon       (node, "\xef\x8a\x92");  /* fa-hashtag U+F292 */
    pn_node_set_color      (node, &rose);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnValue *
pn_value_new (void)
{
    return g_object_new (PN_TYPE_VALUE, NULL);
}
