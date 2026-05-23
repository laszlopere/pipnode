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

#include "pn-image-crosshatch.h"
#include "pn-image-ops.h"

/* Crosshatch: a pen-and-ink look.  Each pixel starts white and is
 * darkened to black where its luma is low enough for one of four
 * diagonal hatch line sets to fall on its position — denser hatching in
 * the darker tones.  Alpha is preserved. */

struct _PnImageCrosshatch
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnImageCrosshatch, pn_image_crosshatch, PN_TYPE_NODE)

static void
crosshatch_point_xy (guchar *p, gint x, gint y, gint w, gint h, PnNode *node)
{
    gdouble L = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    guchar  v = 255;  /* start white */

    (void) w;
    (void) h;
    (void) node;

    if (L < 170.0 && ((((x + y) % 8) + 8) % 8 == 0)) v = 0;
    if (L < 140.0 && ((((x - y) % 8) + 8) % 8 == 0)) v = 0;
    if (L < 100.0 && ((((x + y) % 4) + 4) % 4 == 0)) v = 0;
    if (L < 60.0  && ((((x - y) % 4) + 4) % 4 == 0)) v = 0;

    p[0] = p[1] = p[2] = v;
}

static GdkPixbuf *
crosshatch_transform (GdkPixbuf *src, PnNode *node)
{
    return pn_image_map_point_xy (src, crosshatch_point_xy, node);
}

static void
pn_image_crosshatch_receive (PnNode *node, PnMessage *message)
{
    pn_image_node_process (node, message, "Crosshatch", crosshatch_transform);
}

static void
pn_image_crosshatch_class_init (PnImageCrosshatchClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_image_crosshatch_receive;
    node_class->class_name = "Crosshatch";
    node_class->icon       = "\xef\x83\x90";  /* fa-magic U+F0D0 */
    node_class->color      = (PnColor){ 0.50, 0.45, 0.70, 1.0 };
    node_class->category   = PN_IMAGE_CATEGORY_STYLIZE;
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_image_crosshatch_init (PnImageCrosshatch *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.50, 0.45, 0.70, 1.0 };

    pn_node_set_class_name (node, "Crosshatch");
    pn_node_set_icon       (node, "\xef\x83\x90");
    pn_node_set_color      (node, &color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}
