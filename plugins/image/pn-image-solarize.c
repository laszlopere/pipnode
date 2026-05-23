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

#include "pn-image-solarize.h"
#include "pn-image-ops.h"

/* Solarize: the Sabattier effect — each colour channel at or above the
 * `threshold` is inverted (255 − c) while darker values pass through, so
 * highlights flip and the image takes on the surreal tone-reversed look
 * of an over-exposed print.  Alpha is preserved. */

struct _PnImageSolarize
{
    PnNode parent_instance;

    /* Per-channel value at/above which the channel is inverted. */
    gint threshold;
};

G_DEFINE_TYPE (PnImageSolarize, pn_image_solarize, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_THRESHOLD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static inline guchar
solarize_channel (guchar c, gint threshold)
{
    return (c >= threshold) ? (guchar) (255 - c) : c;
}

static void
solarize_point (guchar *p, PnNode *node)
{
    const gint t = PN_IMAGE_SOLARIZE (node)->threshold;

    p[0] = solarize_channel (p[0], t);
    p[1] = solarize_channel (p[1], t);
    p[2] = solarize_channel (p[2], t);
}

static GdkPixbuf *
solarize_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, solarize_point, node);
}

static void
pn_image_solarize_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Solarize", solarize_transform);
}

static void
pn_image_solarize_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    PnImageSolarize *self = PN_IMAGE_SOLARIZE (object);

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
pn_image_solarize_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    PnImageSolarize *self = PN_IMAGE_SOLARIZE (object);

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
pn_image_solarize_class_init (PnImageSolarizeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_solarize_get_property;
    object_class->set_property = pn_image_solarize_set_property;

    node_class->receive    = pn_image_solarize_receive;
    node_class->class_name = "Solarize";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_THRESHOLD] = g_param_spec_int (
            "threshold", "Threshold",
            "Per-channel value (0-255) at or above which the channel is "
            "inverted; lower flips more of the image",
            0, 255, 128,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_solarize_init (PnImageSolarize *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->threshold = 128;

    pn_node_set_class_name (node, "Solarize");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
