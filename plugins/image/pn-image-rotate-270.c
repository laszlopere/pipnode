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

#include "pn-image-rotate-270.h"
#include "pn-image-ops.h"

/* Rotate 90° CCW: turn the image a quarter-turn counter-clockwise.  Width
 * and height are swapped in the result.  Any alpha channel is preserved. */

struct _PnImageRotate270
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageRotate270, pn_image_rotate_270, PN_TYPE_NODE)

static GdkPixbuf *
rotate_270_transform (GdkPixbuf *src, PnNode *node)
{
    (void) node;
    return gdk_pixbuf_rotate_simple (src, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
}

static void
pn_image_rotate_270_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Rotate 90° CCW", rotate_270_transform);
}

static void
pn_image_rotate_270_class_init (PnImageRotate270Class *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_rotate_270_receive;
    node_class->class_name = "Rotate 90° CCW";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_GEOMETRY;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_rotate_270_init (PnImageRotate270 *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Rotate 90° CCW");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
