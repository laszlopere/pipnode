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

#include "pn-image-black-white.h"
#include "pn-image-ops.h"

/* Black & White: a hard two-tone threshold.  Each pixel's Rec.601 luma
 * is compared against the `threshold` property; at or above it the pixel
 * becomes pure white, below it pure black.  Alpha is preserved. */

struct _PnImageBlackWhite
{
    PnNode parent_instance;

    /* Luma cutoff in [0, 255].  Lower keeps more white, higher more
     * black. */
    gint threshold;
};

G_DEFINE_TYPE (PnImageBlackWhite, pn_image_black_white, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_THRESHOLD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void
bw_point (guchar *p, PnNode *node)
{
    PnImageBlackWhite *self = PN_IMAGE_BLACK_WHITE (node);
    const gdouble      l    = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    const guchar       v    = (l >= (gdouble) self->threshold) ? 255 : 0;

    p[0] = p[1] = p[2] = v;
}

static GdkPixbuf *
bw_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, bw_point, node);
}

static void
pn_image_black_white_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Black & White", bw_transform);
}

static void
pn_image_black_white_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
    PnImageBlackWhite *self = PN_IMAGE_BLACK_WHITE (object);

    switch (prop_id)
    {
    case PROP_THRESHOLD:
        g_value_set_int (value, self->threshold);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_black_white_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
    PnImageBlackWhite *self = PN_IMAGE_BLACK_WHITE (object);

    switch (prop_id)
    {
    case PROP_THRESHOLD:
        self->threshold = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_black_white_class_init (PnImageBlackWhiteClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_black_white_get_property;
    object_class->set_property = pn_image_black_white_set_property;

    node_class->receive    = pn_image_black_white_receive;
    node_class->class_name = "Black & White";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COLOR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_THRESHOLD] = g_param_spec_int (
            "threshold", "Threshold",
            "Luma cut-off (0-255): pixels at or above it turn white, "
            "the rest turn black",
            0, 255, 128,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_black_white_init (PnImageBlackWhite *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->threshold = 128;

    pn_node_set_class_name (node, "Black & White");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
