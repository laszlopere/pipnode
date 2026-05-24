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
/*  PnLedDisplay — a panel-sized seven-segment countdown               */
/*                                                                     */
/*  A miniature of the Countdown node's client area: the same          */
/*  hexagonal-bar geometry, red-on-black LED look and "ddd hh:mm:ss"   */
/*  layout (a zero-padded days block, a wider gap, then HH:MM:SS with  */
/*  colons), sized to a panel row.  Fed a count of seconds remaining,  */
/*  it breaks the value down exactly as pn-countdown.c does.           */
/*  Self-contained cairo + GTK so the applet never links a pipnode     */
/*  library.                                                           */
/* ------------------------------------------------------------------ */

#include "pn-led-display.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Seven-segment geometry                                             */
/*                                                                     */
/*  Segments a..g in the classic layout; bit per segment is            */
/*  a=0, b=1, c=2, d=3, e=4, f=5, g=6:                                  */
/*                                                                     */
/*       aaaa            digit cell is D wide, H tall; bars are flat    */
/*      f    b           hexagons of thickness t.  Lit bars use the     */
/*      f    b           LED colour, unlit ones a dim ghost of it so    */
/*       gggg            an off digit still reads as an LED rather than  */
/*      e    c           empty black.                                   */
/*      e    c                                                          */
/*       dddd                                                           */
/* ------------------------------------------------------------------ */

static const guint8 seg_masks[10] = {
    0x3F, /* 0 */ 0x06, /* 1 */ 0x5B, /* 2 */ 0x4F, /* 3 */ 0x66, /* 4 */
    0x6D, /* 5 */ 0x7D, /* 6 */ 0x07, /* 7 */ 0x7F, /* 8 */ 0x6F, /* 9 */
};

/* Layout ratios, all relative to the digit cell width D (matching the
 * Countdown node's pn-countdown-gui.c). */
#define LED_H_RATIO      1.70   /* digit height / D                      */
#define LED_THICK_RATIO  0.18   /* bar thickness / D                     */
#define LED_INTRA_RATIO  0.14   /* gap between digits in the same group  */
#define LED_GROUP_RATIO  0.55   /* gap between the days block and HH:MM:SS */
#define LED_COLON_RATIO  0.50   /* colon column width / D                */

/* Segment colours, matching the Countdown node's defaults: classic LED
 * red digits on a transparent background, dim red unlit ghosts. */
#define LED_LIT_R   0.92
#define LED_LIT_G   0.12
#define LED_LIT_B   0.08
#define LED_DIM     0.16   /* unlit segment = lit colour * LED_DIM */

/* Seconds-per-unit constants for the breakdown. */
#define LED_SECS_PER_DAY   86400
#define LED_SECS_PER_HOUR   3600
#define LED_SECS_PER_MIN      60

struct _PnLedDisplay
{
    GtkDrawingArea parent_instance;

    gint64 seconds;     /* value being shown, clamped at zero          */
    guint  day_digits;  /* cells reserved for the days field           */
    gint   height;      /* requested overall pixel height              */

    /* Segment colours, defaulting to the LED_* constants above but
     * overridable so the panel editor can mirror the Countdown node's
     * configured lit / unlit colours (pn_led_display_set_*_color). */
    gdouble lit_r,   lit_g,   lit_b;     /* lit seven-segment bar       */
    gdouble unlit_r, unlit_g, unlit_b;   /* unlit off-state ghost bar   */
};

