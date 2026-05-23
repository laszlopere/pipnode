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

#include "pn-image-color-burn.h"
#include "pn-image-ops.h"

/* ------------------------------------------------------------------ */
/*  Color Burn — two-input "Composite" blend mode                      */
/*                                                                     */
/*  Per channel the combine darkens the base in proportion to the       */
/*  overlay (255 - (255-base)·255 / over, clamped), which deepens        */
/*  shadows, then pn_image_compose() mixes that back over the base by    */
/*  `opacity`.  Geometry, overlap and alpha follow the shared compose()  */
/*  rules: the bigger image is the canvas, the smaller is pasted         */
/*  top-left.                                                            */
/* ------------------------------------------------------------------ */

struct _PnImageColorBurn
{
    PnNode parent_instance;

    /* Mix of the combined result back over the base: 0 ⇒ base only. */
    gdouble opacity;
};

G_DEFINE_TYPE (PnImageColorBurn, pn_image_color_burn, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OPACITY,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* Color Burn: 255 - (255-base)·255 / over, clamped — darkens. */
static guchar
color_burn_u8 (guchar base, guchar over)
{
    if (over <= 0)
        return 0;
    {
        gint r = 255 - (255 - base) * 255 / over;
        return (guchar) (r < 0 ? 0 : r);
    }
}

static GdkPixbuf *
color_burn_transform (GdkPixbuf *a, GdkPixbuf *b, PnNode *node)
{
    return pn_image_compose (a, b,
                             PN_IMAGE_COLOR_BURN (node)->opacity, color_burn_u8);
}

static void
pn_image_color_burn_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process2 (node, message, pn_node_current_input (),
                            "Color Burn", color_burn_transform);
}

static void
pn_image_color_burn_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    PnImageColorBurn *self = PN_IMAGE_COLOR_BURN (object);

    switch (prop_id)
    {
    case PROP_OPACITY:
        g_value_set_double (value, self->opacity);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_color_burn_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    PnImageColorBurn *self = PN_IMAGE_COLOR_BURN (object);

    switch (prop_id)
    {
    case PROP_OPACITY:
        self->opacity = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_color_burn_class_init (PnImageColorBurnClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_color_burn_get_property;
    object_class->set_property = pn_image_color_burn_set_property;

    node_class->receive    = pn_image_color_burn_receive;
    node_class->class_name = "Color Burn";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COMPOSITE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_OPACITY] = g_param_spec_double (
            "opacity", "Opacity",
            "Mix of the blended result back over the base image: 0 keeps "
            "the base, 1 shows the effect at full strength",
            0.0, 1.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_color_burn_init (PnImageColorBurn *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->opacity = 1.0;

    pn_node_set_class_name (node, "Color Burn");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_n_inputs   (node, 2);     /* two images to combine */
    pn_node_set_has_output (node, TRUE);
}
