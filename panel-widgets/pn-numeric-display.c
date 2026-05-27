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
/*  PnNumericDisplay — a panel-sized seven-segment numeric readout    */
/*                                                                     */
/*  A miniature of the Numeric node's client area: the same hexagonal- */
/*  bar geometry, sign cell, leading-blank integer padding and         */
/*  decimal-point dot, sized to a panel row.  The bezel/face that the  */
/*  worksheet painter draws is dropped — the digits sit on the panel's */
/*  transparent allocation directly, the same trick PnLedDisplay uses  */
/*  for the Countdown mirror.  Self-contained cairo + GTK so the       */
/*  applet never links a pipnode library.                              */
/* ------------------------------------------------------------------ */

#include "pn-numeric-display.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Seven-segment geometry (matches pn-numeric-gui.c)                  */
/* ------------------------------------------------------------------ */

static const guint8 seg_masks[10] = {
    0x3F, /* 0 */ 0x06, /* 1 */ 0x5B, /* 2 */ 0x4F, /* 3 */ 0x66, /* 4 */
    0x6D, /* 5 */ 0x7D, /* 6 */ 0x07, /* 7 */ 0x7F, /* 8 */ 0x6F, /* 9 */
};

#define SEG_MINUS  0x40        /* segment g only — the centre dash      */

/* Layout ratios, all expressed against the digit cell width D.  Identical
 * to pn-numeric-gui.c so the panel widget looks like a scaled-down twin
 * of the worksheet node. */
#define ND_H_RATIO       1.70   /* digit height / D                      */
#define ND_THICK_RATIO   0.18   /* bar thickness / D                     */
#define ND_INTRA_RATIO   0.14   /* gap between adjacent cells            */
#define ND_DP_RATIO      0.34   /* width of the decimal-point gap        */

/* Default LED colours, matching the Numeric node's defaults: classic LED
 * red on the panel's transparent background, dim red unlit ghosts. */
#define ND_LIT_R   0.92
#define ND_LIT_G   0.12
#define ND_LIT_B   0.08
#define ND_DIM     0.16   /* unlit segment = lit colour * ND_DIM */

#define ND_DEFAULT_DIGITS          6
#define ND_DEFAULT_DECIMAL_PLACES  2

struct _PnNumericDisplay
{
    GtkDrawingArea parent_instance;

    gdouble  value;       /* the value being shown                       */
    gboolean has_value;   /* FALSE = blank screen (off-state ghosts only) */
    guint    digits;      /* integer digit cells (1..12)                 */
    guint    places;      /* fractional cells (0..6)                     */
    gint     height;      /* requested overall pixel height              */

    gdouble lit_r,   lit_g,   lit_b,   lit_a;
    gdouble unlit_r, unlit_g, unlit_b, unlit_a;
};

G_DEFINE_TYPE (PnNumericDisplay, pn_numeric_display, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Bar drawing                                                        */
/* ------------------------------------------------------------------ */

static void
h_seg (cairo_t *cr, double x1, double x2, double cy, double t)
{
    double h = t * 0.5;
    cairo_new_sub_path (cr);
    cairo_move_to (cr, x1,     cy);
    cairo_line_to (cr, x1 + h, cy - h);
    cairo_line_to (cr, x2 - h, cy - h);
    cairo_line_to (cr, x2,     cy);
    cairo_line_to (cr, x2 - h, cy + h);
    cairo_line_to (cr, x1 + h, cy + h);
    cairo_close_path (cr);
}

static void
v_seg (cairo_t *cr, double cx, double y1, double y2, double t)
{
    double h = t * 0.5;
    cairo_new_sub_path (cr);
    cairo_move_to (cr, cx,     y1);
    cairo_line_to (cr, cx + h, y1 + h);
    cairo_line_to (cr, cx + h, y2 - h);
    cairo_line_to (cr, cx,     y2);
    cairo_line_to (cr, cx - h, y2 - h);
    cairo_line_to (cr, cx - h, y1 + h);
    cairo_close_path (cr);
}

/* Fill the current path: lit bars solid in the lit colour + a faint
 * translucent halo; unlit bars in the off-state ghost colour. */
static void
seg_fill (PnNumericDisplay *self, cairo_t *cr, gboolean on)
{
    if (on)
    {
        cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b,
                               self->lit_a);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b,
                               self->lit_a * 0.30);
        cairo_set_line_width (cr, 1.5);
        cairo_stroke (cr);
    }
    else
    {
        cairo_set_source_rgba (cr, self->unlit_r, self->unlit_g,
                               self->unlit_b, self->unlit_a);
        cairo_fill (cr);
    }
}

