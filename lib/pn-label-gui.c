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
/*  PnLabel — gui tier.                                                */
/*                                                                     */
/*  The cairo/Pango drawing for the Label node — a fixed clock-sized    */
/*  box with the output's last one or two lines fitted inside.  The     */
/*  text is shrunk from the configured size down to a minimum font and  */
/*  then clipped, so a long readout is cropped rather than rendered     */
/*  illegibly small or allowed to spill out.  The node's GType,         */
/*  properties, receive() and the tail computation live in the GTK-free */
/*  core file pn-label.c; this file installs the paint_plot vfunc slot   */
/*  onto that class at editor startup (pn_label_gui_install), reading    */
/*  the drawing state through the core's GTK-free snapshot accessor      */
/*  pn_label_get_paint_state().  The headless runtime never loads this   */
/*  half, so the Label logic runs without GTK.                          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-label-gui.h"
#include "pn-label.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Trace a rounded-rectangle path with corner radius @r. */
static void
rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -M_PI_2, 0.0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0.0,     M_PI_2);
    cairo_arc (cr, x + r,     y + h - r, r, M_PI_2,  M_PI);
    cairo_arc (cr, x + r,     y + r,     r, M_PI,    1.5 * M_PI);
    cairo_close_path (cr);
}

/* The desktop UI font family (gtk-font-name) — the same font the panel and
 * its applets use.  The worksheet draws into a bare cairo layout whose
 * default font is fontconfig's, not the desktop's, so we resolve and apply
 * this explicitly to match the panel widget.  Cached for the process. */
static const gchar *
label_default_family (void)
{
    static gchar *family = NULL;

    if (family == NULL)
    {
        GtkSettings *settings = gtk_settings_get_default ();
        gchar       *name     = NULL;

        if (settings != NULL)
            g_object_get (settings, "gtk-font-name", &name, NULL);

        if (name != NULL && *name != '\0')
        {
            PangoFontDescription *d = pango_font_description_from_string (name);
            const gchar          *f = pango_font_description_get_family (d);

            family = g_strdup ((f != NULL && *f != '\0') ? f : "Sans");
            pango_font_description_free (d);
        }
        else
        {
            family = g_strdup ("Sans");
        }
        g_free (name);
    }
    return family;
}

/* Configure @layout's font from the paint state at an absolute pixel
 * size, so the worksheet's zoom does not change the glyph metrics out
 * from under the fit loop. */
static void
label_setup_layout (PangoLayout              *layout,
                    const PnLabelPaintState  *st,
                    double                    font_px)
{
    PangoFontDescription *fd = pango_font_description_new ();

    /* Use the configured family, else the desktop UI font so the worksheet
     * matches the panel applet rather than cairo's fontconfig default. */
    pango_font_description_set_family (
            fd, (st->font_family != NULL && *st->font_family != '\0')
                ? st->font_family : label_default_family ());
    pango_font_description_set_weight (fd, (PangoWeight) st->weight);
    pango_font_description_set_style  (fd, st->italic ? PANGO_STYLE_ITALIC
                                                      : PANGO_STYLE_NORMAL);
    if (font_px < 1.0) font_px = 1.0;
    pango_font_description_set_absolute_size (fd, font_px * PANGO_SCALE);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    /* Tighten the two-line gap: pull baselines closer than the font's
     * natural leading so the lines pack like a panel clock's. */
#if PANGO_VERSION_CHECK(1, 44, 0)
    pango_layout_set_line_spacing (layout, PN_LABEL_LINE_SPACING);
#endif
}

/* The font size that makes `lines` lines fill font-scale percent of
 * @inner_h — measured against a reference-sized sample (with the tight line
 * spacing applied) so it fills exactly whatever the font metrics are, and
 * depends only on the height and line count, never the text length. */
static double
label_fill_font (cairo_t *cr, const PnLabelPaintState *st, double inner_h)
{
    const double  ref = 100.0;
    PangoLayout  *m   = pango_cairo_create_layout (cr);
    GString      *sample = g_string_new (NULL);
    int           i, lines = st->lines > 0 ? st->lines : 1, pw = 0, ph = 0;

    for (i = 0; i < lines; i++)
        g_string_append (sample, i == 0 ? "0" : "\n0");

    label_setup_layout (m, st, ref);
    pango_layout_set_text (m, sample->str, -1);
    pango_layout_set_width (m, -1);
    pango_layout_get_pixel_size (m, &pw, &ph);
    g_object_unref (m);
    g_string_free (sample, TRUE);

    if (ph < 1) ph = 1;
    return ref * (inner_h / (double) ph) * (st->font_scale / 100.0);
}

