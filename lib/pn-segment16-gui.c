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
/*  PnSegment16 — gui tier.                                            */
/*                                                                     */
/*  The cairo 16-segment LED drawing for the Segment16 node.  The      */
/*  node's GType, properties and receive() live in the GTK-free core   */
/*  file pn-segment16.c; this file installs the paint_plot vfunc slot  */
/*  onto that class at editor startup (pn_segment16_gui_install),      */
/*  reading the drawing state through                                  */
/*  pn_segment16_get_paint_state().  The headless runtime never loads  */
/*  this half.                                                         */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-segment16-gui.h"
#include "pn-segment16.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Sixteen-segment geometry                                           */
/*                                                                     */
/*  Segments are labelled with the conventional 16-seg names:          */
/*                                                                     */
/*       --a1-- --a2--          a1, a2 — top horizontals (split)        */
/*      |\  |  /|               b, c   — right verticals (upper/lower)  */
/*      f h i j b               d1, d2 — bottom horizontals (split)     */
/*      |  \|/  |               e, f   — left verticals (lower/upper)   */
/*       -g1- -g2-              g1, g2 — middle horizontals (split)     */
/*      |  /|\  |               h, j   — top diagonals (\, /)           */
/*      e k l m c               k, m   — bottom diagonals (/, \)        */
/*      |/  |  \|               i, l   — middle verticals (upper/lower) */
/*       --d1-- --d2--                                                  */
/*                                                                     */
/*  Each bar is a flat chevron-tipped hexagon, drawn the same way the  */
/*  seven-segment Countdown bars are drawn (pn-countdown-gui.c).  Lit  */
/*  bars use the configured segment colour with a faint LED halo;      */
/*  unlit bars use the dim ghost so an off cell still reads as an LED  */
/*  panel rather than empty black.                                     */
/* ------------------------------------------------------------------ */

#define S_A1 (1u << 0)
#define S_A2 (1u << 1)
#define S_B  (1u << 2)
#define S_C  (1u << 3)
#define S_D2 (1u << 4)
#define S_D1 (1u << 5)
#define S_E  (1u << 6)
#define S_F  (1u << 7)
#define S_G1 (1u << 8)
#define S_G2 (1u << 9)
#define S_H  (1u << 10)
#define S_I  (1u << 11)
#define S_J  (1u << 12)
#define S_K  (1u << 13)
#define S_L  (1u << 14)
#define S_M  (1u << 15)

/* Convenience shorthands for common combinations. */
#define S_A   (S_A1 | S_A2)
#define S_D   (S_D1 | S_D2)
#define S_G   (S_G1 | S_G2)
#define S_IL  (S_I  | S_L)
#define S_HJKM (S_H | S_J | S_K | S_M)

/* Glyph table, indexed by ASCII byte.  Anything not listed (control
 * characters, high-bit bytes, most punctuation) is zero — a blank cell.
 * Lowercase falls through to uppercase in the lookup helper, so only the
 * uppercase rows need to be populated unless we want a distinct shape for
 * a particular lower-case letter (none do here — the row is short
 * already; we can revisit later if a user asks for the small-letter
 * variants from the wikipedia chart). */
