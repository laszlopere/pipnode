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

#include "pn-image-hist-equalize.h"
#include "pn-image-ops.h"

/* Histogram Equalize: spread the image's luma across the full tonal
 * range so a flat, low-contrast picture gains contrast.  A luma
 * histogram is built over every pixel, its CDF mapped onto [0, 255], and
 * each pixel scaled by the ratio of its new luma to its old one, which
 * equalizes brightness while keeping hue.  Alpha is preserved.  There
 * are no properties. */

struct _PnImageHistEqualize
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageHistEqualize, pn_image_hist_equalize, PN_TYPE_NODE)

static inline guchar
clamp_u8 (gdouble v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (guchar) (v + 0.5);
}

static inline gint
luma_of (guchar r, guchar g, guchar b)
{
    gint l = (gint) (0.299 * r + 0.587 * g + 0.114 * b + 0.5);
    return CLAMP (l, 0, 255);
}

static GdkPixbuf *
hist_equalize_transform (GdkPixbuf *src, PnNode *node)
{
    (void) node;

    GdkPixbuf *dst = gdk_pixbuf_copy (src);
    if (!dst)
        return NULL;

    const gint    width    = gdk_pixbuf_get_width (dst);
    const gint    height   = gdk_pixbuf_get_height (dst);
    const gint    rowstride = gdk_pixbuf_get_rowstride (dst);
    const gint    nchan    = gdk_pixbuf_get_n_channels (dst);
    guchar       *pixels   = gdk_pixbuf_get_pixels (dst);

    /* Per-pixel luma histogram over all pixels. */
    glong hist[256] = { 0 };
    const glong count = (glong) width * height;

    for (gint y = 0; y < height; y++)
    {
        const guchar *row = pixels + y * rowstride;
        for (gint x = 0; x < width; x++)
        {
            const guchar *p = row + x * nchan;
            hist[luma_of (p[0], p[1], p[2])]++;
        }
    }

    /* CDF and the first non-zero bin. */
    glong cdf[256];
    glong acc     = 0;
    glong cdf_min = 0;
    gboolean have_min = FALSE;
    for (int i = 0; i < 256; i++)
    {
        acc += hist[i];
        cdf[i] = acc;
        if (!have_min && acc > 0)
        {
            cdf_min  = acc;
            have_min = TRUE;
        }
    }

    /* Equalized luma map. */
    guchar new_luma[256];
    if (count == cdf_min)
    {
        /* Single luma value present: identity map. */
        for (int i = 0; i < 256; i++)
            new_luma[i] = (guchar) i;
    }
    else
    {
        const gdouble denom = (gdouble) (count - cdf_min);
        for (int i = 0; i < 256; i++)
        {
            gdouble v = 255.0 * (cdf[i] - cdf_min) / denom;
            new_luma[i] = clamp_u8 (v);
        }
    }

    /* Scale each pixel toward its new luma, preserving hue. */
    for (gint y = 0; y < height; y++)
    {
        guchar *row = pixels + y * rowstride;
        for (gint x = 0; x < width; x++)
        {
            guchar      *p = row + x * nchan;
            const gint   l = luma_of (p[0], p[1], p[2]);
            const gdouble scale = (l > 0) ? (gdouble) new_luma[l] / l : 0.0;

            p[0] = clamp_u8 (p[0] * scale);
            p[1] = clamp_u8 (p[1] * scale);
            p[2] = clamp_u8 (p[2] * scale);
            /* alpha (p[3] when present) left untouched */
        }
    }

    return dst;
}

static void
pn_image_hist_equalize_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Histogram Equalize",
                           hist_equalize_transform);
}

static void
pn_image_hist_equalize_class_init (PnImageHistEqualizeClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_hist_equalize_receive;
    node_class->class_name = "Histogram Equalize";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_hist_equalize_init (PnImageHistEqualize *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Histogram Equalize");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
