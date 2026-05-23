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

#include "pn-image-dither.h"
#include "pn-image-ops.h"

/* Dither: reduce the image to pure black and white using Floyd–Steinberg
 * error diffusion on luma, spreading each pixel's quantisation error to
 * its neighbours so the 1-bit result still reads as continuous tone.
 * Alpha is preserved. */

struct _PnImageDither
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageDither, pn_image_dither, PN_TYPE_NODE)

static GdkPixbuf *
dither_transform (GdkPixbuf *src, PnNode *node)
{
    const gint w   = gdk_pixbuf_get_width  (src);
    const gint h   = gdk_pixbuf_get_height (src);
    const gint nch = gdk_pixbuf_get_n_channels (src);
    const gint srs = gdk_pixbuf_get_rowstride  (src);
    const guchar *sbase = gdk_pixbuf_get_pixels (src);
    GdkPixbuf *dst;
    gint drs;
    guchar *dbase;
    gdouble *buf;
    gint x, y;

    (void) node;

    buf = g_malloc (sizeof (gdouble) * (gsize) w * h);
    for (y = 0; y < h; y++)
    {
        const guchar *row = sbase + (gsize) y * srs;
        for (x = 0; x < w; x++)
        {
            const guchar *p = row + x * nch;
            buf[(gsize) y * w + x] = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
        }
    }

    dst   = gdk_pixbuf_copy (src);  /* keeps pixels + alpha */
    drs   = gdk_pixbuf_get_rowstride (dst);
    dbase = gdk_pixbuf_get_pixels    (dst);

    for (y = 0; y < h; y++)
    {
        guchar *drow = dbase + (gsize) y * drs;
        for (x = 0; x < w; x++)
        {
            gdouble old = buf[(gsize) y * w + x];
            gdouble nw  = (old < 128.0) ? 0.0 : 255.0;
            gdouble err = old - nw;
            guchar *p   = drow + x * nch;

            p[0] = p[1] = p[2] = (guchar) nw;

            if (x + 1 < w)
                buf[(gsize) y * w + x + 1]       += err * 7.0 / 16.0;
            if (x - 1 >= 0 && y + 1 < h)
                buf[(gsize) (y + 1) * w + x - 1] += err * 3.0 / 16.0;
            if (y + 1 < h)
                buf[(gsize) (y + 1) * w + x]     += err * 5.0 / 16.0;
            if (x + 1 < w && y + 1 < h)
                buf[(gsize) (y + 1) * w + x + 1] += err * 1.0 / 16.0;
        }
    }

    g_free (buf);
    return dst;
}

static void
pn_image_dither_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Dither", dither_transform);
}

static void
pn_image_dither_class_init (PnImageDitherClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_dither_receive;
    node_class->class_name = "Dither";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_dither_init (PnImageDither *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Dither");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
