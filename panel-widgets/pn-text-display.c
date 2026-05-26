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
/*  PnTextDisplay — a panel-sized text readout                         */
/*                                                                     */
/*  The panel-applet face of the Label node: one or two lines of text  */
/*  in a configurable font and colour on an optional rounded box,       */
/*  fitted to the panel row so it never overflows.  Self-contained      */
/*  cairo + Pango + GTK so the applet never links a pipnode library.    */
/* ------------------------------------------------------------------ */

#include "pn-text-display.h"

#include <math.h>

/* Layout ratios — a restatement of the PN_LABEL_* ratios in pn-label.h
 * (panel-widgets cannot include a pipnode header).  Keep them in sync: the
 * panel widget, the panel-editor preview and the worksheet node are then
 * the same readout drawn at three scales. */
#define TD_PAD_X_RATIO  0.07   /* left/right padding / box height         */
#define TD_PAD_Y_RATIO  0.02   /* top/bottom padding / box height         */
#define TD_CORNER_RATIO 0.08   /* corner radius / box height              */
#define TD_LINE_SPACING 0.78   /* Pango line-spacing factor (tight)       */
/* Fixed widget aspect — PN_LABEL_ASPECT (206/100 = 2.06) padded 20% so the
 * label reads with breathing room.  Width is purely a function of height so
 * the widget never resizes when its text changes; positions stay stable. */
#define TD_ASPECT       2.472

struct _PnTextDisplay
{
    GtkDrawingArea parent_instance;

    gchar   *text;          /* may hold one '\n' for two lines           */
    gchar   *font_family;
    gint     lines;         /* 1 or 2                                     */
    gint     scale;         /* text height, % of its line's share        */
    gint     height;        /* requested overall pixel height            */
    gint     align;         /* 0 left, 1 centre, 2 right                  */
    gint     weight;        /* Pango weight number (400/500/600/700)     */
    gboolean italic;

    gdouble  text_r, text_g, text_b;        /* text colour               */
    gdouble  bg_r, bg_g, bg_b, bg_a;        /* background colour + alpha  */
};

