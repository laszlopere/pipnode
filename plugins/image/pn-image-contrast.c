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

#include "pn-image-contrast.h"
#include "pn-image-ops.h"

/* Contrast: push each colour channel away from (or toward) mid-grey 128
 * by the `factor` property — out = (c − 128) · factor + 128.  A factor of
 * 1 is a no-op, below 1 flattens, above 1 steepens; results are clamped
 * to [0, 255].  Alpha is preserved. */

struct _PnImageContrast
{
    PnNode parent_instance;

    /* Slope around the 128 mid-point. */
    gdouble factor;
};

G_DEFINE_TYPE (PnImageContrast, pn_image_contrast, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_FACTOR,
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
contrast_point (guchar *p, PnNode *node)
{
    const gdouble f = PN_IMAGE_CONTRAST (node)->factor;

    p[0] = clamp_u8 ((p[0] - 128.0) * f + 128.0);
    p[1] = clamp_u8 ((p[1] - 128.0) * f + 128.0);
    p[2] = clamp_u8 ((p[2] - 128.0) * f + 128.0);
}

static GdkPixbuf *
contrast_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, contrast_point, node);
}

static void
pn_image_contrast_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Contrast", contrast_transform);
}

static void
pn_image_contrast_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImageContrast *self = PN_IMAGE_CONTRAST (object);

    switch (prop_id)
    {
    case PROP_FACTOR:
        g_value_set_double (value, self->factor);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_contrast_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImageContrast *self = PN_IMAGE_CONTRAST (object);

    switch (prop_id)
    {
    case PROP_FACTOR:
        self->factor = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_contrast_class_init (PnImageContrastClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_contrast_get_property;
    object_class->set_property = pn_image_contrast_set_property;

    node_class->receive    = pn_image_contrast_receive;
    node_class->class_name = "Contrast";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_FACTOR] = g_param_spec_double (
            "factor", "Factor",
            "Contrast slope around mid-grey 128 "
            "(1 = unchanged, <1 flatter, >1 steeper)",
            0.0, 4.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_contrast_init (PnImageContrast *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->factor = 1.0;

    pn_node_set_class_name (node, "Contrast");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
