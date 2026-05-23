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

#include "pn-image-resize.h"
#include "pn-image-ops.h"

/* Resize: scale the image to the exact `width` × `height` given by the
 * properties, using bilinear interpolation.  The aspect ratio is not
 * preserved unless the requested size matches it.  Any alpha channel is
 * preserved. */

struct _PnImageResize
{
    PnNode parent_instance;

    /* Target dimensions, in pixels. */
    gint width;
    gint height;
};

G_DEFINE_TYPE (PnImageResize, pn_image_resize, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_WIDTH,
    PROP_HEIGHT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
resize_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageResize *self = PN_IMAGE_RESIZE (node);

    return gdk_pixbuf_scale_simple (src, self->width, self->height,
                                    GDK_INTERP_BILINEAR);
}

static void
pn_image_resize_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Resize", resize_transform);
}

static void
pn_image_resize_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnImageResize *self = PN_IMAGE_RESIZE (object);

    switch (prop_id)
    {
    case PROP_WIDTH:
        g_value_set_int (value, self->width);
        break;
    case PROP_HEIGHT:
        g_value_set_int (value, self->height);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_resize_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnImageResize *self = PN_IMAGE_RESIZE (object);

    switch (prop_id)
    {
    case PROP_WIDTH:
        self->width = g_value_get_int (value);
        break;
    case PROP_HEIGHT:
        self->height = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_resize_class_init (PnImageResizeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_resize_get_property;
    object_class->set_property = pn_image_resize_set_property;

    node_class->receive    = pn_image_resize_receive;
    node_class->class_name = "Resize";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_GEOMETRY;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_WIDTH] = g_param_spec_int (
            "width", "Width",
            "Target width of the scaled image, in pixels",
            1, 100000, 256,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEIGHT] = g_param_spec_int (
            "height", "Height",
            "Target height of the scaled image, in pixels",
            1, 100000, 256,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_resize_init (PnImageResize *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->width  = 256;
    self->height = 256;

    pn_node_set_class_name (node, "Resize");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
