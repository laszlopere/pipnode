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

#include "pn-image-overlay.h"
#include "pn-image-ops.h"

/* ------------------------------------------------------------------ */
/*  Overlay — two-input "Composite" blend mode                         */
/*                                                                     */
/*  Multiply where the base is dark and Screen where it is light, so    */
/*  the base's own midtones decide the mix: it lifts contrast while     */
/*  keeping shadows and highlights anchored.  The base is the canvas    */
/*  (the bigger image; see pn_image_compose()), which is why this       */
/*  asymmetric mode keys off that image.  pn_image_compose() then mixes */
/*  the result back over the base by `opacity`.                         */
/* ------------------------------------------------------------------ */

struct _PnImageOverlay
{
    PnNode parent_instance;

    /* Mix of the combined result back over the base: 0 ⇒ base only. */
    gdouble opacity;
};

G_DEFINE_TYPE (PnImageOverlay, pn_image_overlay, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OPACITY,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* Overlay: multiply dark base values, screen light ones. */
static guchar
overlay_u8 (guchar base, guchar over)
{
    if (base < 128)
        return (guchar) (2 * (gint) base * over / 255);
    return (guchar) (255 - 2 * (255 - base) * (255 - over) / 255);
}

static GdkPixbuf *
overlay_transform (GdkPixbuf *a, GdkPixbuf *b, PnNode *node)
{
    return pn_image_compose (a, b,
                             PN_IMAGE_OVERLAY (node)->opacity, overlay_u8);
}

static void
pn_image_overlay_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process2 (node, message, pn_node_current_input (),
                            "Overlay", overlay_transform);
}

static void
pn_image_overlay_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
    PnImageOverlay *self = PN_IMAGE_OVERLAY (object);

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
pn_image_overlay_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
    PnImageOverlay *self = PN_IMAGE_OVERLAY (object);

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
pn_image_overlay_class_init (PnImageOverlayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_overlay_get_property;
    object_class->set_property = pn_image_overlay_set_property;

    node_class->receive    = pn_image_overlay_receive;
    node_class->class_name = "Overlay";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
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
pn_image_overlay_init (PnImageOverlay *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->opacity = 1.0;

    pn_node_set_class_name (node, "Overlay");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_n_inputs   (node, 2);     /* two images to combine */
    pn_node_set_has_output (node, TRUE);
}