G_DEFINE_TYPE (PnLedDisplay, pn_led_display, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Value parsing                                                      */
/* ------------------------------------------------------------------ */

gboolean
pn_led_display_parse_seconds (const gchar *text, gint64 *out_seconds)
{
    const gchar *p;
    gchar       *end = NULL;
    gdouble      v;

    if (text == NULL)
        return FALSE;

    /* Skip leading blanks; reject an empty / blank string. */
    for (p = text; *p == ' ' || *p == '\t'; p++)
        ;
    if (*p == '\0')
        return FALSE;

    v = g_ascii_strtod (p, &end);
    if (end == p || !isfinite (v))
        return FALSE;

    /* Only a bare number counts; trailing units or words (e.g. "20 °C")
     * are not a countdown value and should fall back to a text label. */
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return FALSE;

    if (out_seconds != NULL)
        *out_seconds = (gint64) v;   /* floor toward zero */
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Bar drawing (adapted from pn-countdown-gui.c)                       */
/* ------------------------------------------------------------------ */

/* Trace a horizontal hexagonal bar centred on (x1,cy)-(x2,cy). */
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

/* Trace a vertical hexagonal bar centred on (cx,y1)-(cx,y2). */
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

/* Fill the current path as one bar: lit bars are filled solid in the
 * configured lit colour and given a faint translucent halo stroke; unlit
 * bars are painted in the configured off-state ghost colour. */
static void
seg_fill (PnLedDisplay *self, cairo_t *cr, gboolean on)
{
    if (on)
    {
        cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b, 1.0);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b, 0.30);
        cairo_set_line_width (cr, 1.5);
        cairo_stroke (cr);
    }
    else
    {
        cairo_set_source_rgba (cr, self->unlit_r, self->unlit_g,
                               self->unlit_b, 1.0);
        cairo_fill (cr);
    }
}

/* Draw one seven-segment digit in the cell at (x,y) of size (D,H).  A
 * @val outside 0..9 lights no segments (a blank cell). */
static void
draw_digit (PnLedDisplay *self, cairo_t *cr,
            double x, double y, double D, double H, double t, int val)
{
    double inset = t * 0.6;
    double xl    = x + inset;
    double xr    = x + D - inset;
    double yt    = y + inset;
    double yb    = y + H - inset;
    double ym    = y + H * 0.5;
    double g     = t * 0.35;
    guint  mask  = (val >= 0 && val <= 9) ? seg_masks[val] : 0;

    h_seg (cr, xl + g, xr - g, yt, t); seg_fill (self, cr, (mask & 0x01) != 0); /* a */
    v_seg (cr, xr, yt + g, ym - g, t); seg_fill (self, cr, (mask & 0x02) != 0); /* b */
    v_seg (cr, xr, ym + g, yb - g, t); seg_fill (self, cr, (mask & 0x04) != 0); /* c */
    h_seg (cr, xl + g, xr - g, yb, t); seg_fill (self, cr, (mask & 0x08) != 0); /* d */
    v_seg (cr, xl, ym + g, yb - g, t); seg_fill (self, cr, (mask & 0x10) != 0); /* e */
    v_seg (cr, xl, yt + g, ym - g, t); seg_fill (self, cr, (mask & 0x20) != 0); /* f */
    h_seg (cr, xl + g, xr - g, ym, t); seg_fill (self, cr, (mask & 0x40) != 0); /* g */
}

/* Draw the two always-lit colon dots in a colon column centred at @cx,
 * in the configured lit colour. */
