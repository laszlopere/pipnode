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

#include "pn-image-oil-paint.h"
#include "pn-image-ops.h"

/* Oil Paint: a classic intensity-histogram oil-paint filter.  For each
 * pixel the (2·radius+1)² neighbourhood is binned into `levels` intensity
 * buckets; the most-populated bucket's mean colour becomes the output,
 * which flattens detail into broad painterly strokes.  Alpha is
 * preserved. */

struct _PnImageOilPaint
{
    PnNode parent_instance;

    /* Half-width of the sampling window, in pixels. */
    gint radius;
    /* Number of intensity buckets. */
    gint levels;
};

G_DEFINE_TYPE (PnImageOilPaint, pn_image_oil_paint, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_RADIUS,
    PROP_LEVELS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
oil_paint_transform (GdkPixbuf *src, PnNode *node)
{
    const gint radius = PN_IMAGE_OIL_PAINT (node)->radius;
    const gint levels = PN_IMAGE_OIL_PAINT (node)->levels;
    const gint nbuckets = levels;
    const gint w   = gdk_pixbuf_get_width  (src);
    const gint h   = gdk_pixbuf_get_height (src);
    const gint nch = gdk_pixbuf_get_n_channels (src);
    const gint srs = gdk_pixbuf_get_rowstride  (src);
    const guchar *sbase = gdk_pixbuf_get_pixels (src);
    GdkPixbuf *dst = gdk_pixbuf_copy (src);  /* keeps pixels + alpha */
    gint drs;
    guchar *dbase;
    gint x, y, i, j;

    if (dst == NULL)
        return NULL;

    drs   = gdk_pixbuf_get_rowstride (dst);
    dbase = gdk_pixbuf_get_pixels    (dst);

    for (y = 0; y < h; y++)
    {
        guchar *drow = dbase + (gsize) y * drs;
        for (x = 0; x < w; x++)
        {
            gint    count[64];
            gdouble rsum[64], gsum[64], bsum[64];
            gint    b, best;
            guchar *out = drow + x * nch;

            for (b = 0; b < nbuckets; b++)
            {
                count[b] = 0;
                rsum[b] = gsum[b] = bsum[b] = 0.0;
            }

            for (j = -radius; j <= radius; j++)
            {
                gint sy = CLAMP (y + j, 0, h - 1);
                const guchar *row = sbase + (gsize) sy * srs;
                for (i = -radius; i <= radius; i++)
                {
                    gint sx = CLAMP (x + i, 0, w - 1);
                    const guchar *p = row + sx * nch;
                    gdouble L = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    gint bucket = (gint) (L * nbuckets / 256.0);

                    if (bucket >= nbuckets) bucket = nbuckets - 1;

                    count[bucket]++;
                    rsum[bucket] += p[0];
                    gsum[bucket] += p[1];
                    bsum[bucket] += p[2];
                }
            }

            best = 0;
            for (b = 1; b < nbuckets; b++)
                if (count[b] > count[best])
                    best = b;

            out[0] = (guchar) (rsum[best] / count[best] + 0.5);
            out[1] = (guchar) (gsum[best] / count[best] + 0.5);
            out[2] = (guchar) (bsum[best] / count[best] + 0.5);
        }
    }

    return dst;
}

static void
pn_image_oil_paint_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Oil Paint", oil_paint_transform);
}

static void
pn_image_oil_paint_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    PnImageOilPaint *self = PN_IMAGE_OIL_PAINT (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        g_value_set_int (value, self->radius);
        break;
    case PROP_LEVELS:
        g_value_set_int (value, self->levels);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_oil_paint_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    PnImageOilPaint *self = PN_IMAGE_OIL_PAINT (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        self->radius = g_value_get_int (value);
        break;
    case PROP_LEVELS:
        self->levels = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_oil_paint_class_init (PnImageOilPaintClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_oil_paint_get_property;
    object_class->set_property = pn_image_oil_paint_set_property;

    node_class->receive    = pn_image_oil_paint_receive;
    node_class->class_name = "Oil Paint";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_RADIUS] = g_param_spec_int (
            "radius", "Radius",
            "Half-width of the sampling window in pixels "
            "(window = 2·radius + 1); larger gives broader strokes",
            1, 7, 3,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LEVELS] = g_param_spec_int (
            "levels", "Levels",
            "Number of intensity buckets; fewer flattens detail more",
            2, 64, 20,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_oil_paint_init (PnImageOilPaint *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->radius = 3;
    self->levels = 20;

    pn_node_set_class_name (node, "Oil Paint");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
