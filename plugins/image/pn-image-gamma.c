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

#include <math.h>

#include "pn-image-gamma.h"
#include "pn-image-ops.h"

/* Gamma: apply a power-law curve to every colour channel.  A `gamma`
 * below 1 darkens the midtones, above 1 lightens them; the curve is
 * built once into a 256-entry lookup table and applied to each channel.
 * Alpha is preserved. */

struct _PnImageGamma
{
    PnNode parent_instance;

    /* Power-law exponent applied to R, G, B. */
    gdouble gamma;
};

G_DEFINE_TYPE (PnImageGamma, pn_image_gamma, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_GAMMA,
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

static GdkPixbuf *
gamma_transform (GdkPixbuf *src, PnNode *node)
{
    gdouble g = PN_IMAGE_GAMMA (node)->gamma;
    if (g < 0.01)
        g = 0.01;

    guchar lut[256];
    for (int i = 0; i < 256; i++)
    {
        gdouble v = pow (i / 255.0, 1.0 / g);
        lut[i] = clamp_u8 (v * 255.0);
    }

    GdkPixbuf *dst = gdk_pixbuf_copy (src);
    if (!dst)
        return NULL;

    const gint    width    = gdk_pixbuf_get_width (dst);
    const gint    height   = gdk_pixbuf_get_height (dst);
    const gint    rowstride = gdk_pixbuf_get_rowstride (dst);
    const gint    nchan    = gdk_pixbuf_get_n_channels (dst);
    guchar       *pixels   = gdk_pixbuf_get_pixels (dst);

    for (gint y = 0; y < height; y++)
    {
        guchar *row = pixels + y * rowstride;
        for (gint x = 0; x < width; x++)
        {
            guchar *p = row + x * nchan;
            p[0] = lut[p[0]];
            p[1] = lut[p[1]];
            p[2] = lut[p[2]];
            /* alpha (p[3] when present) left untouched */
        }
    }

    return dst;
}

static void
pn_image_gamma_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Gamma", gamma_transform);
}

static void
pn_image_gamma_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    PnImageGamma *self = PN_IMAGE_GAMMA (object);

    switch (prop_id)
    {
    case PROP_GAMMA:
        g_value_set_double (value, self->gamma);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_gamma_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    PnImageGamma *self = PN_IMAGE_GAMMA (object);

    switch (prop_id)
    {
    case PROP_GAMMA:
        self->gamma = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_gamma_class_init (PnImageGammaClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_gamma_get_property;
    object_class->set_property = pn_image_gamma_set_property;

    node_class->receive    = pn_image_gamma_receive;
    node_class->class_name = "Gamma";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_GAMMA] = g_param_spec_double (
            "gamma", "Gamma",
            "Gamma exponent; values below 1 darken midtones, "
            "above 1 lighten them",
            0.1, 5.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_gamma_init (PnImageGamma *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->gamma = 1.0;

    pn_node_set_class_name (node, "Gamma");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
