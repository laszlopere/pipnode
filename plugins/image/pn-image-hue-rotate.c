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

#include "pn-image-hue-rotate.h"
#include "pn-image-ops.h"

/* Hue Rotate: spin every pixel's hue around the colour wheel by the
 * `degrees` property, leaving saturation and value untouched.  Greys
 * (which have no defined hue) are unaffected.  Alpha is preserved. */

struct _PnImageHueRotate
{
    PnNode parent_instance;

    /* Hue offset in degrees, [-180, 180]. */
    gdouble degrees;
};

G_DEFINE_TYPE (PnImageHueRotate, pn_image_hue_rotate, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_DEGREES,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void
hue_rotate_point (guchar *p, PnNode *node)
{
    gdouble h, s, v;

    pn_image_rgb_to_hsv (p[0], p[1], p[2], &h, &s, &v);
    h += PN_IMAGE_HUE_ROTATE (node)->degrees;
    pn_image_hsv_to_rgb (h, s, v, &p[0], &p[1], &p[2]);
}

static GdkPixbuf *
hue_rotate_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, hue_rotate_point, node);
}

static void
pn_image_hue_rotate_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Hue Rotate", hue_rotate_transform);
}

static void
pn_image_hue_rotate_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    PnImageHueRotate *self = PN_IMAGE_HUE_ROTATE (object);

    switch (prop_id)
    {
    case PROP_DEGREES:
        g_value_set_double (value, self->degrees);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_hue_rotate_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    PnImageHueRotate *self = PN_IMAGE_HUE_ROTATE (object);

    switch (prop_id)
    {
    case PROP_DEGREES:
        self->degrees = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_hue_rotate_class_init (PnImageHueRotateClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_hue_rotate_get_property;
    object_class->set_property = pn_image_hue_rotate_set_property;

    node_class->receive    = pn_image_hue_rotate_receive;
    node_class->class_name = "Hue Rotate";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_DEGREES] = g_param_spec_double (
            "degrees", "Degrees",
            "Degrees to rotate every pixel's hue around the colour wheel",
            -180.0, 180.0, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_hue_rotate_init (PnImageHueRotate *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->degrees = 0.0;

    pn_node_set_class_name (node, "Hue Rotate");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
