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

#include "pn-image-vignette.h"
#include "pn-image-ops.h"

#include <math.h>

/* Vignette: darken the image towards its corners.  The `strength`
 * property sets how deep the falloff is — 0 leaves the image untouched,
 * 1 drives the corners to black.  Alpha is preserved. */

struct _PnImageVignette
{
    PnNode parent_instance;

    /* How strongly the corners are darkened, 0..1. */
    gdouble strength;
};

G_DEFINE_TYPE (PnImageVignette, pn_image_vignette, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_STRENGTH,
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
vignette_point_xy (guchar *p, gint x, gint y, gint w, gint h, PnNode *node)
{
    gdouble s = PN_IMAGE_VIGNETTE (node)->strength;
    gdouble cx = w / 2.0, cy = h / 2.0;
    gdouble nx = (x + 0.5 - cx) / cx, ny = (y + 0.5 - cy) / cy;  /* -1..1 at edges */
    gdouble d = sqrt (nx * nx + ny * ny) / 1.4142136;            /* 0 center .. ~1 corner */
    gdouble f;

    if (d > 1.0) d = 1.0;
    f = 1.0 - s * d * d;
    if (f < 0.0) f = 0.0;

    p[0] = clamp_u8 (p[0] * f);
    p[1] = clamp_u8 (p[1] * f);
    p[2] = clamp_u8 (p[2] * f);
}

static GdkPixbuf *
vignette_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point_xy (src, vignette_point_xy, node);
}

static void
pn_image_vignette_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Vignette", vignette_transform);
}

static void
pn_image_vignette_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImageVignette *self = PN_IMAGE_VIGNETTE (object);

    switch (prop_id)
    {
    case PROP_STRENGTH:
        g_value_set_double (value, self->strength);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_vignette_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImageVignette *self = PN_IMAGE_VIGNETTE (object);

    switch (prop_id)
    {
    case PROP_STRENGTH:
        self->strength = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_vignette_class_init (PnImageVignetteClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_vignette_get_property;
    object_class->set_property = pn_image_vignette_set_property;

    node_class->receive    = pn_image_vignette_receive;
    node_class->class_name = "Vignette";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_STRENGTH] = g_param_spec_double (
            "strength", "Strength",
            "How strongly the corners are darkened "
            "(0 = none, 1 = corners go black)",
            0.0, 1.0, 0.5,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_vignette_init (PnImageVignette *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->strength = 0.5;

    pn_node_set_class_name (node, "Vignette");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
