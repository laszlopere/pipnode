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

#ifndef PN_COLOR_H
#define PN_COLOR_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnColor                                                            */
/*                                                                     */
/*  A GTK-free RGBA colour for the headless core.  Deliberately        */
/*  byte-identical in layout to #GdkRGBA — four doubles in the same    */
/*  order with the same member names — so the GUI tier can cast        */
/*  `PnColor* <-> GdkRGBA*` for free at its cairo/gdk drawing boundary  */
/*  while the core never links GDK.  Existed before only as the GdkRGBA */
/*  `color` slot on #PnNodeClass; lifting it out lets pn-node.h compile */
/*  against gobject alone.                                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_COLOR (pn_color_get_type ())

typedef struct _PnColor PnColor;

struct _PnColor
{
    double red;
    double green;
    double blue;
    double alpha;
};

GType    pn_color_get_type  (void) G_GNUC_CONST;
PnColor *pn_color_copy      (const PnColor *self);
void     pn_color_free      (PnColor *self);

/**
 * pn_color_equal:
 * @a: (nullable): a colour
 * @b: (nullable): a colour
 *
 * Exact component-wise equality, matching gdk_rgba_equal() (no epsilon).
 * Two %NULL pointers compare equal; a %NULL against a non-%NULL does not.
 */
gboolean pn_color_equal     (const PnColor *a, const PnColor *b);

/**
 * pn_color_parse:
 * @self: (out): filled in on success, untouched on failure
 * @spec: a colour string
 *
 * Parses the colour-string forms the worksheet save format and the
 * preferences actually use — the output of gdk_rgba_to_string()
 * (`rgb(r,g,b)` / `rgba(r,g,b,a)` with 0-255 integer components and a
 * 0-1 float alpha) plus `#rgb` / `#rgba` / `#rrggbb` / `#rrggbbaa` hex
 * and the keyword `transparent`.  Locale-independent (uses
 * g_ascii_strtod()).  Returns %TRUE and writes @self on success, %FALSE
 * (leaving @self untouched) when the string is not recognised.
 */
gboolean pn_color_parse     (PnColor *self, const char *spec);

/**
 * pn_color_to_string:
 * @self: a colour
 *
 * Serialises @self in exactly the format gdk_rgba_to_string() produces —
 * `rgb(r,g,b)` when fully opaque, `rgba(r,g,b,a)` otherwise — so saved
 * worksheets round-trip byte-for-byte across the GdkRGBA→PnColor split.
 * Returns a newly-allocated string; free with g_free().
 */
gchar   *pn_color_to_string (const PnColor *self);

G_END_DECLS

#endif /* PN_COLOR_H */
