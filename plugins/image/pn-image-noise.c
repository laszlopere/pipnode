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

#include "pn-image-noise.h"
#include "pn-image-ops.h"

/* Add Noise: add a random monochrome (luma) offset to every pixel.  The
 * same offset is applied to R, G and B so the grain stays grey rather
 * than coloured; `amount` scales how strong it is.  Alpha is preserved. */

struct _PnImageNoise
{
    PnNode parent_instance;

    /* Amount of random monochrome noise added per pixel, 0..1. */
    gdouble amount;
};

G_DEFINE_TYPE (PnImageNoise, pn_image_noise, PN_TYPE_NODE)

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
noise_point (guchar *p, PnNode *node)
{
    gdouble a = PN_IMAGE_NOISE (node)->amount;
    gdouble n = g_random_double_range (-1.0, 1.0) * a * 255.0;  /* same offset all 3 channels => luma noise */

    p[0] = clamp_u8 (p[0] + n);
    p[1] = clamp_u8 (p[1] + n);
    p[2] = clamp_u8 (p[2] + n);
}

static GdkPixbuf *
noise_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, noise_point, node);
}

static void
pn_image_noise_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Add Noise", noise_transform);
}

static void
pn_image_noise_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    PnImageNoise *self = PN_IMAGE_NOISE (object);

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
pn_image_noise_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    PnImageNoise *self = PN_IMAGE_NOISE (object);

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
pn_image_noise_class_init (PnImageNoiseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_noise_get_property;
    object_class->set_property = pn_image_noise_set_property;

    node_class->receive    = pn_image_noise_receive;
    node_class->class_name = "Add Noise";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "Amount of random monochrome noise added per pixel",
            0.0, 1.0, 0.2,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_noise_init (PnImageNoise *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->amount = 0.2;

    pn_node_set_class_name (node, "Add Noise");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