static const guint16 glyph_masks[128] = {
    /* 0x00..0x1F: control characters — blank. */
    [' ']  = 0,
    ['!']  = S_B | S_C | S_I,
    ['"']  = S_F | S_J,
    ['#']  = S_B | S_C | S_D | S_G | S_IL,
    ['$']  = S_A | S_C | S_D | S_F | S_G | S_IL,
    ['%']  = S_A1 | S_F | S_J | S_G | S_K | S_C | S_D2,
    ['&']  = S_A1 | S_F | S_G1 | S_H | S_M | S_E | S_D,
    ['\''] = S_J,
    ['(']  = S_J | S_M,
    [')']  = S_H | S_K,
    ['*']  = S_G | S_HJKM | S_IL,
    ['+']  = S_G | S_IL,
    [',']  = S_M,
    ['-']  = S_G,
    ['.']  = 0,                    /* a true LED period is a separate dot */
    ['/']  = S_J | S_K,

    ['0']  = S_A | S_B | S_C | S_D | S_E | S_F | S_J | S_K,
    ['1']  = S_B | S_C | S_J,
    ['2']  = S_A | S_B | S_D | S_E | S_G,
    ['3']  = S_A | S_B | S_C | S_D | S_G2,
    ['4']  = S_B | S_C | S_F | S_G,
    ['5']  = S_A | S_C | S_D | S_F | S_G,
    ['6']  = S_A | S_C | S_D | S_E | S_F | S_G,
    ['7']  = S_A | S_B | S_C,
    ['8']  = S_A | S_B | S_C | S_D | S_E | S_F | S_G,
    ['9']  = S_A | S_B | S_C | S_D | S_F | S_G,

    [':']  = S_IL,
    [';']  = S_I | S_M,
    ['<']  = S_J | S_M,
    ['=']  = S_D | S_G,
    ['>']  = S_H | S_K,
    ['?']  = S_A | S_B | S_G2 | S_L,
    ['@']  = S_A | S_B | S_D | S_E | S_F | S_G1 | S_I,

    ['A']  = S_A | S_B | S_C | S_E | S_F | S_G,
    ['B']  = S_A | S_B | S_C | S_D | S_G2 | S_IL,
    ['C']  = S_A | S_D | S_E | S_F,
    ['D']  = S_A | S_B | S_C | S_D | S_IL,
    ['E']  = S_A | S_D | S_E | S_F | S_G1,
    ['F']  = S_A | S_E | S_F | S_G1,
    ['G']  = S_A | S_C | S_D | S_E | S_F | S_G2,
    ['H']  = S_B | S_C | S_E | S_F | S_G,
    ['I']  = S_A | S_D | S_IL,
    ['J']  = S_B | S_C | S_D | S_E,
    ['K']  = S_E | S_F | S_G1 | S_J | S_M,
    ['L']  = S_D | S_E | S_F,
    ['M']  = S_B | S_C | S_E | S_F | S_H | S_J,
    ['N']  = S_B | S_C | S_E | S_F | S_H | S_M,
    ['O']  = S_A | S_B | S_C | S_D | S_E | S_F,
    ['P']  = S_A | S_B | S_E | S_F | S_G,
    ['Q']  = S_A | S_B | S_C | S_D | S_E | S_F | S_M,
    ['R']  = S_A | S_B | S_E | S_F | S_G | S_M,
    ['S']  = S_A | S_C | S_D | S_F | S_G,
    ['T']  = S_A | S_IL,
    ['U']  = S_B | S_C | S_D | S_E | S_F,
    ['V']  = S_E | S_F | S_J | S_K,
    ['W']  = S_B | S_C | S_E | S_F | S_K | S_M,
    ['X']  = S_HJKM,
    ['Y']  = S_H | S_J | S_L,
    ['Z']  = S_A | S_D | S_J | S_K,

    ['[']  = S_A2 | S_B | S_C | S_D2,
    ['\\'] = S_H | S_M,
    [']']  = S_A1 | S_F | S_E | S_D1,
    ['^']  = S_K | S_M,
    ['_']  = S_D,
    ['`']  = S_H,
};

/* ------------------------------------------------------------------ */
/*  Bar drawing                                                        */
/* ------------------------------------------------------------------ */

/* Trace a chevron-tipped bar from (x1,y1) to (x2,y2) with thickness @t.
 * The orthogonal case yields the same flat hexagon the seven-segment
 * Countdown bars use; the diagonal case rotates the hexagon so the LED
 * starburst look stays consistent across the whole glyph. */
static void
seg_path (cairo_t *cr,
          double   x1, double y1,
          double   x2, double y2,
          double   t)
{
    double dx = x2 - x1;
    double dy = y2 - y1;
    double L  = hypot (dx, dy);
    double ux, uy, nx, ny, h;

    if (L < 1e-6)
        return;

    ux = dx / L;
    uy = dy / L;
    nx = -uy;
    ny =  ux;
    h  = t * 0.5;

    cairo_new_sub_path (cr);
    cairo_move_to (cr, x1, y1);
    cairo_line_to (cr, x1 + ux * h - nx * h, y1 + uy * h - ny * h);
    cairo_line_to (cr, x2 - ux * h - nx * h, y2 - uy * h - ny * h);
    cairo_line_to (cr, x2, y2);
    cairo_line_to (cr, x2 - ux * h + nx * h, y2 - uy * h + ny * h);
    cairo_line_to (cr, x1 + ux * h + nx * h, y1 + uy * h + ny * h);
    cairo_close_path (cr);
}

/* Fill the current path as one lit bar — flat fill in @lit plus a faint
 * translucent stroke so the bar carries an LED halo.  Same recipe as the
 * seven-segment painter. */
static void
seg_fill_lit (cairo_t *cr, const PnColor *lit)
{
    cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, lit->alpha);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, lit->red, lit->green, lit->blue, 0.30);
    cairo_set_line_width (cr, 2.0);
    cairo_stroke (cr);
}