/* Draw one seven-segment cell at (x,y) of size (D,H).  @mask is a bitmask
 * of segments to light (0 = blank cell). */
static void
draw_cell (PnNumericDisplay *self, cairo_t *cr,
           double x, double y, double D, double H, double t, guint mask)
{
    double inset = t * 0.6;
    double xl    = x + inset;
    double xr    = x + D - inset;
    double yt    = y + inset;
    double yb    = y + H - inset;
    double ym    = y + H * 0.5;
    double g     = t * 0.35;

    h_seg (cr, xl + g, xr - g, yt, t); seg_fill (self, cr, (mask & 0x01) != 0);
    v_seg (cr, xr, yt + g, ym - g, t); seg_fill (self, cr, (mask & 0x02) != 0);
    v_seg (cr, xr, ym + g, yb - g, t); seg_fill (self, cr, (mask & 0x04) != 0);
    h_seg (cr, xl + g, xr - g, yb, t); seg_fill (self, cr, (mask & 0x08) != 0);
    v_seg (cr, xl, ym + g, yb - g, t); seg_fill (self, cr, (mask & 0x10) != 0);
    v_seg (cr, xl, yt + g, ym - g, t); seg_fill (self, cr, (mask & 0x20) != 0);
    h_seg (cr, xl + g, xr - g, ym, t); seg_fill (self, cr, (mask & 0x40) != 0);
}

/* Draw the decimal-point dot to the right of the cell whose right edge
 * is at @x_right, at the cell baseline. */
static void
draw_dot (PnNumericDisplay *self, cairo_t *cr,
          double x_right, double y, double H, double t)
{
    double r  = t * 0.55;
    double cx = x_right + t * 0.6;
    double cy = y + H - t * 0.6;

    cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b,
                           self->lit_a);
    cairo_arc (cr, cx, cy, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
}

/* ------------------------------------------------------------------ */
/*  Value rendering (mirror of render_value in pn-numeric-gui.c)        */
/* ------------------------------------------------------------------ */

/* Largest integer @digits seven-segment cells can show (10^digits - 1). */
static gint64
max_for_digits (guint digits)
{
    gint64 cap = 1;
    guint  i;
    for (i = 0; i < digits; i++) cap *= 10;
    return cap - 1;
}

/* Render @value into @int_d (int_n cells, most-significant first; -1 =
 * leading blank) and @frac_d (frac_n cells, left to right).  Returns
 * FALSE when the rounded integer part overflows @int_n cells. */