G_DEFINE_TYPE (PnTextDisplay, pn_text_display, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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

static void
apply_font (PnTextDisplay *self, PangoLayout *layout, double font_px)
{
    PangoFontDescription *fd = pango_font_description_new ();

    /* Leave the family unset when none is configured, so the text inherits
     * the panel's default font and matches the clock applet. */
    if (self->font_family != NULL && *self->font_family != '\0')
        pango_font_description_set_family (fd, self->font_family);
    pango_font_description_set_weight (fd, (PangoWeight) self->weight);
    pango_font_description_set_style  (fd, self->italic ? PANGO_STYLE_ITALIC
                                                        : PANGO_STYLE_NORMAL);
    if (font_px < 1.0) font_px = 1.0;
    pango_font_description_set_absolute_size (fd, font_px * PANGO_SCALE);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    /* Tighten the two-line gap (pull baselines closer than natural). */
#if PANGO_VERSION_CHECK(1, 44, 0)
    pango_layout_set_line_spacing (layout, TD_LINE_SPACING);
#endif
}

/* The font size that makes `lines` lines fill self->scale percent of the
 * inner height of a box @widget_h px tall.  Measured rather than guessed:
 * a reference-sized sample of the right line count is laid out (with the
 * tight line spacing applied) and the font scaled by the ratio, so it fills
 * exactly whatever the real font metrics are — and depends only on the
 * height and line count, never the text length. */
static double
fill_font_px (PnTextDisplay *self, GtkWidget *widget, double widget_h)
{
    const double  ref = 100.0;
    double        inner_h = widget_h - 2.0 * (widget_h * TD_PAD_Y_RATIO);
    int           lines   = self->lines > 0 ? self->lines : 1;
    PangoLayout  *m;
    GString      *sample;
    int           i, pw = 0, ph = 0;

    if (inner_h < 1.0) inner_h = 1.0;

    sample = g_string_new (NULL);
    for (i = 0; i < lines; i++)
        g_string_append (sample, i == 0 ? "0" : "\n0");

    m = gtk_widget_create_pango_layout (widget, sample->str);
    apply_font (self, m, ref);
    pango_layout_set_width (m, -1);
    pango_layout_get_pixel_size (m, &pw, &ph);
    g_object_unref (m);
    g_string_free (sample, TRUE);

    if (ph < 1) ph = 1;
    return ref * (inner_h / (double) ph) * (self->scale / 100.0);
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

static gboolean
pn_text_display_draw (GtkWidget *widget, cairo_t *cr)
{
    PnTextDisplay *self = PN_TEXT_DISPLAY (widget);
    GtkAllocation  alloc;
    PangoLayout   *layout;
    double         pad_x, pad_y, aw, ah, font_px, tx, ty;
    int            pw, ph;

    gtk_widget_get_allocation (widget, &alloc);

    /* Optional rounded background — drawn whenever it is not effectively
     * transparent, so the label can read as a badge or float bare on the
     * panel as the user configures. */
    if (self->bg_a > 0.02)
    {
        rounded_rect (cr, 0.0, 0.0, alloc.width, alloc.height,
                      alloc.height * TD_CORNER_RATIO);
        cairo_set_source_rgba (cr, self->bg_r, self->bg_g, self->bg_b,
                               self->bg_a);
        cairo_fill (cr);
    }

    if (self->text == NULL || *self->text == '\0')
        return FALSE;

    pad_x = alloc.height * TD_PAD_X_RATIO;
    pad_y = alloc.height * TD_PAD_Y_RATIO;
    aw    = alloc.width  - 2.0 * pad_x;
    ah    = alloc.height - 2.0 * pad_y;
    if (aw < 1.0 || ah < 1.0)
        return FALSE;

    font_px = fill_font_px (self, widget, alloc.height);

    layout = gtk_widget_create_pango_layout (widget, self->text);
    apply_font (self, layout, font_px);
    pango_layout_set_width (layout, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    /* Align each line within the text block (so a centred two-line label
     * centres its shorter line); the block as a whole is then positioned in
     * the box below.  Setting the width to the natural block width keeps the
     * per-line alignment without wrapping any line. */
    pango_layout_set_alignment (layout,
                                self->align == 0 ? PANGO_ALIGN_LEFT
                                : self->align == 2 ? PANGO_ALIGN_RIGHT
                                : PANGO_ALIGN_CENTER);
    pango_layout_set_width (layout, pw * PANGO_SCALE);

    /* Position the block by alignment when it fits; when it overflows,
     * anchor it to the top-left so the start of each line stays visible and
     * the crop takes the right / bottom edge. */
    tx = pw > aw ? 0.0
       : self->align == 0 ? 0.0
       : self->align == 2 ? (aw - pw)
       :                    (aw - pw) * 0.5;
    ty = ph > ah ? 0.0 : (ah - ph) * 0.5;

    cairo_save (cr);
    cairo_rectangle (cr, pad_x, pad_y, aw, ah);
    cairo_clip (cr);

    cairo_set_source_rgba (cr, self->text_r, self->text_g, self->text_b, 1.0);
    /* Snap the text origin to whole pixels so glyph hinting lands on the
     * pixel grid — fractional positions blur the text. */
    cairo_move_to (cr, floor (pad_x + tx + 0.5), floor (pad_y + ty + 0.5));
    pango_cairo_show_layout (cr, layout);

    cairo_restore (cr);
    g_object_unref (layout);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_text_display_get_preferred_width (GtkWidget *widget,
                                     gint      *minimum,
                                     gint      *natural)
{
    PnTextDisplay *self = PN_TEXT_DISPLAY (widget);
    gint           w = (gint) (self->height * TD_ASPECT);

    if (w < 8) w = 8;
    *minimum = *natural = w;
}

static void
pn_text_display_get_preferred_height (GtkWidget *widget,
                                      gint      *minimum,
                                      gint      *natural)
{
    PnTextDisplay *self = PN_TEXT_DISPLAY (widget);

    *minimum = *natural = self->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_text_display_finalize (GObject *object)
{
    PnTextDisplay *self = PN_TEXT_DISPLAY (object);

    g_free (self->text);
    g_free (self->font_family);

    G_OBJECT_CLASS (pn_text_display_parent_class)->finalize (object);
}

static void
pn_text_display_class_init (PnTextDisplayClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize = pn_text_display_finalize;

    widget_class->draw                 = pn_text_display_draw;
    widget_class->get_preferred_width  = pn_text_display_get_preferred_width;
    widget_class->get_preferred_height = pn_text_display_get_preferred_height;
}

static void
pn_text_display_init (PnTextDisplay *self)
{
    self->text        = NULL;
    self->font_family = g_strdup ("");
    self->lines       = 1;
    self->scale       = 100;
    self->height      = 24;   /* matches the other panel widgets' default */
    self->align       = 1;    /* centre */
    self->weight      = 400;  /* normal */
    self->italic      = FALSE;

    self->text_r = 0.90; self->text_g = 0.92; self->text_b = 0.94;
    self->bg_r = 0.10; self->bg_g = 0.11; self->bg_b = 0.13; self->bg_a = 1.0;

    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_text_display_new (void)
{
    return g_object_new (PN_TYPE_TEXT_DISPLAY, NULL);
}

void
pn_text_display_set_text (PnTextDisplay *self, const gchar *text)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (g_strcmp0 (self->text, text) == 0)
        return;

    g_free (self->text);
    self->text = (text != NULL && *text != '\0') ? g_strdup (text) : NULL;
    /* Width is a fixed function of height, so a text change only repaints —
     * no resize, so the widget stays at its laid-out x and neighbours don't
     * shift when the text arrives late. */
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_lines (PnTextDisplay *self, gint lines)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (lines < 1) lines = 1;
    if (lines > 2) lines = 2;
    if (lines == self->lines)
        return;

    self->lines = lines;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_color (PnTextDisplay *self,
                           gdouble red, gdouble green, gdouble blue)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (red == self->text_r && green == self->text_g && blue == self->text_b)
        return;

    self->text_r = red;
    self->text_g = green;
    self->text_b = blue;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_background (PnTextDisplay *self,
                               gdouble red, gdouble green,
                               gdouble blue, gdouble alpha)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (red == self->bg_r && green == self->bg_g && blue == self->bg_b
        && alpha == self->bg_a)
        return;

    self->bg_r = red;
    self->bg_g = green;
    self->bg_b = blue;
    self->bg_a = alpha;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_font (PnTextDisplay *self,
                          const gchar   *family,
                          gint           scale,
                          gint           weight,
                          gboolean       italic)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (scale < 1) scale = 1;

    if (g_strcmp0 (self->font_family, family) == 0
        && self->scale == scale
        && self->weight == weight && self->italic == italic)
        return;

    g_free (self->font_family);
    self->font_family = g_strdup (family != NULL ? family : "");
    self->scale  = scale;
    self->weight = weight;
    self->italic = italic;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_align (PnTextDisplay *self, gint align)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (align < 0) align = 0;
    if (align > 2) align = 2;
    if (align == self->align)
        return;

    self->align = align;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_text_display_set_height (PnTextDisplay *self, gint height)
{
    g_return_if_fail (PN_IS_TEXT_DISPLAY (self));

    if (height < 8)
        height = 8;
    if (height == self->height)
        return;

    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}
