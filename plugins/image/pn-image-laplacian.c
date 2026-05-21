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

#include "pn-image-laplacian.h"
#include "pn-image-ops.h"

/* Laplacian edge detection: the second-derivative 3×3 kernel run on the
 * image's luma.  Flat regions go black; intensity transitions in any
 * direction light up, so the result is an isotropic edge map. */

struct _PnImageLaplacian
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageLaplacian, pn_image_laplacian, PN_TYPE_NODE)

static GdkPixbuf *
laplacian_transform (GdkPixbuf *src, PnNode *node)
{
    static const gdouble kernel[9] = {
        0.0,  1.0,  0.0,
        1.0, -4.0,  1.0,
        0.0,  1.0,  0.0,
    };
    (void) node;
    return pn_image_convolve (src, kernel, 3, 1.0, 0.0, TRUE);
}

static void
pn_image_laplacian_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Laplacian Edge", laplacian_transform);
}

static void
pn_image_laplacian_class_init (PnImageLaplacianClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_laplacian_receive;
    node_class->class_name = "Laplacian Edge";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_EDGE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_laplacian_init (PnImageLaplacian *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Laplacian Edge");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
