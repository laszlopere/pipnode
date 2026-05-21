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

#include "pn-image-blend.h"
#include "pn-image-ops.h"

/* ------------------------------------------------------------------ */
/*  Blend — the plugin's one two-input node                            */
/*                                                                     */
/*  Mixes the images on its two input ports:                           */
/*      out = base·(1 - opacity) + overlay·opacity                     */
/*                                                                     */
/*  The output keeps the geometry of the *bigger* input (greater pixel */
/*  area; a tie keeps input 0).  The smaller image is mixed into the   */
/*  upper-left corner and the bigger image's pixels are copied through */
/*  wherever the smaller one has run out — no scaling.  Colour channels */
/*  only; the bigger image's alpha is preserved, matching every other  */
/*  node in this plugin.                                                */
/* ------------------------------------------------------------------ */

struct _PnImageBlend
{
    PnNode parent_instance;

    /* Mix factor: 0 ⇒ base only, 1 ⇒ overlay only. */
    gdouble opacity;
};

G_DEFINE_TYPE (PnImageBlend, pn_image_blend, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OPACITY,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* Blend's "combine" is simply the overlay channel; pn_image_compose then
 * applies the opacity mix, giving the classic out = base·(1-op) + over·op. */
static guchar
blend_over (guchar base, guchar over)
{
    (void) base;
    return over;
}

static GdkPixbuf *
blend_transform (GdkPixbuf *a, GdkPixbuf *b, PnNode *node)
{
    return pn_image_compose (a, b, PN_IMAGE_BLEND (node)->opacity, blend_over);
}

static void
pn_image_blend_receive (PnNode *node, PnMessage *message)
{
    /* Route the message to its input slot (0 or 1); the driver waits
     * until both ports hold an image, then runs blend_transform(). */
    pn_image_node_process2 (node, message, pn_node_current_input (),
                            "Blend", blend_transform);
}

static void
pn_image_blend_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    PnImageBlend *self = PN_IMAGE_BLEND (object);

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
pn_image_blend_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    PnImageBlend *self = PN_IMAGE_BLEND (object);

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
pn_image_blend_class_init (PnImageBlendClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_blend_get_property;
    object_class->set_property = pn_image_blend_set_property;

    node_class->receive    = pn_image_blend_receive;
    node_class->class_name = "Blend";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_COMPOSITE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_OPACITY] = g_param_spec_double (
            "opacity", "Opacity",
            "Mix factor for the overlay image: 0 keeps the base, 1 shows "
            "only the overlay",
            0.0, 1.0, 0.5,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_blend_init (PnImageBlend *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->opacity = 0.5;

    pn_node_set_class_name (node, "Blend");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_n_inputs   (node, 2);     /* the whole point: two inputs */
    pn_node_set_has_output (node, TRUE);
}