static void
draw_colon (PnLedDisplay *self, cairo_t *cr, double cx, double y,
            double H, double t)
{
    double r  = t * 0.62;
    double y1 = y + H * 0.34;
    double y2 = y + H * 0.66;

    cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b, 1.0);
    cairo_arc (cr, cx, y1, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_arc (cr, cx, y2, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
}

/* Largest value @digits seven-segment cells can show (10^digits - 1). */
static gint64
max_for_digits (guint digits)
{
    gint64 cap = 1;
    guint  i;
    for (i = 0; i < digits; i++)
        cap *= 10;
    return cap - 1;
}

/* ------------------------------------------------------------------ */
/*  Sizing                                                             */
/* ------------------------------------------------------------------ */

/* The bezel padding inset around the digit strip, in pixels. */
#define LED_PAD 2.0

/* Number of digit cells: the days block plus HH, MM and SS. */
static guint
cell_count (PnLedDisplay *self)
{
    return self->day_digits + 6;
}

/* The content-width-per-D factor: how many digit widths the whole strip
 * spans (nd cells + their intra gaps + two colon columns + one group
 * gap).  content_width(D) == D * this. */
static double
width_per_digit (PnLedDisplay *self)
{
    guint nd = cell_count (self);

    return (double) nd * (1.0 + LED_INTRA_RATIO)
         + 2.0 * LED_COLON_RATIO
         + LED_GROUP_RATIO;
}

/* Total content width (digit strip, no bezel padding) at digit width D. */
static double
content_width (PnLedDisplay *self, double D)
{
    return D * width_per_digit (self);
}

/* Solve the digit width D so the whole strip fits BOTH the available
 * width and height (mirroring the Countdown node's paint_plot): the
 * digits are sized to whichever dimension binds, so the readout never
 * overflows its allocation.  @avail_w <= 0 means width is unconstrained
 * (used by the size request, which derives width from the height). */
static double
digit_width_fit (PnLedDisplay *self, double avail_w, double avail_h)
{
    double by_height = (avail_h - 2.0 * LED_PAD) / LED_H_RATIO;
    double D         = by_height;

    if (avail_w > 0.0)
    {
        double by_width = (avail_w - 2.0 * LED_PAD) / width_per_digit (self);
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
pn_led_display_draw (GtkWidget *widget, cairo_t *cr)
{
    PnLedDisplay *self = PN_LED_DISPLAY (widget);
    GtkAllocation alloc;
    double D, H, t, s_intra, s_group, colon_w;
    double x_cur, y;
    gint64 rem, daysv, daymax;
    guint  dd = self->day_digits;
    int    hh[2], mm[2], ss[2];
    int    i;

    gtk_widget_get_allocation (widget, &alloc);

    /* No frame or background: the digits are drawn straight onto the
     * transparent allocation, so the panel shows through behind them. */
    D       = digit_width_fit (self, alloc.width, alloc.height);
    H       = D * LED_H_RATIO;
    t       = D * LED_THICK_RATIO;
    s_intra = D * LED_INTRA_RATIO;
    s_group = D * LED_GROUP_RATIO;
    colon_w = D * LED_COLON_RATIO;

    /* Centre the digit strip in the allocation. */
    x_cur = (alloc.width - content_width (self, D)) * 0.5;
    if (x_cur < LED_PAD)
        x_cur = LED_PAD;
    y = (alloc.height - H) * 0.5;

    /* Break the clamped seconds count down the same way pn-countdown.c
     * does, saturating the days field rather than wrapping. */
    rem    = self->seconds > 0 ? self->seconds : 0;
    daysv  = rem / LED_SECS_PER_DAY;
    daymax = max_for_digits (dd);
    if (daysv > daymax)
        daysv = daymax;

    hh[0] = (int) ((rem % LED_SECS_PER_DAY)  / LED_SECS_PER_HOUR) / 10;
    hh[1] = (int) ((rem % LED_SECS_PER_DAY)  / LED_SECS_PER_HOUR) % 10;
    mm[0] = (int) ((rem % LED_SECS_PER_HOUR) / LED_SECS_PER_MIN)  / 10;
    mm[1] = (int) ((rem % LED_SECS_PER_HOUR) / LED_SECS_PER_MIN)  % 10;
    ss[0] = (int)  (rem % LED_SECS_PER_MIN) / 10;
    ss[1] = (int)  (rem % LED_SECS_PER_MIN) % 10;

    /* Days: dd zero-padded cells, most-significant first. */
    for (i = 0; i < (int) dd; i++)
    {
        gint64 div = max_for_digits (dd - 1 - i) + 1;   /* 10^(dd-1-i) */
        int    d   = (int) ((daysv / div) % 10);

        if (i > 0) x_cur += s_intra;
        draw_digit (self, cr, x_cur, y, D, H, t, d);
        x_cur += D;
    }

    /* The wider days -> hours group gap. */
    x_cur += s_group;

    /* Hours, colon, minutes, colon, seconds. */
    draw_digit (self, cr, x_cur, y, D, H, t, hh[0]); x_cur += D + s_intra;
    draw_digit (self, cr, x_cur, y, D, H, t, hh[1]); x_cur += D + s_intra;
    draw_colon (self, cr, x_cur + colon_w * 0.5, y, H, t);
    x_cur += colon_w + s_intra;

    draw_digit (self, cr, x_cur, y, D, H, t, mm[0]); x_cur += D + s_intra;
    draw_digit (self, cr, x_cur, y, D, H, t, mm[1]); x_cur += D + s_intra;
    draw_colon (self, cr, x_cur + colon_w * 0.5, y, H, t);
    x_cur += colon_w + s_intra;

    draw_digit (self, cr, x_cur, y, D, H, t, ss[0]); x_cur += D + s_intra;
    draw_digit (self, cr, x_cur, y, D, H, t, ss[1]);

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_led_display_get_preferred_width (GtkWidget *widget,
                                    gint      *minimum,
                                    gint      *natural)
{
    PnLedDisplay *self = PN_LED_DISPLAY (widget);
    /* Width unconstrained: size the digits to the nominal height, then
     * ask for exactly the strip they span.  draw() re-fits to whatever
     * the panel actually grants, so the readout is never clipped. */
    double D = digit_width_fit (self, -1.0, self->height);
    gint   w = (gint) ceil (content_width (self, D) + 2.0 * LED_PAD);

    *minimum = *natural = w;
}

static void
pn_led_display_get_preferred_height (GtkWidget *widget,
                                     gint      *minimum,
                                     gint      *natural)
{
    PnLedDisplay *self = PN_LED_DISPLAY (widget);

    *minimum = *natural = self->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_led_display_class_init (PnLedDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->draw                 = pn_led_display_draw;
    widget_class->get_preferred_width  = pn_led_display_get_preferred_width;
    widget_class->get_preferred_height = pn_led_display_get_preferred_height;
}

static void
pn_led_display_init (PnLedDisplay *self)
{
    self->seconds    = 0;
    self->day_digits = 3;
    self->height     = 24;   /* matches PnLedLamp's default so a mixed row
                              * of applet widgets stands one height */

    /* Classic LED red lit, dim red ghost — the Countdown node's defaults. */
    self->lit_r   = LED_LIT_R;
    self->lit_g   = LED_LIT_G;
    self->lit_b   = LED_LIT_B;
    self->unlit_r = LED_LIT_R * LED_DIM;
    self->unlit_g = LED_LIT_G * LED_DIM;
    self->unlit_b = LED_LIT_B * LED_DIM;

    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_led_display_new (void)
{
    return g_object_new (PN_TYPE_LED_DISPLAY, NULL);
}

void
pn_led_display_set_seconds (PnLedDisplay *self, gint64 seconds)
{
    g_return_if_fail (PN_IS_LED_DISPLAY (self));

    if (seconds < 0)
        seconds = 0;
    if (seconds == self->seconds)
        return;

    self->seconds = seconds;

    /* The strip width is fixed (zero-padded days), so ticking seconds do
     * not change the requested size — force a repaint explicitly so the
     * clock actually refreshes. */
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_led_display_set_day_digits (PnLedDisplay *self, guint digits)
{
    g_return_if_fail (PN_IS_LED_DISPLAY (self));

    if (digits < 1) digits = 1;
    if (digits > 6) digits = 6;
    if (digits == self->day_digits)
        return;

    self->day_digits = digits;
    gtk_widget_queue_resize (GTK_WIDGET (self));   /* width changes */
}

void
pn_led_display_set_segment_color (PnLedDisplay *self,
                                  gdouble red, gdouble green, gdouble blue)
{
    g_return_if_fail (PN_IS_LED_DISPLAY (self));

    if (red == self->lit_r && green == self->lit_g && blue == self->lit_b)
        return;

    self->lit_r = red;
    self->lit_g = green;
    self->lit_b = blue;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_led_display_set_unlit_color (PnLedDisplay *self,
                                gdouble red, gdouble green, gdouble blue)
{
    g_return_if_fail (PN_IS_LED_DISPLAY (self));

    if (red == self->unlit_r && green == self->unlit_g &&
        blue == self->unlit_b)
        return;

    self->unlit_r = red;
    self->unlit_g = green;
    self->unlit_b = blue;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_led_display_set_height (PnLedDisplay *self, gint height)
{
    g_return_if_fail (PN_IS_LED_DISPLAY (self));

    if (height < 8)
        height = 8;
    if (height == self->height)
        return;

    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}
