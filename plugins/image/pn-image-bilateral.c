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

#include "pn-image-bilateral.h"
#include "pn-image-ops.h"

/* Bilateral filter: edge-preserving smoothing.  Each output pixel is a
 * weighted average of its spatial neighbourhood, where a neighbour's
 * weight falls off both with its distance (spatial sigma, tied to the
 * window radius) and with how different its colour is from the centre
 * (colour sigma).  Large colour differences get tiny weights, so edges
 * are kept while flat regions are smoothed.  Alpha is preserved. */

struct _PnImageBilateral
{
    PnNode parent_instance;

    /* Spatial window radius. */
    gint radius;

    /* Colour sigma. */
    gdouble sigma;
};

G_DEFINE_TYPE (PnImageBilateral, pn_image_bilateral, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_RADIUS,
    PROP_SIGMA,
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
bilateral_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageBilateral *self   = PN_IMAGE_BILATERAL (node);
    gint              radius = self->radius;
    gdouble           sigmaC = self->sigma;
    gdouble           sigmaS = radius;
    gint              x, y, i, j;

    gdouble inv2sc = 1.0 / (2.0 * sigmaC * sigmaC);
    gdouble inv2ss = 1.0 / (2.0 * sigmaS * sigmaS);

    /* Copy keeps alpha + geometry; we overwrite the RGB channels. */
    GdkPixbuf *dst = gdk_pixbuf_copy (src);
    if (!dst)
        return NULL;

    const gint    w   = gdk_pixbuf_get_width      (src);
    const gint    h   = gdk_pixbuf_get_height     (src);
    const gint    nch = gdk_pixbuf_get_n_channels (src);
    const gint    rs  = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp  = gdk_pixbuf_get_pixels     (src);
    guchar       *dp  = gdk_pixbuf_get_pixels     (dst);

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            const guchar *cpx = sp + (gsize) y * rs + (gsize) x * nch;
            gdouble cr = cpx[0], cg = cpx[1], cb = cpx[2];

            gdouble wsum = 0.0, ar = 0.0, ag = 0.0, ab = 0.0;

            for (j = -radius; j <= radius; j++)
            {
                for (i = -radius; i <= radius; i++)
                {
                    gint sx = CLAMP (x + i, 0, w - 1);
                    gint sy = CLAMP (y + j, 0, h - 1);
                    const guchar *npx =
                        sp + (gsize) sy * rs + (gsize) sx * nch;
                    gdouble nr = npx[0], ng = npx[1], nb = npx[2];

                    /* colour distance² */
                    gdouble dc = (nr - cr) * (nr - cr)
                               + (ng - cg) * (ng - cg)
                               + (nb - cb) * (nb - cb);

                    gdouble ws = exp (-(i * i + j * j) * inv2ss)
                               * exp (-dc * inv2sc);

                    wsum += ws;
                    ar   += ws * nr;
                    ag   += ws * ng;
                    ab   += ws * nb;
                }
            }

            guchar *dpx = dp + (gsize) y * rs + (gsize) x * nch;
            dpx[0] = clamp_u8 (ar / wsum);
            dpx[1] = clamp_u8 (ag / wsum);
            dpx[2] = clamp_u8 (ab / wsum);
            /* alpha (if any) preserved by the copy */
        }
    }

    return dst;
}

static void
pn_image_bilateral_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Bilateral", bilateral_transform);
}

static void
pn_image_bilateral_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    PnImageBilateral *self = PN_IMAGE_BILATERAL (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        g_value_set_int (value, self->radius);
        break;
    case PROP_SIGMA:
        g_value_set_double (value, self->sigma);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_bilateral_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    PnImageBilateral *self = PN_IMAGE_BILATERAL (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        self->radius = g_value_get_int (value);
        break;
    case PROP_SIGMA:
        self->sigma = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_bilateral_class_init (PnImageBilateralClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_bilateral_get_property;
    object_class->set_property = pn_image_bilateral_set_property;

    node_class->receive    = pn_image_bilateral_receive;
    node_class->class_name = "Bilateral";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_BLUR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_RADIUS] = g_param_spec_int (
            "radius", "Radius",
            "Spatial window radius",
            1, 7, 3,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SIGMA] = g_param_spec_double (
            "sigma", "Sigma",
            "Colour sigma: larger blends across bigger colour differences "
            "(less edge-preserving)",
            1.0, 150.0, 30.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_bilateral_init (PnImageBilateral *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->radius = 3;
    self->sigma  = 30.0;

    pn_node_set_class_name (node, "Bilateral");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
