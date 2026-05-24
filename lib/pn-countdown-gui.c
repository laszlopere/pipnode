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
/*  PnCountdown — gui tier.                                            */
/*                                                                     */
/*  The cairo seven-segment LED drawing for the Countdown node.  The    */
/*  node's GType, properties, receive() and the days/hours/minutes/     */
/*  seconds breakdown live in the GTK-free core file pn-countdown.c;    */
/*  this file installs the paint_plot vfunc slot onto that class at      */
/*  editor startup (pn_countdown_gui_install), reading the drawing state */
/*  through the core's GTK-free snapshot accessor                       */
/*  pn_countdown_get_paint_state().  The headless runtime never loads    */
/*  this half, so the Countdown logic runs without GTK.                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-countdown-gui.h"
#include "pn-countdown.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Seven-segment geometry                                             */
/*                                                                     */
/*  Segments are labelled a..g in the classic layout:                  */
/*                                                                     */
/*       aaaa            digit cell is D wide, H tall; bars are drawn   */
/*      f    b           as flat hexagons of thickness t.  Lit bars use */
/*      f    b           the configured segment colour, unlit ones a    */
/*       gggg            dim ghost of it so an off digit still reads as  */
/*      e    c           an LED panel rather than empty black.          */
/*      e    c                                                          */
/*       dddd                                                           */
/* ------------------------------------------------------------------ */

/* Bit per segment: a=0, b=1, c=2, d=3, e=4, f=5, g=6. */
static const guint8 seg_masks[10] = {
    0x3F, /* 0: a b c d e f   */
    0x06, /* 1: b c           */
    0x5B, /* 2: a b g e d     */
    0x4F, /* 3: a b g c d     */
    0x66, /* 4: f g b c       */
    0x6D, /* 5: a f g c d     */
    0x7D, /* 6: a f g e c d   */
    0x07, /* 7: a b c         */
    0x7F, /* 8: all           */
    0x6F, /* 9: a b c d f g   */
};

/* Layout ratios, all expressed against the digit cell width D. */
#define CD_H_RATIO       1.70   /* digit height / D                      */
#define CD_THICK_RATIO   0.18   /* bar thickness / D                     */
#define CD_INTRA_RATIO   0.14   /* gap between digits in the same group  */
#define CD_GROUP_RATIO   0.55   /* gap between the days block and HH:MM:SS */
#define CD_COLON_RATIO   0.50   /* colon column width / D                */

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

/* Fill the current path as one bar.  A lit bar is filled in @lit and then
 * stroked with a translucent copy of @lit so it carries a faint LED halo;
 * an unlit bar is filled in @dim and left as a dark ghost. */
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

/* Draw one seven-segment digit in the cell at (x, y) of size (D, H).  A
 * @val outside 0..9 (used for a blank cell) lights no segments. */
static void
draw_digit (cairo_t       *cr,
            double         x,
            double         y,
            double         D,
            double         H,
            double         t,
            int            val,
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
    guint  mask  = (val >= 0 && val <= 9) ? seg_masks[val] : 0;

    h_seg (cr, xl + g, xr - g, yt, t); seg_fill (cr, (mask & 0x01) != 0, lit, dim); /* a */
    v_seg (cr, xr, yt + g, ym - g, t); seg_fill (cr, (mask & 0x02) != 0, lit, dim); /* b */
    v_seg (cr, xr, ym + g, yb - g, t); seg_fill (cr, (mask & 0x04) != 0, lit, dim); /* c */
    h_seg (cr, xl + g, xr - g, yb, t); seg_fill (cr, (mask & 0x08) != 0, lit, dim); /* d */
    v_seg (cr, xl, ym + g, yb - g, t); seg_fill (cr, (mask & 0x10) != 0, lit, dim); /* e */
    v_seg (cr, xl, yt + g, ym - g, t); seg_fill (cr, (mask & 0x20) != 0, lit, dim); /* f */
    h_seg (cr, xl + g, xr - g, ym, t); seg_fill (cr, (mask & 0x40) != 0, lit, dim); /* g */
}

