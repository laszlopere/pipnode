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

#include "pn-image-posterize.h"
#include "pn-image-ops.h"

/* Posterize: quantise each colour channel to `levels` evenly spaced
 * values, collapsing smooth gradients into flat bands.  2 levels is the
 * harshest, higher is closer to the original.  Alpha is preserved. */

struct _PnImagePosterize
{
    PnNode parent_instance;

    /* Number of output levels per channel (>= 2). */
    gint levels;
};

G_DEFINE_TYPE (PnImagePosterize, pn_image_posterize, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_LEVELS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static inline guchar
quantise (guchar c, gint levels)
{
    /* Bucket the input into one of `levels` bins, then spread the bins
     * back across the full [0, 255] range. */
    gint idx = (gint) c * levels / 256;     /* 0 .. levels-1 */
    if (idx > levels - 1)
        idx = levels - 1;
    return (guchar) (idx * 255 / (levels - 1));
}

static void
posterize_point (guchar *p, PnNode *node)
{
    const gint levels = PN_IMAGE_POSTERIZE (node)->levels;

    p[0] = quantise (p[0], levels);
    p[1] = quantise (p[1], levels);
    p[2] = quantise (p[2], levels);
}

static GdkPixbuf *
posterize_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, posterize_point, node);
}

static void
pn_image_posterize_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Posterize", posterize_transform);
}

static void
pn_image_posterize_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    PnImagePosterize *self = PN_IMAGE_POSTERIZE (object);

    switch (prop_id)
    {
    case PROP_LEVELS:
        g_value_set_int (value, self->levels);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_posterize_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    PnImagePosterize *self = PN_IMAGE_POSTERIZE (object);

    switch (prop_id)
    {
    case PROP_LEVELS:
        self->levels = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_posterize_class_init (PnImagePosterizeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_posterize_get_property;
    object_class->set_property = pn_image_posterize_set_property;

    node_class->receive    = pn_image_posterize_receive;
    node_class->class_name = "Posterize";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_LEVELS] = g_param_spec_int (
            "levels", "Levels",
            "Number of distinct values kept per colour channel "
            "(2 = harshest banding, higher is subtler)",
            2, 64, 4,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_posterize_init (PnImagePosterize *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->levels = 4;

    pn_node_set_class_name (node, "Posterize");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
