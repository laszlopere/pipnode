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
/*  PnNumeric — gui tier.                                              */
/*                                                                     */
/*  The cairo seven-segment drawing for the Numeric node.  Mirrors the */
/*  #PnCountdown painter: a dark bezel framing the LED face, then a    */
/*  row of digit cells.  This node draws an optional minus-sign cell   */
/*  on the left, the integer digits, a decimal point, and the          */
/*  fractional digits — no captions, no colon glyphs.                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-numeric-gui.h"
#include "pn-numeric.h"

#include <gtk/gtk.h>
#include <math.h>

/* Bit per segment: a=0, b=1, c=2, d=3, e=4, f=5, g=6. */
static const guint8 seg_masks[10] = {
    0x3F, /* 0 */ 0x06, /* 1 */ 0x5B, /* 2 */ 0x4F, /* 3 */ 0x66, /* 4 */
    0x6D, /* 5 */ 0x7D, /* 6 */ 0x07, /* 7 */ 0x7F, /* 8 */ 0x6F, /* 9 */
};

/* The g segment alone — a centre bar; used both for the minus sign in
 * the sign cell and for the "dashes" overload row. */
#define SEG_MINUS  0x40

/* Layout ratios, all expressed against the digit cell width D — same
 * proportions #PnCountdown uses so the two LED panels read identically. */
#define NM_H_RATIO       1.70   /* digit height / D                       */
#define NM_THICK_RATIO   0.18   /* bar thickness / D                      */
#define NM_INTRA_RATIO   0.14   /* gap between adjacent cells             */
#define NM_DP_RATIO      0.34   /* width of the decimal-point gap         */

/* Trace a horizontal hexagonal bar centred on the line (x1,cy)-(x2,cy). */
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

/* Trace a vertical hexagonal bar centred on the line (cx,y1)-(cx,y2). */
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

/* Fill the current path as one bar.  Lit bar gets a faint LED halo. */
static void
seg_fill (cairo_t *cr, gboolean on, const PnColor *lit, const PnColor *dim)
{
    if (on)
    {
        cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, lit->alpha);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, 0.30);
        cairo_set_line_width (cr, 2.0);
        cairo_stroke (cr);
    }
    else
    {
        cairo_set_source_rgba (cr, dim->red, dim->green, dim->blue, dim->alpha);
        cairo_fill (cr);
    }
}

/* Draw one seven-segment cell.  @mask is a bitmask of segments to light;
 * the unlit ones still paint as ghosts so the cell always reads as an
 * LED panel rather than empty black. */
static void
draw_cell (cairo_t       *cr,
           double         x,
           double         y,
           double         D,
           double         H,
           double         t,
           guint          mask,
           const PnColor *lit,
           const PnColor *dim)
{
    double inset = t * 0.6;
    double xl    = x + inset;
    double xr    = x + D - inset;
    double yt    = y + inset;
    double yb    = y + H - inset;
    double ym    = y + H * 0.5;
    double g     = t * 0.35;

    h_seg (cr, xl + g, xr - g, yt, t); seg_fill (cr, (mask & 0x01) != 0, lit, dim); /* a */
    v_seg (cr, xr, yt + g, ym - g, t); seg_fill (cr, (mask & 0x02) != 0, lit, dim); /* b */
    v_seg (cr, xr, ym + g, yb - g, t); seg_fill (cr, (mask & 0x04) != 0, lit, dim); /* c */
    h_seg (cr, xl + g, xr - g, yb, t); seg_fill (cr, (mask & 0x08) != 0, lit, dim); /* d */
    v_seg (cr, xl, ym + g, yb - g, t); seg_fill (cr, (mask & 0x10) != 0, lit, dim); /* e */
    v_seg (cr, xl, yt + g, ym - g, t); seg_fill (cr, (mask & 0x20) != 0, lit, dim); /* f */
    h_seg (cr, xl + g, xr - g, ym, t); seg_fill (cr, (mask & 0x40) != 0, lit, dim); /* g */
}

/* Draw the decimal-point dot to the right of the cell whose right edge
 * is at @x_right, at the cell baseline. */