/* Draw the two always-lit colon dots in a colon column centred at @cx. */
static void
draw_colon (cairo_t       *cr,
            double         cx,
            double         y,
            double         H,
            double         t,
            const PnColor *lit)
{
    double r  = t * 0.62;
    double y1 = y + H * 0.34;
    double y2 = y + H * 0.66;

    cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, lit->alpha);
    cairo_arc (cr, cx, y1, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_arc (cr, cx, y2, r, 0.0, 2.0 * M_PI);
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

/* Paint a horizontally-centred caption at (cx, cy) in the label colour. */
static void
draw_label (cairo_t *cr, const gchar *text, double cx, double cy, double size,
            const PnColor *color)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    gchar                *font;
    int                   pw, ph;

    if (size < 6.0) size = 6.0;

    layout = pango_cairo_create_layout (cr);
    font   = g_strdup_printf ("Sans Bold %d", (int) size);
    fd     = pango_font_description_from_string (font);
    g_free (font);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
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

static void
pn_countdown_paint_plot (PnNode  *node,
                         cairo_t *cr,
                         double   x,
                         double   y,
                         double   w,
                         double   h)
{
    PnCountdown           *self = PN_COUNTDOWN (node);
    PnCountdownPaintState  st;
    PnColor                lit, dim;
    guint                  dd;
    guint                  nd;
    double                 pad = 10.0;
    double                 label_h;
    double                 avail_w, digit_top, digit_h;
    double                 D, H, t, s_intra, s_group, colon_w, content_w;
    double                 x_cur, digit_y;

    pn_countdown_get_paint_state (self, &st);

    dd = st.day_digits;
    if (dd < 1) dd = 1;
    if (dd > 6) dd = 6;
    nd = dd + 6;                       /* days + HH + MM + SS digit cells */

    lit = st.segment_color;
    /* Unlit segment: the configured off-state ghost over the black face. */
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

    /* ---- layout: a caption strip on top, the digits below. ---- */
    label_h   = st.show_labels ? h * 0.24 : 0.0;
    avail_w   = w - 2.0 * pad;
    digit_top = y + pad + label_h;
    digit_h   = h - 2.0 * pad - label_h;
    if (digit_h < 1.0 || avail_w < 1.0)
        return;

    /* Solve the digit width D so the whole row fits both the available
     * width and the digit-strip height; see content_w below. */
    {
        double by_width  = avail_w / ((double) nd * (1.0 + CD_INTRA_RATIO)
                                      + 2.0 * CD_COLON_RATIO
                                      + CD_GROUP_RATIO);
        double by_height = digit_h / CD_H_RATIO;
        D = fmin (by_width, by_height);
        if (D < 4.0) D = 4.0;
    }

    H       = D * CD_H_RATIO;
    t       = D * CD_THICK_RATIO;
    s_intra = D * CD_INTRA_RATIO;
    s_group = D * CD_GROUP_RATIO;
    colon_w = D * CD_COLON_RATIO;

    /* content_w sums the dd+6 digit cells, the two colon columns and the
     * inter-slot gaps (one group gap, nd intra gaps). */
    content_w = (double) nd * D + 2.0 * colon_w
              + s_group + (double) nd * s_intra;

    x_cur   = x + (w - content_w) * 0.5;
    digit_y = digit_top + (digit_h - H) * 0.5;

    /* ---- the four digit groups, recording each group's x-range so the
     *      caption can be centred over it. ---- */
    {
        gint64 daysv  = st.days;
        gint64 daymax = max_for_digits (dd);
        double grp_x0[4], grp_x1[4];
        int    i;

        if (daysv < 0)      daysv = 0;
        if (daysv > daymax) daysv = daymax;   /* saturate rather than wrap */

        for (i = 0; i < 4; i++) { grp_x0[i] = 0.0; grp_x1[i] = 0.0; }

        /* Days: dd cells, zero-padded, most-significant first. */
        grp_x0[0] = x_cur;
        for (i = 0; i < (int) dd; i++)
        {
            gint64 div = max_for_digits (dd - 1 - i) + 1; /* 10^(dd-1-i) */
            int    d   = (int) ((daysv / div) % 10);

            if (i > 0) x_cur += s_intra;
            draw_digit (cr, x_cur, digit_y, D, H, t, d, &lit, &dim);
            x_cur += D;
        }
        grp_x1[0] = x_cur;

        /* Days -> hours: the wider group gap. */
        x_cur += s_group;

        /* Hours (2 digits), colon, minutes (2), colon, seconds (2). */
        {
            int hh[2] = { st.hours   / 10, st.hours   % 10 };
            int mm[2] = { st.minutes / 10, st.minutes % 10 };
            int ss[2] = { st.seconds / 10, st.seconds % 10 };

            grp_x0[1] = x_cur;
            draw_digit (cr, x_cur, digit_y, D, H, t, hh[0], &lit, &dim);
            x_cur += D + s_intra;
            draw_digit (cr, x_cur, digit_y, D, H, t, hh[1], &lit, &dim);
            x_cur += D;
            grp_x1[1] = x_cur;

            x_cur += s_intra;
            draw_colon (cr, x_cur + colon_w * 0.5, digit_y, H, t, &lit);
            x_cur += colon_w + s_intra;

            grp_x0[2] = x_cur;
            draw_digit (cr, x_cur, digit_y, D, H, t, mm[0], &lit, &dim);
            x_cur += D + s_intra;
            draw_digit (cr, x_cur, digit_y, D, H, t, mm[1], &lit, &dim);
            x_cur += D;
            grp_x1[2] = x_cur;

            x_cur += s_intra;
            draw_colon (cr, x_cur + colon_w * 0.5, digit_y, H, t, &lit);
            x_cur += colon_w + s_intra;

            grp_x0[3] = x_cur;
            draw_digit (cr, x_cur, digit_y, D, H, t, ss[0], &lit, &dim);
            x_cur += D + s_intra;
            draw_digit (cr, x_cur, digit_y, D, H, t, ss[1], &lit, &dim);
            x_cur += D;
            grp_x1[3] = x_cur;
        }

        /* ---- captions. ---- */
        if (st.show_labels)
        {
            static const gchar *caps[4] =
                { "DAYS", "HOURS", "MINUTES", "SECONDS" };
            double cy   = y + pad + label_h * 0.5;
            double size = label_h * 0.38;

            for (i = 0; i < 4; i++)
                draw_label (cr, caps[i],
                            (grp_x0[i] + grp_x1[i]) * 0.5, cy, size,
                            &st.label_color);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_countdown_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_COUNTDOWN));

    node_class->paint_plot = pn_countdown_paint_plot;
    /* The panel has rounded corners, so the standard rectangular drop
     * shadow would leak past them; and the at-rest display is already the
     * readable surface, so a press should select / drag rather than lift
     * a zoomed copy. */
    node_class->paint_plot_skip_shadow = TRUE;
    node_class->paint_plot_skip_zoom   = TRUE;

    /* The class ref is intentionally held for the process lifetime so the
     * slots we just wrote stay valid (one leaked ref on a singleton
     * class, mirroring the other gui-tier installers). */
}
