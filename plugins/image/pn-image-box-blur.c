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

#include "pn-image-box-blur.h"
#include "pn-image-ops.h"

/* Box blur: a separable moving-average over a (2·radius+1)-wide window —
 * the cheapest blur, giving a flat, boxy softening.  Uniform weights, so
 * each pass is a plain average. */

struct _PnImageBoxBlur
{
    PnNode parent_instance;

    /* Half-width of the averaging window, in pixels. */
    gint radius;
};

G_DEFINE_TYPE (PnImageBoxBlur, pn_image_box_blur, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_RADIUS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
box_blur_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageBoxBlur *self = PN_IMAGE_BOX_BLUR (node);
    gint            r    = self->radius;
    gint            n, i;
    gdouble        *kernel;
    GdkPixbuf      *out;

    if (r < 1)
        return gdk_pixbuf_copy (src);

    n      = 2 * r + 1;
    kernel = g_malloc (n * sizeof (gdouble));
    for (i = 0; i < n; i++)
        kernel[i] = 1.0;

    out = pn_image_separable (src, kernel, n, (gdouble) n);
    g_free (kernel);
    return out;
}

static void
pn_image_box_blur_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Box Blur", box_blur_transform);
}

static void
pn_image_box_blur_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImageBoxBlur *self = PN_IMAGE_BOX_BLUR (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        g_value_set_int (value, self->radius);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_box_blur_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImageBoxBlur *self = PN_IMAGE_BOX_BLUR (object);

    switch (prop_id)
    {
    case PROP_RADIUS:
        self->radius = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_box_blur_class_init (PnImageBoxBlurClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_box_blur_get_property;
    object_class->set_property = pn_image_box_blur_set_property;

    node_class->receive    = pn_image_box_blur_receive;
    node_class->class_name = "Box Blur";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_BLUR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_RADIUS] = g_param_spec_int (
            "radius", "Radius",
            "Half-width of the averaging window in pixels "
            "(window = 2·radius + 1)",
            1, 25, 1,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_box_blur_init (PnImageBoxBlur *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->radius = 1;

    pn_node_set_class_name (node, "Box Blur");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