/* Fill the current path as one unlit bar — the dim off-state ghost. */
static void
seg_fill_dim (cairo_t *cr, const PnColor *dim)
{
    cairo_set_source_rgba (cr, dim->red, dim->green, dim->blue, dim->alpha);
    cairo_fill (cr);
}

/* ------------------------------------------------------------------ */
/*  One 16-segment cell                                                */
/* ------------------------------------------------------------------ */

/* Layout ratios, expressed against the cell width D.  H_RATIO matches
 * the seven-segment digits (a tall narrow box). */
#define S16_H_RATIO       1.70
#define S16_THICK_RATIO   0.16
#define S16_INTRA_RATIO   0.18    /* gap between adjacent cells */

/* Endpoint table for the 16 bars of one cell — each row is one segment's
 * bit-mask and the two endpoints of its bar's centre-line.  The endpoints
 * are computed lazily from the cell's geometry; this lets draw_cell()
 * paint in two passes (all unlit first, then all lit on top) so the lit
 * bars' chevrons overlap and visually cover the dim ghosts at the centre
 * cluster — the dim hexagons in the off bars never bleed through. */
typedef struct
{
    guint16 bit;
    double  x1, y1, x2, y2;
} CellSeg;

static void
fill_segments (CellSeg segs[16],
               double  x, double y, double D, double H, double t)
{
    double inset = t * 0.6;
    double xl    = x + inset;
    double xr    = x + D - inset;
    double xc    = x + D * 0.5;
    double yt    = y + inset;
    double yb    = y + H - inset;
    double ym    = y + H * 0.5;
    double g     = t * 0.35;        /* end-cap gap so adjacent bars don't merge */
    double gd    = t * 0.45;        /* slightly larger gap for diagonals */

    /* Orthogonal segments: same geometry as the seven-segment painter. */
    segs[0]  = (CellSeg){ S_A1, xl + g,        yt,            xc - g * 0.5,  yt };
    segs[1]  = (CellSeg){ S_A2, xc + g * 0.5,  yt,            xr - g,        yt };
    segs[2]  = (CellSeg){ S_B,  xr,            yt + g,        xr,            ym - g * 0.5 };
    segs[3]  = (CellSeg){ S_C,  xr,            ym + g * 0.5,  xr,            yb - g };
    segs[4]  = (CellSeg){ S_D2, xc + g * 0.5,  yb,            xr - g,        yb };
    segs[5]  = (CellSeg){ S_D1, xl + g,        yb,            xc - g * 0.5,  yb };
    segs[6]  = (CellSeg){ S_E,  xl,            ym + g * 0.5,  xl,            yb - g };
    segs[7]  = (CellSeg){ S_F,  xl,            yt + g,        xl,            ym - g * 0.5 };

    /* Middle horizontals and verticals — they share the centre with the
     * four diagonals, so each end is pulled back by g to leave room. */
    segs[8]  = (CellSeg){ S_G1, xl + g,        ym,            xc - g * 0.5,  ym };
    segs[9]  = (CellSeg){ S_G2, xc + g * 0.5,  ym,            xr - g,        ym };
    segs[10] = (CellSeg){ S_I,  xc,            yt + g,        xc,            ym - g * 0.5 };
    segs[11] = (CellSeg){ S_L,  xc,            ym + g * 0.5,  xc,            yb - g };

    /* Diagonals — each runs from a corner to the centre; the chevron tips
     * are pulled in by gd so the four meeting at the centre, and the
     * corner ones meeting the orthogonal segments, do not overlap. */
    segs[12] = (CellSeg){ S_H,  xl + gd,       yt + gd,       xc - gd * 0.7, ym - gd * 0.7 };
    segs[13] = (CellSeg){ S_J,  xr - gd,       yt + gd,       xc + gd * 0.7, ym - gd * 0.7 };
    segs[14] = (CellSeg){ S_K,  xl + gd,       yb - gd,       xc - gd * 0.7, ym + gd * 0.7 };
    segs[15] = (CellSeg){ S_M,  xr - gd,       yb - gd,       xc + gd * 0.7, ym + gd * 0.7 };
}

