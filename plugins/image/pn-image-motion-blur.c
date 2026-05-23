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

#include "pn-image-motion-blur.h"
#include "pn-image-ops.h"

/* Motion Blur: a diagonal streak blur.  An n×n kernel carrying ones down
 * its main diagonal averages each pixel with its neighbours along the
 * top-left → bottom-right direction, the smear of a camera panned along
 * that line.  `length` sets n (forced odd); 1 is a no-op pass-through. */

struct _PnImageMotionBlur
{
    PnNode parent_instance;

    /* Streak length in pixels (the kernel size). */
    gint length;
};

G_DEFINE_TYPE (PnImageMotionBlur, pn_image_motion_blur, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_LENGTH,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
motion_blur_transform (GdkPixbuf *src, PnNode *node)
{
    gint       n = PN_IMAGE_MOTION_BLUR (node)->length;
    gdouble   *kernel;
    GdkPixbuf *out;
    gint       i;

    if (n < 2)
        return gdk_pixbuf_copy (src);   /* nothing to do */
    if ((n & 1) == 0)
        n += 1;                         /* pn_image_convolve needs odd n */

    kernel = g_malloc0 ((gsize) n * n * sizeof (gdouble));
    for (i = 0; i < n; i++)
        kernel[i * n + i] = 1.0;        /* main diagonal */

    out = pn_image_convolve (src, kernel, n, (gdouble) n, 0.0, FALSE);
    g_free (kernel);
    return out;
}

static void
pn_image_motion_blur_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Motion Blur", motion_blur_transform);
}

static void
pn_image_motion_blur_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
    PnImageMotionBlur *self = PN_IMAGE_MOTION_BLUR (object);

    switch (prop_id)
    {
    case PROP_LENGTH:
        g_value_set_int (value, self->length);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_motion_blur_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
    PnImageMotionBlur *self = PN_IMAGE_MOTION_BLUR (object);

    switch (prop_id)
    {
    case PROP_LENGTH:
        self->length = g_value_get_int (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_motion_blur_class_init (PnImageMotionBlurClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_motion_blur_get_property;
    object_class->set_property = pn_image_motion_blur_set_property;

    node_class->receive    = pn_image_motion_blur_receive;
    node_class->class_name = "Motion Blur";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_BLUR;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_LENGTH] = g_param_spec_int (
            "length", "Length",
            "Diagonal streak length in pixels (the kernel size); "
            "1 disables the blur",
            1, 25, 9,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_motion_blur_init (PnImageMotionBlur *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    self->length = 9;

    pn_node_set_class_name (node, "Motion Blur");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
