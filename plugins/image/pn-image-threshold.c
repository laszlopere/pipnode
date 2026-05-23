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

#include "pn-image-threshold.h"
#include "pn-image-ops.h"

/* Threshold: hard two-tone conversion.  Each pixel's luma is compared to
 * the `level` cutoff: at or above becomes white, below becomes black.
 * Alpha is preserved. */

struct _PnImageThreshold
{
    PnNode parent_instance;

    /* Luma cutoff, 0..255. */
    gint level;
};

G_DEFINE_TYPE (PnImageThreshold, pn_image_threshold, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_LEVEL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void
threshold_point (guchar *p, PnNode *node)
{
    gdouble L = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    guchar  v = (L >= PN_IMAGE_THRESHOLD (node)->level) ? 255 : 0;

    p[0] = p[1] = p[2] = v;
}

static GdkPixbuf *
threshold_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point (src, threshold_point, node);
}

static void
pn_image_threshold_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Threshold", threshold_transform);
}

static void
pn_image_threshold_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    PnImageThreshold *self = PN_IMAGE_THRESHOLD (object);

    switch (prop_id)
    {
    case PROP_LEVEL:
        g_value_set_int (value, self->level);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_threshold_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    PnImageThreshold *self = PN_IMAGE_THRESHOLD (object);

    switch (prop_id)
    {
    case PROP_LEVEL:
        self->level = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_threshold_class_init (PnImageThresholdClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_threshold_get_property;
    object_class->set_property = pn_image_threshold_set_property;

    node_class->receive    = pn_image_threshold_receive;
    node_class->class_name = "Threshold";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_LEVEL] = g_param_spec_int (
            "level", "Level",
            "Luma cutoff: pixels at or above become white, "
            "below become black",
            0, 255, 128,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_threshold_init (PnImageThreshold *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->level = 128;

    pn_node_set_class_name (node, "Threshold");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
