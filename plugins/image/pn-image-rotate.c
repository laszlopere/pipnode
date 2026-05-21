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

#include "pn-image-rotate.h"
#include "pn-image-ops.h"

/* Rotate: turn the image by an arbitrary `angle` (degrees, counter-
 * clockwise).  The canvas grows so the whole rotated picture fits, the
 * source is sampled with bilinear interpolation, and the new corners
 * that fall outside the original are filled with black (transparent when
 * the image has an alpha channel). */

struct _PnImageRotate
{
    PnNode parent_instance;

    /* Rotation angle in degrees, counter-clockwise. */
    gdouble angle;
};

G_DEFINE_TYPE (PnImageRotate, pn_image_rotate, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_ANGLE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static GdkPixbuf *
rotate_transform (GdkPixbuf *src, PnNode *node)
{
    gdouble ang = PN_IMAGE_ROTATE (node)->angle * G_PI / 180.0;
    gdouble cs = cos (ang), sn = sin (ang);
    gint sw = gdk_pixbuf_get_width (src), sh = gdk_pixbuf_get_height (src);
    gboolean alpha = gdk_pixbuf_get_has_alpha (src);
    gint snch = gdk_pixbuf_get_n_channels (src);
    gint dw = (gint) ceil (fabs (sw * cs) + fabs (sh * sn));
    gint dh = (gint) ceil (fabs (sw * sn) + fabs (sh * cs));
    GdkPixbuf *dst;
    gint srs, drs, dnch;
    const guchar *sp;
    guchar *dp;
    gdouble scx, scy, dcx, dcy;
    gint dy, dx;

    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    dst = gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, dw, dh);
    if (!dst) return NULL;

    srs = gdk_pixbuf_get_rowstride (src);
    drs = gdk_pixbuf_get_rowstride (dst);
    dnch = gdk_pixbuf_get_n_channels (dst);
    sp = gdk_pixbuf_get_pixels (src);
    dp = gdk_pixbuf_get_pixels (dst);
    scx = sw / 2.0; scy = sh / 2.0;
    dcx = dw / 2.0; dcy = dh / 2.0;

    for (dy = 0; dy < dh; dy++)
        for (dx = 0; dx < dw; dx++)
        {
            gdouble rx = dx + 0.5 - dcx, ry = dy + 0.5 - dcy;
            gdouble fx = scx + rx * cs + ry * sn - 0.5;  /* inverse rotation */
            gdouble fy = scy - rx * sn + ry * cs - 0.5;
            guchar *o = dp + (gsize) dy * drs + dx * dnch;
            gint x0 = (gint) floor (fx), y0 = (gint) floor (fy);
            gdouble tx, ty;
            gint x1, y1, cx0, cx1, cy0, cy1, c;

            if (x0 < -1 || x0 > sw - 1 || y0 < -1 || y0 > sh - 1)
            {
                o[0] = o[1] = o[2] = 0;
                if (dnch == 4) o[3] = 0;
                continue;
            }

            tx = fx - x0; ty = fy - y0;
            x1 = x0 + 1; y1 = y0 + 1;
            cx0 = CLAMP (x0, 0, sw - 1); cx1 = CLAMP (x1, 0, sw - 1);
            cy0 = CLAMP (y0, 0, sh - 1); cy1 = CLAMP (y1, 0, sh - 1);

            for (c = 0; c < 3; c++)
            {
                gdouble p00 = sp[(gsize) cy0 * srs + cx0 * snch + c];
                gdouble p10 = sp[(gsize) cy0 * srs + cx1 * snch + c];
                gdouble p01 = sp[(gsize) cy1 * srs + cx0 * snch + c];
                gdouble p11 = sp[(gsize) cy1 * srs + cx1 * snch + c];
                gdouble top = p00 + (p10 - p00) * tx;
                gdouble bot = p01 + (p11 - p01) * tx;
                o[c] = (guchar) (top + (bot - top) * ty + 0.5);
            }

            if (dnch == 4)
            {
                gdouble a00 = sp[(gsize) cy0 * srs + cx0 * snch + 3];
                gdouble a10 = sp[(gsize) cy0 * srs + cx1 * snch + 3];
                gdouble a01 = sp[(gsize) cy1 * srs + cx0 * snch + 3];
                gdouble a11 = sp[(gsize) cy1 * srs + cx1 * snch + 3];
                gdouble top = a00 + (a10 - a00) * tx;
                gdouble bot = a01 + (a11 - a01) * tx;
                o[3] = (guchar) (top + (bot - top) * ty + 0.5);
            }
        }

    return dst;
}

static void
pn_image_rotate_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Rotate", rotate_transform);
}

static void
pn_image_rotate_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnImageRotate *self = PN_IMAGE_ROTATE (object);

    switch (prop_id)
    {
    case PROP_ANGLE:
        g_value_set_double (value, self->angle);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_rotate_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnImageRotate *self = PN_IMAGE_ROTATE (object);

    switch (prop_id)
    {
    case PROP_ANGLE:
        self->angle = g_value_get_double (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_image_rotate_class_init (PnImageRotateClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_image_rotate_get_property;
    object_class->set_property = pn_image_rotate_set_property;

    node_class->receive    = pn_image_rotate_receive;
    node_class->class_name = "Rotate";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (GdkRGBA){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_GEOMETRY;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_ANGLE] = g_param_spec_double (
            "angle", "Angle",
            "Rotation angle in degrees (counter-clockwise); the canvas "
            "grows to fit and new corners are filled with black",
            -180.0, 180.0, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_image_rotate_init (PnImageRotate *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  color = { 0.50, 0.45, 0.70, 1.0 };

    self->angle = 0.0;

    pn_node_set_class_name (node, "Rotate");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
