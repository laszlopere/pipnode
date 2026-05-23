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

#include "pn-image-grayscale.h"
#include "pn-image-ops.h"

/* Grayscale: replace every pixel's RGB with its Rec.601 luma, leaving a
 * neutral grey of the same perceived brightness.  Alpha is preserved. */

struct _PnImageGrayscale
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageGrayscale, pn_image_grayscale, PN_TYPE_NODE)

static void
grayscale_point (guchar *p, PnNode *node)
{
    const guchar l = (guchar) (0.299 * p[0] + 0.587 * p[1]
                             + 0.114 * p[2] + 0.5);
    (void) node;
    p[0] = p[1] = p[2] = l;
}

static GdkPixbuf *
grayscale_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, grayscale_point, node);
}

static void
pn_image_grayscale_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Grayscale", grayscale_transform);
}

static void
pn_image_grayscale_class_init (PnImageGrayscaleClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_grayscale_receive;
    node_class->class_name = "Grayscale";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COLOR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_grayscale_init (PnImageGrayscale *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Grayscale");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
