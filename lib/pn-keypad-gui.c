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

/* ------------------------------------------------------------------ */
/*  PnKeypad — gui tier.                                               */
/*                                                                     */
/*  The cairo painter for the Keypad's client area: a dark case, the   */
/*  layout's rounded keys, their legends and — on the phone pad — the  */
/*  letter groups under them.  Every key's                             */
/*  rectangle comes from the core's pn_keypad_key_rect_in(), the same  */
/*  call the hit-test uses, so the key the user sees under the cursor  */
/*  is always the key the press resolves to.                           */
/*                                                                     */
/*  The headless runtime never loads this file's half, so the keypad's */
/*  layout, hit-test and emission run without GTK.                     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-keypad-gui.h"
#include "pn-keypad.h"

#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>

/* Case corner radius, key corner radius, and the legend's font size as
 * a fraction of the key height — so the legends scale with the node
 * rather than being pinned to the at-rest size. */
#define PN_KEYPAD_CASE_RADIUS   6.0
#define PN_KEYPAD_KEY_RADIUS    4.0
#define PN_KEYPAD_FONT_FRACTION 0.46
#define PN_KEYPAD_FONT_MAX_PX   20.0

/* The phone pad's letter group under the digit: a fraction of the
 * legend's own size, and the share of the key height the two lines
 * together are centred in. */
#define PN_KEYPAD_SUB_FRACTION  0.52
#define PN_KEYPAD_SUB_MIN_PX     6.0
#define PN_KEYPAD_SUB_FADE      0.40

static void
rounded_rect (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   r)
{
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -G_PI_2, 0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0,        G_PI_2);
    cairo_arc (cr, x + r,     y + h - r, r,  G_PI_2,  G_PI);
    cairo_arc (cr, x + r,     y + r,     r,  G_PI,    1.5 * G_PI);
    cairo_close_path (cr);
}

/** Blend @c toward white (@t > 0) or black (@t < 0) by |@t|.  Used for
 *  the key's top highlight and its pressed state, so a re-coloured
 *  keypad keeps a consistent relief without four more properties. */
static void
set_shaded (cairo_t *cr, const PnColor *c, double t)
{
    double target = (t >= 0.0) ? 1.0 : 0.0;
    double k      = fabs (t);

    cairo_set_source_rgba (cr,
                           c->red   + (target - c->red)   * k,
                           c->green + (target - c->green) * k,
                           c->blue  + (target - c->blue)  * k,
                           c->alpha);
}

/** Lay @text out at @font_px in @weight, returning its pixel size. */
static PangoLayout *
make_layout (
        cairo_t     *cr,
        const gchar *text,
        const gchar *font,
        double       font_px,
        int         *out_w,
        int         *out_h)
{
    PangoLayout          *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *desc   = pango_font_description_from_string (font);

    pango_font_description_set_absolute_size (desc, font_px * PANGO_SCALE);
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);

    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, out_w, out_h);

    return layout;
}

/** Paint one key's legend, centred in (@x, @y, @w, @h), with @sublabel
 *  — the phone pad's letter group — on a smaller second line beneath
 *  it when the key has one.  Both lines are centred as a block, so a
 *  lettered key and a bare one still read as the same row of keys. */
