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

#include "pn-image-vibrance.h"
#include "pn-image-ops.h"

/* Vibrance: a saturation boost weighted toward the less-saturated
 * pixels, so already-vivid colours move less than muted ones.  A
 * positive `amount` enriches colour, a negative one mutes it toward
 * grey.  Hue and value are untouched; alpha is preserved. */

struct _PnImageVibrance
{
    PnNode parent_instance;

    /* Saturation push, [-1, 1]. */
    gdouble amount;
};

G_DEFINE_TYPE (PnImageVibrance, pn_image_vibrance, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_AMOUNT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void
vibrance_point (guchar *p, PnNode *node)
{
    gdouble       h, s, v;
    const gdouble a = PN_IMAGE_VIBRANCE (node)->amount;

    pn_image_rgb_to_hsv (p[0], p[1], p[2], &h, &s, &v);
    s = s + a * (1.0 - s);
    s = CLAMP (s, 0.0, 1.0);
    pn_image_hsv_to_rgb (h, s, v, &p[0], &p[1], &p[2]);
}

static GdkPixbuf *
vibrance_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, vibrance_point, node);
}

static void
pn_image_vibrance_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Vibrance", vibrance_transform);
}

static void
pn_image_vibrance_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImageVibrance *self = PN_IMAGE_VIBRANCE (object);

    switch (prop_id)
    {
    case PROP_AMOUNT:
        g_value_set_double (value, self->amount);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_vibrance_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImageVibrance *self = PN_IMAGE_VIBRANCE (object);

    switch (prop_id)
    {
    case PROP_AMOUNT:
        self->amount = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_vibrance_class_init (PnImageVibranceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_vibrance_get_property;
    object_class->set_property = pn_image_vibrance_set_property;

    node_class->receive    = pn_image_vibrance_receive;
    node_class->class_name = "Vibrance";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "Saturation boost weighted toward less-saturated pixels; "
            "negative values mute colour",
            -1.0, 1.0, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_vibrance_init (PnImageVibrance *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->amount = 0.0;

    pn_node_set_class_name (node, "Vibrance");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
