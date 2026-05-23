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

#include "pn-image-sepia.h"
#include "pn-image-ops.h"

/* Sepia: the classic warm-brown photographic tone, produced by the
 * standard sepia colour matrix applied per pixel.  Alpha is preserved. */

struct _PnImageSepia
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageSepia, pn_image_sepia, PN_TYPE_NODE)

static inline guchar
clamp_u8 (gdouble v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (guchar) (v + 0.5);
}

static void
sepia_point (guchar *p, PnNode *node)
{
    const gdouble r = p[0], g = p[1], b = p[2];
    (void) node;
    p[0] = clamp_u8 (0.393 * r + 0.769 * g + 0.189 * b);
    p[1] = clamp_u8 (0.349 * r + 0.686 * g + 0.168 * b);
    p[2] = clamp_u8 (0.272 * r + 0.534 * g + 0.131 * b);
}

static GdkPixbuf *
sepia_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, sepia_point, node);
}

static void
pn_image_sepia_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Sepia", sepia_transform);
}

static void
pn_image_sepia_class_init (PnImageSepiaClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_sepia_receive;
    node_class->class_name = "Sepia";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COLOR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_sepia_init (PnImageSepia *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Sepia");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