static void
draw_dot (cairo_t       *cr,
          double         x_right,
          double         y,
          double         H,
          double         t,
          const PnColor *lit)
{
    double r  = t * 0.55;
    double cx = x_right + t * 0.6;
    double cy = y + H - t * 0.6;

    cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, lit->alpha);
    cairo_arc (cr, cx, cy, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
}

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

/* Largest integer @digits seven-segment cells can show (10^digits - 1). */
static gint64
max_for_digits (guint digits)
{
    gint64 cap = 1;
    guint  i;
    for (i = 0; i < digits; i++)
        cap *= 10;
    return cap - 1;
}

/* Render the value into a digit array: int_d[0..int_n-1] is the integer
 * part (most significant first; -1 marks a leading-blank cell), and
 * frac_d[0..frac_n-1] is the fractional part (left to right).  Returns
 * TRUE on success, FALSE when the rounded absolute value does not fit
 * in @int_n digits (overload).  Also reports the sign so the caller can
 * pick the right mask for the sign cell. */
static gboolean
render_value (gdouble  value,
              guint    int_n,
              guint    frac_n,
              gint    *int_d,    /* [int_n] */
              gint    *frac_d,   /* [frac_n] */
              gboolean *neg_out)
{
    gboolean neg = (value < 0.0);
    gdouble  av  = fabs (value);
    gdouble  scale;
    gint64   scaled, int_part, max_int;
    guint    i;

    *neg_out = neg;

    /* Round half-away-from-zero to the configured fractional resolution. */
    scale  = pow (10.0, (double) frac_n);
    scaled = (gint64) floor (av * scale + 0.5);

    /* Split: low @frac_n digits are the fractional part, the rest the
     * integer part. */
    {
        gint64 div = 1;
        for (i = 0; i < frac_n; i++) div *= 10;
        int_part = scaled / div;

        /* Fractional digits left-to-right. */
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
        return FALSE;   /* overload */

    /* Integer digits most-significant first, with leading blanks (cell
     * value -1) so a small reading like "1.23" reads as " 1.23" rather
     * than "001.23". */
    {
        gint64 div = max_for_digits (int_n - 1) + 1;   /* 10^(int_n-1) */
        gboolean started = FALSE;

        for (i = 0; i < int_n; i++)
        {
            gint d = (gint) ((int_part / div) % 10);
            if (d != 0)
                started = TRUE;
            if (!started && i < int_n - 1)
                int_d[i] = -1;          /* leading blank */
            else
                int_d[i] = d;           /* including the last cell, always shown */
            div /= 10;
        }
    }

    return TRUE;
}

static void
pn_numeric_paint_plot (PnNode  *node,
                       cairo_t *cr,
                       double   x,
                       double   y,
                       double   w,
                       double   h)
{
    PnNumeric           *self = PN_NUMERIC (node);
    PnNumericPaintState  st;
    PnColor              lit, dim;
    guint                int_n, frac_n, n_cells;
    double               pad = 10.0;
    double               avail_w, digit_h;
    double               D, H, t, s_intra, dp_w, content_w;
    double               x_cur, digit_y;
    gboolean             show_dp;
    gboolean             blank_screen;

    pn_numeric_get_paint_state (self, &st);

    int_n  = st.digits;
    frac_n = st.decimal_places;
    if (int_n  < 1)  int_n  = 1;
    if (int_n  > 12) int_n  = 12;
    if (frac_n > 6)  frac_n = 6;

    show_dp = (frac_n > 0);
    /* One sign cell + integer digits + (optional) decimal-point gap +
     * fractional digits. */
    n_cells = 1 + int_n + frac_n;

    lit = st.segment_color;
    dim = st.unlit_segment_color;

    /* ---- panel: a dark bezel framing the screen face. ---- */
    rounded_rect (cr, x, y, w, h, 9.0);
    cairo_set_source_rgba (cr, 0.12, 0.12, 0.13, 1.0);
    cairo_fill (cr);

    rounded_rect (cr, x + 4.0, y + 4.0, w - 8.0, h - 8.0, 6.0);
    cairo_set_source_rgba (cr, st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill (cr);

    /* ---- layout: digits fill the screen face (no caption strip). ---- */
    avail_w = w - 2.0 * pad;
    digit_h = h - 2.0 * pad;
    if (digit_h < 1.0 || avail_w < 1.0)
        return;

    /* Solve the digit width D so the whole row fits both the available
     * width and the digit-strip height. */
    {
        double by_width  = avail_w
                / ((double) n_cells * (1.0 + NM_INTRA_RATIO)
                   + (show_dp ? NM_DP_RATIO : 0.0));
        double by_height = digit_h / NM_H_RATIO;
        D = fmin (by_width, by_height);
        if (D < 4.0) D = 4.0;
    }

    H       = D * NM_H_RATIO;
    t       = D * NM_THICK_RATIO;
    s_intra = D * NM_INTRA_RATIO;
    dp_w    = show_dp ? D * NM_DP_RATIO : 0.0;

    content_w = (double) n_cells * D
              + (double) n_cells * s_intra
              + dp_w;

    x_cur   = x + (w - content_w) * 0.5;
    digit_y = y + pad + (digit_h - H) * 0.5;

    /* ---- compose the cell masks. ---- */
    {
        gint     int_d[12];
        gint     frac_d[6];
        gboolean neg = FALSE;
        gboolean ok;
        guint    i;
        guint    sign_mask;

        blank_screen = !st.has_value;

        if (blank_screen)
        {
            /* No reading yet: leave the screen as off-state ghosts only.  */
            ok  = TRUE;
            neg = FALSE;
            for (i = 0; i < int_n; i++)  int_d[i]  = -1;
            for (i = 0; i < frac_n; i++) frac_d[i] = -1;
        }
        else
        {
            ok = render_value (st.value, int_n, frac_n, int_d, frac_d, &neg);
            if (!ok)
            {
                /* Overload: every digit reads as a dash (segment g only).  */
                for (i = 0; i < int_n; i++)  int_d[i]  = -2;
                for (i = 0; i < frac_n; i++) frac_d[i] = -2;
                /* Sign cell mirrors the value's sign so a "below the
                 * negative end of the range" reading still reads as
                 * negative; a positive overload leaves the sign blank. */
            }
        }

        sign_mask = neg ? SEG_MINUS : 0;

        /* Sign cell. */
        draw_cell (cr, x_cur, digit_y, D, H, t, sign_mask, &lit, &dim);
        x_cur += D + s_intra;

        /* Integer digits (most-significant first).  -1 = blank, -2 = dash. */
        for (i = 0; i < int_n; i++)
        {
            guint m;
            if      (int_d[i] == -1) m = 0;
            else if (int_d[i] == -2) m = SEG_MINUS;
            else                     m = seg_masks[int_d[i]];
            draw_cell (cr, x_cur, digit_y, D, H, t, m, &lit, &dim);
            /* Decimal point after the last integer cell — but only when
             * there is a fractional block; the dot lives in the gap, not
             * inside a cell, so the digit cell next to it stays clean. */
            if (show_dp && i == int_n - 1)
            {
                /* Only light the dot when the screen actually shows a
                 * reading (or an overload — overload still shows the dot
                 * to keep the layout self-evident). */
                if (!blank_screen)
                    draw_dot (cr, x_cur + D, digit_y, H, t, &lit);
                x_cur += D + dp_w;
            }
            else
            {
                x_cur += D + s_intra;
            }
        }

        /* Fractional digits (left to right). */
        for (i = 0; i < frac_n; i++)
        {
            guint m;
            if      (frac_d[i] == -1) m = 0;
            else if (frac_d[i] == -2) m = SEG_MINUS;
            else                      m = seg_masks[frac_d[i]];
            draw_cell (cr, x_cur, digit_y, D, H, t, m, &lit, &dim);
            x_cur += D + s_intra;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_numeric_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_NUMERIC));

    node_class->paint_plot = pn_numeric_paint_plot;
    /* Same rounded plastic bezel as #PnCountdown — match the drop-shadow
     * silhouette and treat a press as drag/select rather than zoom. */
    node_class->paint_plot_corner_radius = 9.0;
    node_class->paint_plot_skip_zoom     = TRUE;

    /* Leak the class ref intentionally so the slots stay valid for the
     * process lifetime, mirroring the other gui-tier installers. */
}
