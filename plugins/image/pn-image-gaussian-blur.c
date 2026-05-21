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

#include "pn-image-gaussian-blur.h"
#include "pn-image-ops.h"

#include <math.h>

/* Gaussian blur: a separable Gaussian whose 1-D kernel is built from the
 * `radius` property (in pixels).  The standard deviation is radius/2, so
 * the weights have effectively decayed to nothing by the kernel edge.
 * A radius below half a pixel is a no-op pass-through. */

struct _PnImageGaussianBlur
{
    PnNode parent_instance;

    /* Blur radius in pixels; the kernel spans [-ceil(radius), +ceil] and
     * sigma = radius / 2. */
    gdouble radius;
};

G_DEFINE_TYPE (PnImageGaussianBlur, pn_image_gaussian_blur, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_RADIUS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
gaussian_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageGaussianBlur *self   = PN_IMAGE_GAUSSIAN_BLUR (node);
    const gdouble        radius = self->radius;
    gdouble              sigma, sum;
    gint                 kr, n, i;
    gdouble             *kernel;
    GdkPixbuf           *out;

    if (radius < 0.5)
        return gdk_pixbuf_copy (src);   /* nothing to do */

    sigma = radius / 2.0;
    kr    = (gint) ceil (radius);
    n     = 2 * kr + 1;

    kernel = g_malloc (n * sizeof (gdouble));
    sum    = 0.0;
    for (i = -kr; i <= kr; i++)
    {
        const gdouble w = exp (-(gdouble) (i * i) / (2.0 * sigma * sigma));
        kernel[i + kr] = w;
        sum += w;
    }

    out = pn_image_separable (src, kernel, n, sum);
    g_free (kernel);
    return out;
}

static void
pn_image_gaussian_blur_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Gaussian Blur", gaussian_transform);
}

static void
pn_image_gaussian_blur_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
    PnImageGaussianBlur *self = PN_IMAGE_GAUSSIAN_BLUR (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        g_value_set_double (value, self->radius);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_gaussian_blur_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
    PnImageGaussianBlur *self = PN_IMAGE_GAUSSIAN_BLUR (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        self->radius = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_gaussian_blur_class_init (PnImageGaussianBlurClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_gaussian_blur_get_property;
    object_class->set_property = pn_image_gaussian_blur_set_property;

    node_class->receive    = pn_image_gaussian_blur_receive;
    node_class->class_name = "Gaussian Blur";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_BLUR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_RADIUS] = g_param_spec_double (
            "radius", "Radius",
            "Blur radius in pixels (sigma = radius / 2); 0 disables the blur",
            0.0, 50.0, 2.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_gaussian_blur_init (PnImageGaussianBlur *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->radius = 2.0;

    pn_node_set_class_name (node, "Gaussian Blur");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
