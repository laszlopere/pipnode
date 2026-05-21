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

#include "pn-image-saturation.h"
#include "pn-image-ops.h"

/* Saturation: interpolate each pixel between its Rec.601 luma (grey) and
 * its original colour by the `amount` property — out = luma + (c − luma)
 * · amount.  0 collapses to greyscale, 1 is a no-op, above 1 boosts
 * colour; results are clamped to [0, 255].  Alpha is preserved. */

struct _PnImageSaturation
{
    PnNode parent_instance;

    /* Colour scale relative to luma. */
    gdouble amount;
};

G_DEFINE_TYPE (PnImageSaturation, pn_image_saturation, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_AMOUNT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static inline guchar
clamp_u8 (gdouble v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (guchar) (v + 0.5);
}

static void
saturation_point (guchar *p, PnNode *node)
{
    const gdouble a = PN_IMAGE_SATURATION (node)->amount;
    const gdouble l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];

    p[0] = clamp_u8 (l + (p[0] - l) * a);
    p[1] = clamp_u8 (l + (p[1] - l) * a);
    p[2] = clamp_u8 (l + (p[2] - l) * a);
}

static GdkPixbuf *
saturation_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, saturation_point, node);
}

static void
pn_image_saturation_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Saturation", saturation_transform);
}

static void
pn_image_saturation_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    PnImageSaturation *self = PN_IMAGE_SATURATION (object);

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
pn_image_saturation_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    PnImageSaturation *self = PN_IMAGE_SATURATION (object);

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
pn_image_saturation_class_init (PnImageSaturationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_saturation_get_property;
    object_class->set_property = pn_image_saturation_set_property;

    node_class->receive    = pn_image_saturation_receive;
    node_class->class_name = "Saturation";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "Colour scale relative to grey "
            "(0 = greyscale, 1 = unchanged, >1 more saturated)",
            0.0, 4.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_saturation_init (PnImageSaturation *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->amount = 1.0;

    pn_node_set_class_name (node, "Saturation");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
