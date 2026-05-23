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

#include "pn-image-unsharp-mask.h"
#include "pn-image-ops.h"

/* Unsharp Mask: subtract a blurred copy of the image from the original to
 * isolate the edges, then add a scaled version of that difference back, so
 * edges are accentuated.  The `amount` property scales the edge boost and
 * `radius` sets the blur radius of the mask.  Results are clamped to
 * [0, 255]; alpha is preserved. */

struct _PnImageUnsharpMask
{
    PnNode parent_instance;

    /* How strongly edges are accentuated. */
    gdouble amount;

    /* Blur radius of the mask. */
    gint radius;
};

G_DEFINE_TYPE (PnImageUnsharpMask, pn_image_unsharp_mask, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_AMOUNT,
    PROP_RADIUS,
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
unsharp_mask_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageUnsharpMask *self   = PN_IMAGE_UNSHARP_MASK (node);
    gint                radius = self->radius;
    gdouble             amount = self->amount;
    gint                i, x, y, c;

    /* Box-blur mask: a flat 1-D kernel of length n, normalised by n. */
    gint     n = 2 * radius + 1;
    gdouble *k = g_malloc (sizeof (gdouble) * n);
    for (i = 0; i < n; i++)
        k[i] = 1.0;

    GdkPixbuf *blur = pn_image_separable (src, k, n, (gdouble) n);
    g_free (k);
    if (!blur)
        return NULL;

    GdkPixbuf *dst = gdk_pixbuf_copy (src);
    if (!dst)
    {
        g_object_unref (blur);
        return NULL;
    }

    /* src, blur and dst share geometry and alpha-ness. */
    const gint    w   = gdk_pixbuf_get_width      (src);
    const gint    h   = gdk_pixbuf_get_height     (src);
    const gint    nch = gdk_pixbuf_get_n_channels (src);
    const gint    rs  = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp  = gdk_pixbuf_get_pixels     (src);
    const guchar *bp  = gdk_pixbuf_get_pixels     (blur);
    guchar       *dp  = gdk_pixbuf_get_pixels     (dst);

    for (y = 0; y < h; y++)
    {
        const guchar *srow = sp + (gsize) y * rs;
        const guchar *brow = bp + (gsize) y * rs;
        guchar       *drow = dp + (gsize) y * rs;

        for (x = 0; x < w; x++)
        {
            const guchar *spx = srow + (gsize) x * nch;
            const guchar *bpx = brow + (gsize) x * nch;
            guchar       *dpx = drow + (gsize) x * nch;

            for (c = 0; c < 3; c++)
            {
                gdouble s   = spx[c];
                gdouble b   = bpx[c];
                gdouble out = s + amount * (s - b);
                dpx[c] = clamp_u8 (out);
            }
            /* alpha (if any) left as the copy */
        }
    }

    g_object_unref (blur);
    return dst;
}

static void
pn_image_unsharp_mask_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Unsharp Mask", unsharp_mask_transform);
}

static void
pn_image_unsharp_mask_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    PnImageUnsharpMask *self = PN_IMAGE_UNSHARP_MASK (object);

    switch (prop_id)
    {
    case PROP_AMOUNT:
        g_value_set_double (value, self->amount);
        break;
    case PROP_RADIUS:
        g_value_set_int (value, self->radius);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_unsharp_mask_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
    PnImageUnsharpMask *self = PN_IMAGE_UNSHARP_MASK (object);

    switch (prop_id)
    {
    case PROP_AMOUNT:
        self->amount = g_value_get_double (value);
        break;
    case PROP_RADIUS:
        self->radius = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_unsharp_mask_class_init (PnImageUnsharpMaskClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_unsharp_mask_get_property;
    object_class->set_property = pn_image_unsharp_mask_set_property;

    node_class->receive    = pn_image_unsharp_mask_receive;
    node_class->class_name = "Unsharp Mask";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_SHARPEN;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_AMOUNT] = g_param_spec_double (
            "amount", "Amount",
            "How strongly edges are accentuated",
            0.0, 5.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RADIUS] = g_param_spec_int (
            "radius", "Radius",
            "Blur radius of the mask; larger softens a wider halo",
            1, 15, 3,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_unsharp_mask_init (PnImageUnsharpMask *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->amount = 1.0;
    self->radius = 3;

    pn_node_set_class_name (node, "Unsharp Mask");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