static void
draw_legend (
        cairo_t       *cr,
        const gchar   *label,
        const gchar   *sublabel,
        double         x,
        double         y,
        double         w,
        double         h,
        const PnColor *color,
        const PnColor *face)
{
    PangoLayout *layout;
    double       font_px = h * PN_KEYPAD_FONT_FRACTION;
    double       sub_px;
    int          pw, ph, sw = 0, sh = 0;
    double       block_h, top;

    if (font_px > PN_KEYPAD_FONT_MAX_PX) font_px = PN_KEYPAD_FONT_MAX_PX;
    if (font_px < 6.0)                   font_px = 6.0;

    layout = make_layout (cr, label, "Sans Bold", font_px, &pw, &ph);

    if (sublabel == NULL)
    {
        cairo_set_source_rgba (cr, color->red, color->green,
                                   color->blue, color->alpha);
        cairo_move_to (cr, x + (w - pw) / 2.0, y + (h - ph) / 2.0);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
        return;
    }

    sub_px = font_px * PN_KEYPAD_SUB_FRACTION;
    if (sub_px < PN_KEYPAD_SUB_MIN_PX) sub_px = PN_KEYPAD_SUB_MIN_PX;

    {
        PangoLayout *sub = make_layout (cr, sublabel, "Sans", sub_px,
                                        &sw, &sh);

        block_h = ph + sh;
        top     = y + (h - block_h) / 2.0;

        cairo_set_source_rgba (cr, color->red, color->green,
                                   color->blue, color->alpha);
        cairo_move_to (cr, x + (w - pw) / 2.0, top);
        pango_cairo_show_layout (cr, layout);

        /* The letters sit back a little — blended part of the way to
         * the key's own face — so the digit stays the thing you read
         * first, whatever colours the node has been given. */
        cairo_set_source_rgba (
                cr,
                color->red   + (face->red   - color->red)   * PN_KEYPAD_SUB_FADE,
                color->green + (face->green - color->green) * PN_KEYPAD_SUB_FADE,
                color->blue  + (face->blue  - color->blue)  * PN_KEYPAD_SUB_FADE,
                color->alpha);
        cairo_move_to (cr, x + (w - sw) / 2.0, top + ph);
        pango_cairo_show_layout (cr, sub);

        g_object_unref (sub);
    }

    g_object_unref (layout);
}

static void
pn_keypad_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnKeypad          *self = PN_KEYPAD (node);
    PnKeypadPaintState st;
    const PnKeypadKey *keys;
    guint              n_keys = 0;
    guint              i;

    pn_keypad_get_paint_state (self, &st);
    keys = pn_keypad_layout_get_keys (st.layout, &n_keys);

    cairo_save (cr);

    /* The case. */
    rounded_rect (cr, x, y, w, h, PN_KEYPAD_CASE_RADIUS);
    cairo_set_source_rgba (cr,
                           st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill_preserve (cr);
    set_shaded (cr, &st.background_color, 0.25);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* The keys.  Each layout marks the ones it picks out — the
     * operators and "=", "*" and "#", the hex letters — and those take
     * the accent colour, everything else the plain key colour. */
    for (i = 0; i < n_keys; i++)
    {
        const PnKeypadKey *k = &keys[i];
        const PnColor     *face;
        const gboolean     pressed = (st.pressed_index == (gint) i);
        double             kx, ky, kw, kh;

        if (!pn_keypad_key_rect_in (st.layout, x, y, w, h, i,
                                    &kx, &ky, &kw, &kh))
            continue;

        face = k->accent ? &st.accent_color : &st.key_color;

        /* A pressed key darkens and loses its highlight, so the click
         * reads as the key going down into the case. */
        rounded_rect (cr, kx, ky, kw, kh, PN_KEYPAD_KEY_RADIUS);
        set_shaded (cr, face, pressed ? -0.25 : 0.0);
        cairo_fill (cr);

        if (!pressed)
        {
            /* Top-edge highlight: a thin lighter band along the upper
             * half, clipped to the key's rounded silhouette. */
            cairo_save (cr);
            rounded_rect (cr, kx, ky, kw, kh, PN_KEYPAD_KEY_RADIUS);
            cairo_clip (cr);
            cairo_rectangle (cr, kx, ky, kw, kh * 0.45);
            set_shaded (cr, face, 0.12);
            cairo_fill (cr);
            cairo_restore (cr);
        }

        /* Outline, so adjacent keys stay separate on a dark case. */
        rounded_rect (cr, kx, ky, kw, kh, PN_KEYPAD_KEY_RADIUS);
        set_shaded (cr, face, -0.35);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);

        draw_legend (cr, k->label, k->sublabel, kx, ky, kw, kh,
                     &st.text_color, face);
    }

    cairo_restore (cr);
}

void
pn_keypad_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_KEYPAD));

    node_class->paint_plot = pn_keypad_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
