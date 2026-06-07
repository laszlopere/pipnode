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

#include "pn-image-pixelate.h"
#include "pn-image-ops.h"

/* Pixelate: split the image into a grid of square blocks `block` pixels
 * on a side, average each block's colour and paint the whole block that
 * mean — the classic mosaic / "censor" look.  Alpha is preserved. */

struct _PnImagePixelate
{
    PnNode parent_instance;

    /* Edge length in pixels of each averaged block. */
    gint block;
};

G_DEFINE_TYPE (PnImagePixelate, pn_image_pixelate, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_BLOCK,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
pixelate_transform (GdkPixbuf *src, PnNode *node)
{
    const gint block = PN_IMAGE_PIXELATE (node)->block;
    GdkPixbuf *dst = gdk_pixbuf_copy (src);  /* keeps pixels + alpha */
    const gint w   = gdk_pixbuf_get_width  (src);
    const gint h   = gdk_pixbuf_get_height (src);
    const gint nch = gdk_pixbuf_get_n_channels (src);
    const gint srs = gdk_pixbuf_get_rowstride  (src);
    const guchar *sbase = gdk_pixbuf_get_pixels (src);
    gint drs;
    guchar *dbase;
    gint bx, by, i, j;

    if (dst == NULL)
        return NULL;

    drs   = gdk_pixbuf_get_rowstride (dst);
    dbase = gdk_pixbuf_get_pixels (dst);

    for (by = 0; by < h; by += block)
    {
        const gint bh = MIN (block, h - by);
        for (bx = 0; bx < w; bx += block)
        {
            const gint bw = MIN (block, w - bx);
            gulong sr = 0, sg = 0, sb = 0;
            guchar mr, mg, mb;
            const gulong n = (gulong) bw * bh;

            for (j = 0; j < bh; j++)
            {
                const guchar *row = sbase + (gsize) (by + j) * srs;
                for (i = 0; i < bw; i++)
                {
                    const guchar *p = row + (bx + i) * nch;
                    sr += p[0];
                    sg += p[1];
                    sb += p[2];
                }
            }

            mr = (guchar) (sr / n);
            mg = (guchar) (sg / n);
            mb = (guchar) (sb / n);

            for (j = 0; j < bh; j++)
            {
                guchar *row = dbase + (gsize) (by + j) * drs;
                for (i = 0; i < bw; i++)
                {
                    guchar *p = row + (bx + i) * nch;
                    p[0] = mr;
                    p[1] = mg;
                    p[2] = mb;
                }
            }
        }
    }

    return dst;
}

static void
pn_image_pixelate_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Pixelate", pixelate_transform);
}

static void
pn_image_pixelate_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImagePixelate *self = PN_IMAGE_PIXELATE (object);

    switch (prop_id)
    {
    case PROP_BLOCK:
        g_value_set_int (value, self->block);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_pixelate_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImagePixelate *self = PN_IMAGE_PIXELATE (object);

    switch (prop_id)
    {
    case PROP_BLOCK:
        self->block = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_pixelate_class_init (PnImagePixelateClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_pixelate_get_property;
    object_class->set_property = pn_image_pixelate_set_property;

    node_class->receive    = pn_image_pixelate_receive;
    node_class->class_name = "Pixelate";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_BLOCK] = g_param_spec_int (
            "block", "Block",
            "Edge length in pixels of each averaged block",
            2, 64, 8,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_pixelate_init (PnImagePixelate *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->block = 8;

    pn_node_set_class_name (node, "Pixelate");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