static gboolean
render_value (gdouble  value, guint int_n, guint frac_n,
              gint    *int_d, gint *frac_d, gboolean *neg_out)
{
    gboolean neg = (value < 0.0);
    gdouble  av  = fabs (value);
    gdouble  scale;
    gint64   scaled, int_part, max_int;
    guint    i;

    *neg_out = neg;

    scale  = pow (10.0, (double) frac_n);
    scaled = (gint64) floor (av * scale + 0.5);

    {
        gint64 div = 1;
        for (i = 0; i < frac_n; i++) div *= 10;
        int_part = scaled / div;

        if (frac_n > 0)
        {
            gint64 r = scaled % div;
            gint64 dv = div / 10;
            for (i = 0; i < frac_n; i++)
            {
                frac_d[i] = (gint) ((r / dv) % 10);
                dv /= 10;
            }
        }
    }

    max_int = max_for_digits (int_n);
    if (int_part > max_int)
        return FALSE;

    {
        gint64   div     = max_for_digits (int_n - 1) + 1;
        gboolean started = FALSE;

        for (i = 0; i < int_n; i++)
        {
            gint d = (gint) ((int_part / div) % 10);
            if (d != 0) started = TRUE;
            if (!started && i < int_n - 1)
                int_d[i] = -1;
            else
                int_d[i] = d;
            div /= 10;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Sizing                                                             */
/* ------------------------------------------------------------------ */

/* Clamped layout used for both sizing and drawing. */
static void
clamped_layout (PnNumericDisplay *self, guint *out_int_n, guint *out_frac_n)
{
    guint int_n  = self->digits;
    guint frac_n = self->places;
    if (int_n  < 1)  int_n  = 1;
    if (int_n  > 12) int_n  = 12;
    if (frac_n > 6)  frac_n = 6;
    *out_int_n  = int_n;
    *out_frac_n = frac_n;
}

/* Total content width at digit width D — the strip the digits span. */
static double
content_width (PnNumericDisplay *self, double D)
{
    guint    int_n, frac_n;
    gboolean show_dp;
    guint    n;

    clamped_layout (self, &int_n, &frac_n);
    show_dp = (frac_n > 0);
    n       = 1 + int_n + frac_n;

    return (double) n * D
         + (double) n * (D * ND_INTRA_RATIO)
         + (show_dp ? D * ND_DP_RATIO : 0.0);
}

/* Solve the digit width D that fits the available height (and width when
 * one is given). */
static double
digit_width_fit (PnNumericDisplay *self, double avail_w, double avail_h)
{
    double by_height = avail_h / ND_H_RATIO;
    double D         = by_height;

    if (avail_w > 0.0)
    {
        guint    int_n, frac_n;
        gboolean show_dp;
        guint    n;
        double   by_width;

        clamped_layout (self, &int_n, &frac_n);
        show_dp = (frac_n > 0);
        n       = 1 + int_n + frac_n;
        by_width = avail_w
                / ((double) n * (1.0 + ND_INTRA_RATIO)
                   + (show_dp ? ND_DP_RATIO : 0.0));
        if (by_width < D)
            D = by_width;
    }
    if (D < 3.0) D = 3.0;
    return D;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

static gboolean
pn_numeric_display_draw (GtkWidget *widget, cairo_t *cr)
{
    PnNumericDisplay *self = PN_NUMERIC_DISPLAY (widget);
    GtkAllocation     alloc;
    guint             int_n, frac_n;
    gboolean          show_dp;
    double            D, H, t, s_intra, dp_w, cw;
    double            x_cur, y;
    gint              int_d[12];
    gint              frac_d[6];
    gboolean          neg = FALSE;
    gboolean          overload = FALSE;
    guint             i;

    gtk_widget_get_allocation (widget, &alloc);
    clamped_layout (self, &int_n, &frac_n);
    show_dp = (frac_n > 0);

    /* No bezel/face — the digits sit on the transparent allocation, so
     * the panel shows through behind them.  Fits to whatever GTK granted. */
    D       = digit_width_fit (self, alloc.width, alloc.height);
    H       = D * ND_H_RATIO;
    t       = D * ND_THICK_RATIO;
    s_intra = D * ND_INTRA_RATIO;
    dp_w    = show_dp ? D * ND_DP_RATIO : 0.0;
    cw      = content_width (self, D);

    x_cur = (alloc.width - cw) * 0.5;
    if (x_cur < 0.0) x_cur = 0.0;
    y     = (alloc.height - H) * 0.5;

    /* Compose the cells.  Pre-message and overload follow the worksheet
     * painter's behaviour: blank screen vs all-dashes row. */
    if (!self->has_value)
    {
        for (i = 0; i < int_n; i++)  int_d[i]  = -1;
        for (i = 0; i < frac_n; i++) frac_d[i] = -1;
    }
    else if (!render_value (self->value, int_n, frac_n, int_d, frac_d, &neg))
    {
        overload = TRUE;
        for (i = 0; i < int_n; i++)  int_d[i]  = -2;
        for (i = 0; i < frac_n; i++) frac_d[i] = -2;
    }

    /* Sign cell. */
    draw_cell (self, cr, x_cur, y, D, H, t,
               (self->has_value && neg) ? SEG_MINUS : 0);
    x_cur += D + s_intra;

    /* Integer digits. */
    for (i = 0; i < int_n; i++)
    {
        guint m;
        if      (int_d[i] == -1) m = 0;
        else if (int_d[i] == -2) m = SEG_MINUS;
        else                     m = seg_masks[int_d[i]];
        draw_cell (self, cr, x_cur, y, D, H, t, m);

        if (show_dp && i == int_n - 1)
        {
            if (self->has_value || overload)
                draw_dot (self, cr, x_cur + D, y, H, t);
            x_cur += D + dp_w;
        }
        else
        {
            x_cur += D + s_intra;
        }
    }

    /* Fractional digits. */
    for (i = 0; i < frac_n; i++)
    {
        guint m;
        if      (frac_d[i] == -1) m = 0;
        else if (frac_d[i] == -2) m = SEG_MINUS;
        else                      m = seg_masks[frac_d[i]];
        draw_cell (self, cr, x_cur, y, D, H, t, m);
        x_cur += D + s_intra;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_numeric_display_get_preferred_width (GtkWidget *widget,
                                        gint      *minimum,
                                        gint      *natural)
{
    PnNumericDisplay *self = PN_NUMERIC_DISPLAY (widget);
    /* Width is a function of the requested height and the layout: ask
     * for exactly the strip the digits span at the configured height. */
    double D = digit_width_fit (self, -1.0, (double) self->height);
    gint   w = (gint) ceil (content_width (self, D));

    *minimum = *natural = w;
}

static void
pn_numeric_display_get_preferred_height (GtkWidget *widget,
                                         gint      *minimum,
                                         gint      *natural)
{
    PnNumericDisplay *self = PN_NUMERIC_DISPLAY (widget);
    *minimum = *natural = self->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_numeric_display_class_init (PnNumericDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->draw                 = pn_numeric_display_draw;
    widget_class->get_preferred_width  = pn_numeric_display_get_preferred_width;
    widget_class->get_preferred_height = pn_numeric_display_get_preferred_height;
}

static void
pn_numeric_display_init (PnNumericDisplay *self)
{
    self->value     = 0.0;
    self->has_value = FALSE;
    self->digits    = ND_DEFAULT_DIGITS;
    self->places    = ND_DEFAULT_DECIMAL_PLACES;
    self->height    = 24;

    self->lit_r = ND_LIT_R; self->lit_g = ND_LIT_G; self->lit_b = ND_LIT_B;
    self->lit_a = 1.0;
    self->unlit_r = ND_LIT_R * ND_DIM;
    self->unlit_g = ND_LIT_G * ND_DIM;
    self->unlit_b = ND_LIT_B * ND_DIM;
    self->unlit_a = 1.0;

    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_numeric_display_new (void)
{
    return g_object_new (PN_TYPE_NUMERIC_DISPLAY, NULL);
}

void
pn_numeric_display_set_value (PnNumericDisplay *self, gdouble value)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (!isfinite (value))
        return;
    if (self->has_value && self->value == value)
        return;

    self->has_value = TRUE;
    self->value     = value;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_numeric_display_set_has_value (PnNumericDisplay *self, gboolean has_value)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (self->has_value == has_value)
        return;
    self->has_value = has_value;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_numeric_display_set_digits (PnNumericDisplay *self, guint digits)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (digits < 1)  digits = 1;
    if (digits > 12) digits = 12;
    if (digits == self->digits)
        return;
    self->digits = digits;
    gtk_widget_queue_resize (GTK_WIDGET (self));   /* width changes */
}

void
pn_numeric_display_set_decimal_places (PnNumericDisplay *self, guint places)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (places > 6) places = 6;
    if (places == self->places)
        return;
    self->places = places;
    gtk_widget_queue_resize (GTK_WIDGET (self));   /* width changes */
}

void
pn_numeric_display_set_segment_color (PnNumericDisplay *self,
                                      gdouble red, gdouble green,
                                      gdouble blue, gdouble alpha)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (red == self->lit_r && green == self->lit_g && blue == self->lit_b
        && alpha == self->lit_a)
        return;
    self->lit_r = red; self->lit_g = green; self->lit_b = blue;
    self->lit_a = alpha;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_numeric_display_set_unlit_color (PnNumericDisplay *self,
                                    gdouble red, gdouble green,
                                    gdouble blue, gdouble alpha)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (red == self->unlit_r && green == self->unlit_g
        && blue == self->unlit_b && alpha == self->unlit_a)
        return;
    self->unlit_r = red; self->unlit_g = green; self->unlit_b = blue;
    self->unlit_a = alpha;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_numeric_display_set_height (PnNumericDisplay *self, gint height)
{
    g_return_if_fail (PN_IS_NUMERIC_DISPLAY (self));
    if (height < 8) height = 8;
    if (height == self->height)
        return;
    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}
