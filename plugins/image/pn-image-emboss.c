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

#include "pn-image-emboss.h"
#include "pn-image-ops.h"

/* Emboss: a directional 3×3 kernel run on the image's luma with a +128
 * bias, giving the classic grey relief where edges look raised or
 * carved depending on their orientation. */

struct _PnImageEmboss
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageEmboss, pn_image_emboss, PN_TYPE_NODE)

static GdkPixbuf *
emboss_transform (GdkPixbuf *src, PnNode *node)
{
    static const gdouble kernel[9] = {
        -2.0, -1.0,  0.0,
        -1.0,  1.0,  1.0,
         0.0,  1.0,  2.0,
    };
    (void) node;
    return pn_image_convolve (src, kernel, 3, 1.0, 128.0, TRUE);
}

static void
pn_image_emboss_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Emboss", emboss_transform);
}

static void
pn_image_emboss_class_init (PnImageEmbossClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_emboss_receive;
    node_class->class_name = "Emboss";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_SHARPEN;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_emboss_init (PnImageEmboss *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Emboss");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
