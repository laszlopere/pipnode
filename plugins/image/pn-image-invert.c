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

#include "pn-image-invert.h"
#include "pn-image-ops.h"

/* Invert: photographic negative — each colour channel becomes 255 − c.
 * Alpha is preserved. */

struct _PnImageInvert
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageInvert, pn_image_invert, PN_TYPE_NODE)

static void
invert_point (guchar *p, PnNode *node)
{
    (void) node;
    p[0] = 255 - p[0];
    p[1] = 255 - p[1];
    p[2] = 255 - p[2];
}

static GdkPixbuf *
invert_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, invert_point, node);
}

static void
pn_image_invert_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Invert", invert_transform);
}

static void
pn_image_invert_class_init (PnImageInvertClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_invert_receive;
    node_class->class_name = "Invert";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COLOR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_invert_init (PnImageInvert *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Invert");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
