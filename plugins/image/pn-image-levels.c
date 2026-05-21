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

#include <math.h>

#include "pn-image-levels.h"
#include "pn-image-ops.h"

/* Levels: remap the tonal range.  Input values at or below `black` map
 * to 0 and at or above `white` map to 255; the span between is stretched
 * and reshaped by `gamma` (a midtone power curve).  The mapping is built
 * once into a 256-entry lookup table and applied to each colour channel.
 * Alpha is preserved. */

struct _PnImageLevels
{
    PnNode parent_instance;

    /* Input black point (mapped to 0) and white point (mapped to 255). */
    gint    black;
    gint    white;
    /* Midtone power curve applied across the remapped span. */
    gdouble gamma;
};

G_DEFINE_TYPE (PnImageLevels, pn_image_levels, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_BLACK,
    PROP_WHITE,
    PROP_GAMMA,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static inline guchar
clamp_u8 (gdouble v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (guchar) (v + 0.5);
}

static GdkPixbuf *
levels_transform (GdkPixbuf *src, PnNode *node)
{
    PnImageLevels *self = PN_IMAGE_LEVELS (node);

    gint    b = self->black;
    gint    w = self->white;
    gdouble g = self->gamma;

    if (w <= b)
        w = b + 1;
    if (g < 0.01)
        g = 0.01;

    guchar lut[256];
    for (int i = 0; i < 256; i++)
    {
        gdouble t = (i - b) / (gdouble) (w - b);
        t = CLAMP (t, 0.0, 1.0);
        t = pow (t, 1.0 / g);
        lut[i] = clamp_u8 (t * 255.0);
    }

    GdkPixbuf *dst = gdk_pixbuf_copy (src);
    if (!dst)
        return NULL;

    const gint    width    = gdk_pixbuf_get_width (dst);
    const gint    height   = gdk_pixbuf_get_height (dst);
    const gint    rowstride = gdk_pixbuf_get_rowstride (dst);
    const gint    nchan    = gdk_pixbuf_get_n_channels (dst);
    guchar       *pixels   = gdk_pixbuf_get_pixels (dst);

    for (gint y = 0; y < height; y++)
    {
        guchar *row = pixels + y * rowstride;
        for (gint x = 0; x < width; x++)
        {
            guchar *p = row + x * nchan;
            p[0] = lut[p[0]];
            p[1] = lut[p[1]];
            p[2] = lut[p[2]];
            /* alpha (p[3] when present) left untouched */
        }
    }

    return dst;
}

static void
pn_image_levels_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Levels", levels_transform);
}

static void
pn_image_levels_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnImageLevels *self = PN_IMAGE_LEVELS (object);

    switch (prop_id)
    {
    case PROP_BLACK:
        g_value_set_int (value, self->black);
        break;
    case PROP_WHITE:
        g_value_set_int (value, self->white);
        break;
    case PROP_GAMMA:
        g_value_set_double (value, self->gamma);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_levels_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnImageLevels *self = PN_IMAGE_LEVELS (object);

    switch (prop_id)
    {
    case PROP_BLACK:
        self->black = g_value_get_int (value);
        break;
    case PROP_WHITE:
        self->white = g_value_get_int (value);
        break;
    case PROP_GAMMA:
        self->gamma = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_levels_class_init (PnImageLevelsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_levels_get_property;
    object_class->set_property = pn_image_levels_set_property;

    node_class->receive    = pn_image_levels_receive;
    node_class->class_name = "Levels";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_ADJUST;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_BLACK] = g_param_spec_int (
            "black", "Black",
            "Input black point: values at or below it map to 0",
            0, 254, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WHITE] = g_param_spec_int (
            "white", "White",
            "Input white point: values at or above it map to 255",
            1, 255, 255,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GAMMA] = g_param_spec_double (
            "gamma", "Gamma",
            "Midtone power curve across the remapped span "
            "(below 1 darkens, above 1 lightens)",
            0.1, 5.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_levels_init (PnImageLevels *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->black = 0;
    self->white = 255;
    self->gamma = 1.0;

    pn_node_set_class_name (node, "Levels");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