static void
draw_cell (cairo_t       *cr,
           double         x,
           double         y,
           double         D,
           double         H,
           double         t,
           guint16        mask,
           const PnColor *lit,
           const PnColor *dim)
{
    CellSeg segs[16];
    int     i;

    fill_segments (segs, x, y, D, H, t);

    /* Pass 1 — all unlit bars as the dim off-state ghost. */
    for (i = 0; i < 16; i++)
    {
        if ((mask & segs[i].bit) != 0)
            continue;
        seg_path (cr, segs[i].x1, segs[i].y1, segs[i].x2, segs[i].y2, t);
        seg_fill_dim (cr, dim);
    }

    /* Pass 2 — all lit bars on top, so their chevron tips cover any dim
     * ghost they meet at the centre cluster or at a corner. */
    for (i = 0; i < 16; i++)
    {
        if ((mask & segs[i].bit) == 0)
            continue;
        seg_path (cr, segs[i].x1, segs[i].y1, segs[i].x2, segs[i].y2, t);
        seg_fill_lit (cr, lit);
    }
}

/* Look up the mask for an ASCII byte.  Lowercase falls through to
 * uppercase; non-ASCII and unmapped bytes light no segments. */
static guint16
mask_for_char (gchar c)
{
    guchar u = (guchar) c;

    if (u >= 128)
        return 0;
    if (u >= 'a' && u <= 'z')
        u = (guchar) (u - 'a' + 'A');
    return glyph_masks[u];
}

/* ------------------------------------------------------------------ */
/*  Panel                                                              */
/* ------------------------------------------------------------------ */

/* Trace a rounded-rectangle path with corner radius @r — same helper as
 * the seven-segment painter; copied locally to keep this translation unit
 * self-contained (the helper is static in pn-countdown-gui.c). */
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
pn_segment16_paint_plot (PnNode  *node,
                         cairo_t *cr,
                         double   x,
                         double   y,
                         double   w,
                         double   h)
{
    PnSegment16           *self = PN_SEGMENT16 (node);
    PnSegment16PaintState  st;
    PnColor                lit, dim;
    guint                  cells;
    double                 pad = 10.0;
    double                 avail_w, avail_h;
    double                 D, H, t, gap, content_w;
    double                 x_cur, cell_y;
    const gchar           *p;
    guint                  i;

    pn_segment16_get_paint_state (self, &st);

    cells = st.cells;
    if (cells < 1)  cells = 1;
    if (cells > 32) cells = 32;

    lit = st.segment_color;
    dim = st.unlit_segment_color;

    /* ---- panel: a dark bezel framing the screen face (same look as
     *      the Countdown panel). ---- */
    rounded_rect (cr, x, y, w, h, 9.0);
    cairo_set_source_rgba (cr, 0.12, 0.12, 0.13, 1.0);
    cairo_fill (cr);

    rounded_rect (cr, x + 4.0, y + 4.0, w - 8.0, h - 8.0, 6.0);
    cairo_set_source_rgba (cr, st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill (cr);

    avail_w = w - 2.0 * pad;
    avail_h = h - 2.0 * pad;
    if (avail_w < 1.0 || avail_h < 1.0)
        return;

    /* Solve cell width D so the @cells-wide row fits both the available
     * width and the screen height.  content_w = N*D + (N-1)*gap. */
    {
        double by_width  = avail_w / ((double) cells
                                      + (double) (cells - 1) * S16_INTRA_RATIO);
        double by_height = avail_h / S16_H_RATIO;
        D = fmin (by_width, by_height);
        if (D < 4.0) D = 4.0;
    }

    H        = D * S16_H_RATIO;
    t        = D * S16_THICK_RATIO;
    gap      = D * S16_INTRA_RATIO;
    content_w = (double) cells * D + (double) (cells - 1) * gap;

    x_cur  = x + (w - content_w) * 0.5;
    cell_y = y + pad + (avail_h - H) * 0.5;

    /* Walk the text and the cell row together.  When the text runs out,
     * remaining cells render their off-state ghost; when the text is
     * longer than the row, the tail is silently dropped. */
    p = st.text;
    for (i = 0; i < cells; i++)
    {
        guint16 mask = 0;

        if (*p != '\0')
        {
            mask = mask_for_char (*p);
            p++;
        }
        draw_cell (cr, x_cur, cell_y, D, H, t, mask, &lit, &dim);
        x_cur += D + gap;
    }
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_segment16_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_SEGMENT16));

    node_class->paint_plot = pn_segment16_paint_plot;
    /* Panel has rounded corners (would leak past a rectangular drop
     * shadow) and the at-rest display is already the readable surface, so
     * a press should select / drag rather than lift a zoomed copy —
     * matching #PnCountdown. */
    node_class->paint_plot_skip_shadow = TRUE;
    node_class->paint_plot_skip_zoom   = TRUE;

    /* Class ref held for the process lifetime so the slots we just wrote
     * stay valid (one leaked ref on a singleton class, mirroring the
     * other gui-tier installers). */
}