/* ------------------------------------------------------------------ */
/*  Painter                                                            */
/* ------------------------------------------------------------------ */

static void
pn_label_paint_plot (PnNode  *node,
                     cairo_t *cr,
                     double   x,
                     double   y,
                     double   w,
                     double   h)
{
    PnLabel           *self = PN_LABEL (node);
    PnLabelPaintState  st;
    PangoLayout       *layout;
    double             pad_x, pad_y, ix, iy, iw, ih;
    double             font_px;
    double             ox, oy;
    int                pw, ph;

    pn_label_get_paint_state (self, &st);

    /* ---- the box: a rounded panel filled with the background colour.  The
     * corner radius, padding and font are all fractions of the box height,
     * so this is the same readout the panel widget draws, just scaled. ---- */
    rounded_rect (cr, x, y, w, h, h * PN_LABEL_CORNER_RATIO);
    cairo_set_source_rgba (cr, st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill (cr);

    pad_x = h * PN_LABEL_PAD_X_RATIO;
    pad_y = h * PN_LABEL_PAD_Y_RATIO;
    ix = x + pad_x;
    iy = y + pad_y;
    iw = w - 2.0 * pad_x;
    ih = h - 2.0 * pad_y;
    if (iw < 1.0 || ih < 1.0)
        return;
    if (st.text == NULL || *st.text == '\0')
        return;   /* blank — nothing to draw */

    layout = pango_cairo_create_layout (cr);
    pango_layout_set_text (layout, st.text, -1);

    /* The font fills font-scale percent of the inner height — measured so
     * the lines pack tight and fill the box, the same fraction of the client
     * area in every rendering.  It is not shrunk for long text; overflow is
     * cropped (anchored top-left). */
    font_px = label_fill_font (cr, &st, ih);
    label_setup_layout (layout, &st, font_px);
    pango_layout_set_width (layout, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    /* Align each line within the text block (a centred two-line label
     * centres its shorter line); the block is positioned in the box below.
     * Width = natural block width keeps per-line alignment without wrap. */
    pango_layout_set_alignment (layout,
                                st.align == PN_LABEL_ALIGN_LEFT  ? PANGO_ALIGN_LEFT
                                : st.align == PN_LABEL_ALIGN_RIGHT ? PANGO_ALIGN_RIGHT
                                : PANGO_ALIGN_CENTER);
    pango_layout_set_width (layout, pw * PANGO_SCALE);

    /* Position the text block by alignment when it fits; when it overflows,
     * anchor it to the top-left so the start of each line stays visible and
     * the crop below takes the right / bottom edge. */
    ox = pw > iw ? 0.0
       : st.align == PN_LABEL_ALIGN_LEFT  ? 0.0
       : st.align == PN_LABEL_ALIGN_RIGHT ? (iw - pw)
       :                                    (iw - pw) * 0.5;
    oy = ph > ih ? 0.0 : (ih - ph) * 0.5;

    cairo_save (cr);
    cairo_rectangle (cr, ix, iy, iw, ih);
    cairo_clip (cr);

    cairo_set_source_rgba (cr, st.text_color.red, st.text_color.green,
                           st.text_color.blue, st.text_color.alpha);
    /* Snap the text origin to whole pixels so glyph hinting lands on the
     * pixel grid — fractional positions blur the text. */
    cairo_move_to (cr, floor (ix + ox + 0.5), floor (iy + oy + 0.5));
    pango_cairo_show_layout (cr, layout);

    cairo_restore (cr);
    g_object_unref (layout);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_label_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_LABEL));

    node_class->paint_plot = pn_label_paint_plot;
    /* The footprint is fixed (the core get_size), so the gui tier only adds
     * the painter.  The box has rounded corners (skip the rectangular
     * shadow), and the at-rest readout is already legible (a press selects
     * / drags rather than lifting a zoomed copy). */
    node_class->paint_plot_skip_shadow = TRUE;
    node_class->paint_plot_skip_zoom   = TRUE;

    /* The class ref is intentionally held for the process lifetime so the
     * slots we just wrote stay valid (one leaked ref on a singleton
     * class, mirroring the other gui-tier installers). */
}
