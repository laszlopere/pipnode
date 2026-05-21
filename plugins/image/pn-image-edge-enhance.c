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

#include "pn-image-edge-enhance.h"
#include "pn-image-ops.h"

/* Edge Enhance: the full 3×3 eight-neighbour sharpen kernel (centre 9,
 * each neighbour −1) applied per colour channel.  It accentuates edges
 * more aggressively than the cross-shaped Sharpen node while keeping the
 * picture's colours. */

struct _PnImageEdgeEnhance
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageEdgeEnhance, pn_image_edge_enhance, PN_TYPE_NODE)

static GdkPixbuf *
edge_enhance_transform (GdkPixbuf *src, PnNode *node)
{
    static const gdouble kernel[9] = {
        -1.0, -1.0, -1.0,
        -1.0,  9.0, -1.0,
        -1.0, -1.0, -1.0,
    };
    (void) node;
    return pn_image_convolve (src, kernel, 3, 1.0, 0.0, FALSE);
}

static void
pn_image_edge_enhance_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Edge Enhance", edge_enhance_transform);
}

static void
pn_image_edge_enhance_class_init (PnImageEdgeEnhanceClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_edge_enhance_receive;
    node_class->class_name = "Edge Enhance";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_SHARPEN;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_edge_enhance_init (PnImageEdgeEnhance *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Edge Enhance");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
