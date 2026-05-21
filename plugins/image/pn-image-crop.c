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

#include "pn-image-crop.h"
#include "pn-image-ops.h"

/* Crop: cut out the rectangle (x, y, width, height) and emit it as a new,
 * smaller image.  The origin is clamped inside the source and the size is
 * trimmed to what remains, so the rectangle can never run off the edge.
 * Any alpha channel is preserved. */

struct _PnImageCrop
{
    PnNode parent_instance;

    /* Top-left corner and size of the rectangle to keep, in pixels. */
    gint x;
    gint y;
    gint width;
    gint height;
};

G_DEFINE_TYPE (PnImageCrop, pn_image_crop, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_X,
    PROP_Y,
    PROP_WIDTH,
    PROP_HEIGHT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
crop_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageCrop *self = PN_IMAGE_CROP (node);
    gint         w    = gdk_pixbuf_get_width (src);
    gint         h    = gdk_pixbuf_get_height (src);
    gint         x    = CLAMP (self->x, 0, w - 1);
    gint         y    = CLAMP (self->y, 0, h - 1);
    gint         cw   = MIN (self->width, w - x);
    gint         ch   = MIN (self->height, h - y);
    GdkPixbuf   *sub;
    GdkPixbuf   *out;

    if (cw < 1 || ch < 1)
        return gdk_pixbuf_copy (src);

    sub = gdk_pixbuf_new_subpixbuf (src, x, y, cw, ch);  /* shares pixels */
    out = gdk_pixbuf_copy (sub);                          /* own copy */
    g_object_unref (sub);
    return out;
}

static void
pn_image_crop_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Crop", crop_transform);
}

static void
pn_image_crop_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
    PnImageCrop *self = PN_IMAGE_CROP (object);

    switch (prop_id)
    {
    case PROP_X:
        g_value_set_int (value, self->x);
        break;
    case PROP_Y:
        g_value_set_int (value, self->y);
        break;
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
pn_image_crop_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
    PnImageCrop *self = PN_IMAGE_CROP (object);

    switch (prop_id)
    {
    case PROP_X:
        self->x = g_value_get_int (value);
        break;
    case PROP_Y:
        self->y = g_value_get_int (value);
        break;
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
pn_image_crop_class_init (PnImageCropClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_crop_get_property;
    object_class->set_property = pn_image_crop_set_property;

    node_class->receive    = pn_image_crop_receive;
    node_class->class_name = "Crop";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_GEOMETRY;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_X] = g_param_spec_int (
            "x", "X",
            "Left edge of the crop rectangle, in pixels",
            0, 100000, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_Y] = g_param_spec_int (
            "y", "Y",
            "Top edge of the crop rectangle, in pixels",
            0, 100000, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WIDTH] = g_param_spec_int (
            "width", "Width",
            "Width of the crop rectangle, in pixels",
            1, 100000, 256,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEIGHT] = g_param_spec_int (
            "height", "Height",
            "Height of the crop rectangle, in pixels",
            1, 100000, 256,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_crop_init (PnImageCrop *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->x      = 0;
    self->y      = 0;
    self->width  = 256;
    self->height = 256;

    pn_node_set_class_name (node, "Crop");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
