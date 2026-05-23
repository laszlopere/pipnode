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

#include "pn-color.h"

#include <stdio.h>
#include <string.h>

PnColor *
pn_color_copy (const PnColor *self)
{
    return self != NULL ? g_memdup2 (self, sizeof *self) : NULL;
}

void
pn_color_free (PnColor *self)
{
    g_free (self);
}

G_DEFINE_BOXED_TYPE (PnColor, pn_color, pn_color_copy, pn_color_free)

gboolean
pn_color_equal (
        const PnColor *a,
        const PnColor *b)
{
    if (a == b)
        return TRUE;
    if (a == NULL || b == NULL)
        return FALSE;
    return a->red   == b->red
        && a->green == b->green
        && a->blue  == b->blue
        && a->alpha == b->alpha;
}

/* Parse one channel token of an rgb()/rgba() body: a 0-255 integer
 * (or a percentage) for r/g/b mapped to 0-1, or a raw 0-1 float for
 * the alpha.  g_ascii_strtod() keeps this locale-independent. */
static double
parse_channel (
        const char *tok,
        gboolean    is_alpha)
{
    double v = g_ascii_strtod (tok, NULL);
    if (is_alpha)
        return v;
    if (strchr (tok, '%') != NULL)
        return v / 100.0;
    return v / 255.0;
}

static gboolean
parse_rgb_func (
        const char *s,
        gboolean    has_alpha,
        PnColor    *out)
{
    const char *open  = strchr (s, '(');
    const char *close = strrchr (s, ')');
    gchar     **parts;
    guint       n;
    gchar      *inner;
    gboolean    ok = FALSE;

    if (open == NULL || close == NULL || close <= open)
        return FALSE;

    inner = g_strndup (open + 1, (gsize) (close - open - 1));
    parts = g_strsplit (inner, ",", -1);
    n     = g_strv_length (parts);

    if ((has_alpha && n == 4) || (!has_alpha && n == 3))
    {
        PnColor c = { 0.0, 0.0, 0.0, 1.0 };
        c.red   = parse_channel (g_strstrip (parts[0]), FALSE);
        c.green = parse_channel (g_strstrip (parts[1]), FALSE);
        c.blue  = parse_channel (g_strstrip (parts[2]), FALSE);
        if (has_alpha)
            c.alpha = parse_channel (g_strstrip (parts[3]), TRUE);
        *out = c;
        ok = TRUE;
    }

    g_strfreev (parts);
    g_free (inner);
    return ok;
}

static gboolean
parse_hex (
        const char *hex,
        PnColor    *out)
{
    gsize    n = strlen (hex);
    guint    r, g, b, a = 255;
    PnColor  c = { 0.0, 0.0, 0.0, 1.0 };

    if (n == 3 && sscanf (hex, "%1x%1x%1x", &r, &g, &b) == 3)
    {
        r *= 17; g *= 17; b *= 17;
    }
    else if (n == 4 && sscanf (hex, "%1x%1x%1x%1x", &r, &g, &b, &a) == 4)
    {
        r *= 17; g *= 17; b *= 17; a *= 17;
    }
    else if (n == 6 && sscanf (hex, "%2x%2x%2x", &r, &g, &b) == 3)
    {
        /* values already 0-255 */
    }
    else if (n == 8 && sscanf (hex, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4)
    {
        /* values already 0-255 */
    }
    else
        return FALSE;

    c.red   = r / 255.0;
    c.green = g / 255.0;
    c.blue  = b / 255.0;
    c.alpha = a / 255.0;
    *out = c;
    return TRUE;
}

gboolean
pn_color_parse (
        PnColor    *self,
        const char *spec)
{
    gchar    *s;
    gboolean  ok = FALSE;

    g_return_val_if_fail (self != NULL, FALSE);

    if (spec == NULL)
        return FALSE;

    s = g_strstrip (g_strdup (spec));

    if (s[0] == '#')
    {
        ok = parse_hex (s + 1, self);
    }
    else if (g_str_has_prefix (s, "rgba"))
    {
        ok = parse_rgb_func (s, TRUE, self);
    }
    else if (g_str_has_prefix (s, "rgb"))
    {
        ok = parse_rgb_func (s, FALSE, self);
    }
    else if (g_strcmp0 (s, "transparent") == 0)
    {
        PnColor c = { 0.0, 0.0, 0.0, 0.0 };
        *self = c;
        ok = TRUE;
    }

    g_free (s);
    return ok;
}

gchar *
pn_color_to_string (const PnColor *self)
{
    int r, g, b;

    if (self == NULL)
        return g_strdup ("");

    r = (int) (0.5 + CLAMP (self->red,   0.0, 1.0) * 255.0);
    g = (int) (0.5 + CLAMP (self->green, 0.0, 1.0) * 255.0);
    b = (int) (0.5 + CLAMP (self->blue,  0.0, 1.0) * 255.0);

    /* Match gdk_rgba_to_string() exactly: opaque colours collapse to the
     * three-component rgb() form, everything else keeps the float alpha
     * formatted locale-independently. */
    if (self->alpha > 0.999)
        return g_strdup_printf ("rgb(%d,%d,%d)", r, g, b);

    {
        gchar alpha[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (alpha, G_ASCII_DTOSTR_BUF_SIZE, "%g",
                         CLAMP (self->alpha, 0.0, 1.0));
        return g_strdup_printf ("rgba(%d,%d,%d,%s)", r, g, b, alpha);
    }
}
