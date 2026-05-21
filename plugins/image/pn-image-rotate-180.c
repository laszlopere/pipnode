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

#include "pn-image-rotate-180.h"
#include "pn-image-ops.h"

/* Rotate 180°: turn the image upside down (a half-turn).  The result keeps
 * the same dimensions.  Any alpha channel is preserved. */

struct _PnImageRotate180
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageRotate180, pn_image_rotate_180, PN_TYPE_NODE)

static GdkPixbuf *
rotate_180_transform (GdkPixbuf *src, PnNode *node)
{
    (void) node;
    return gdk_pixbuf_rotate_simple (src, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
}

static void
pn_image_rotate_180_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Rotate 180°", rotate_180_transform);
}

static void
pn_image_rotate_180_class_init (PnImageRotate180Class *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_rotate_180_receive;
    node_class->class_name = "Rotate 180°";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_GEOMETRY;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_rotate_180_init (PnImageRotate180 *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Rotate 180°");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
