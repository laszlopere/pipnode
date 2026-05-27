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
/*  PnMatrix57 — gui tier.                                             */
/*                                                                     */
/*  The cairo 5x7 dot-matrix LCD drawing for the Matrix57 node.  The   */
/*  node's GType, properties and receive() live in the GTK-free core   */
/*  file pn-matrix57.c; this file installs the paint_plot vfunc slot   */
/*  onto that class at editor startup (pn_matrix57_gui_install),       */
/*  reading the drawing state through pn_matrix57_get_paint_state().   */
/*  The headless runtime never loads this half.                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-matrix57-gui.h"
#include "pn-matrix57.h"

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  5x7 ASCII font                                                     */
/*                                                                     */
/*  One glyph = 5 bytes, one byte per column (left to right).  Within  */
/*  a column, bit 0 is the top row and bit 6 the bottom; bit 7 is      */
/*  unused.  The table covers the printable ASCII range 0x20..0x7E; a  */
/*  control byte or non-ASCII byte lights no dots (the cell reads as   */
/*  blank face).  This is the long-standing GLCD-style 5x7 font that   */
/*  HD44780 modules ship with, hand-typed here so we don't pull in an  */
/*  external font file.                                                */
/* ------------------------------------------------------------------ */

#define FONT_FIRST 0x20
#define FONT_LAST  0x7E

static const guint8 font5x7[FONT_LAST - FONT_FIRST + 1][5] = {
    /* 0x20 ' ' */ { 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* 0x21 '!' */ { 0x00, 0x00, 0x5F, 0x00, 0x00 },
    /* 0x22 '"' */ { 0x00, 0x07, 0x00, 0x07, 0x00 },
    /* 0x23 '#' */ { 0x14, 0x7F, 0x14, 0x7F, 0x14 },
    /* 0x24 '$' */ { 0x24, 0x2A, 0x7F, 0x2A, 0x12 },
    /* 0x25 '%' */ { 0x23, 0x13, 0x08, 0x64, 0x62 },
    /* 0x26 '&' */ { 0x36, 0x49, 0x55, 0x22, 0x50 },
    /* 0x27 '\''*/ { 0x00, 0x05, 0x07, 0x00, 0x00 },
    /* 0x28 '(' */ { 0x00, 0x1C, 0x22, 0x41, 0x00 },
    /* 0x29 ')' */ { 0x00, 0x41, 0x22, 0x1C, 0x00 },
    /* 0x2A '*' */ { 0x14, 0x08, 0x3E, 0x08, 0x14 },
    /* 0x2B '+' */ { 0x08, 0x08, 0x3E, 0x08, 0x08 },
    /* 0x2C ',' */ { 0x00, 0x50, 0x30, 0x00, 0x00 },
    /* 0x2D '-' */ { 0x08, 0x08, 0x08, 0x08, 0x08 },
    /* 0x2E '.' */ { 0x00, 0x60, 0x60, 0x00, 0x00 },
    /* 0x2F '/' */ { 0x20, 0x10, 0x08, 0x04, 0x02 },

    /* 0x30 '0' */ { 0x3E, 0x51, 0x49, 0x45, 0x3E },
    /* 0x31 '1' */ { 0x00, 0x42, 0x7F, 0x40, 0x00 },
    /* 0x32 '2' */ { 0x42, 0x61, 0x51, 0x49, 0x46 },
    /* 0x33 '3' */ { 0x21, 0x41, 0x45, 0x4B, 0x31 },
    /* 0x34 '4' */ { 0x18, 0x14, 0x12, 0x7F, 0x10 },
    /* 0x35 '5' */ { 0x27, 0x45, 0x45, 0x45, 0x39 },
    /* 0x36 '6' */ { 0x3C, 0x4A, 0x49, 0x49, 0x30 },
    /* 0x37 '7' */ { 0x01, 0x71, 0x09, 0x05, 0x03 },
    /* 0x38 '8' */ { 0x36, 0x49, 0x49, 0x49, 0x36 },
    /* 0x39 '9' */ { 0x06, 0x49, 0x49, 0x29, 0x1E },

    /* 0x3A ':' */ { 0x00, 0x36, 0x36, 0x00, 0x00 },
    /* 0x3B ';' */ { 0x00, 0x56, 0x36, 0x00, 0x00 },
    /* 0x3C '<' */ { 0x00, 0x08, 0x14, 0x22, 0x41 },
    /* 0x3D '=' */ { 0x14, 0x14, 0x14, 0x14, 0x14 },
    /* 0x3E '>' */ { 0x41, 0x22, 0x14, 0x08, 0x00 },
    /* 0x3F '?' */ { 0x02, 0x01, 0x51, 0x09, 0x06 },
    /* 0x40 '@' */ { 0x32, 0x49, 0x79, 0x41, 0x3E },

    /* 0x41 'A' */ { 0x7E, 0x11, 0x11, 0x11, 0x7E },
    /* 0x42 'B' */ { 0x7F, 0x49, 0x49, 0x49, 0x36 },
    /* 0x43 'C' */ { 0x3E, 0x41, 0x41, 0x41, 0x22 },
    /* 0x44 'D' */ { 0x7F, 0x41, 0x41, 0x22, 0x1C },
    /* 0x45 'E' */ { 0x7F, 0x49, 0x49, 0x49, 0x41 },
    /* 0x46 'F' */ { 0x7F, 0x09, 0x09, 0x01, 0x01 },
    /* 0x47 'G' */ { 0x3E, 0x41, 0x41, 0x51, 0x32 },
    /* 0x48 'H' */ { 0x7F, 0x08, 0x08, 0x08, 0x7F },
    /* 0x49 'I' */ { 0x00, 0x41, 0x7F, 0x41, 0x00 },
    /* 0x4A 'J' */ { 0x20, 0x40, 0x41, 0x3F, 0x01 },
    /* 0x4B 'K' */ { 0x7F, 0x08, 0x14, 0x22, 0x41 },
    /* 0x4C 'L' */ { 0x7F, 0x40, 0x40, 0x40, 0x40 },
    /* 0x4D 'M' */ { 0x7F, 0x02, 0x04, 0x02, 0x7F },
    /* 0x4E 'N' */ { 0x7F, 0x04, 0x08, 0x10, 0x7F },
    /* 0x4F 'O' */ { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    /* 0x50 'P' */ { 0x7F, 0x09, 0x09, 0x09, 0x06 },
    /* 0x51 'Q' */ { 0x3E, 0x41, 0x51, 0x21, 0x5E },
    /* 0x52 'R' */ { 0x7F, 0x09, 0x19, 0x29, 0x46 },
    /* 0x53 'S' */ { 0x46, 0x49, 0x49, 0x49, 0x31 },
    /* 0x54 'T' */ { 0x01, 0x01, 0x7F, 0x01, 0x01 },
    /* 0x55 'U' */ { 0x3F, 0x40, 0x40, 0x40, 0x3F },
    /* 0x56 'V' */ { 0x1F, 0x20, 0x40, 0x20, 0x1F },
    /* 0x57 'W' */ { 0x7F, 0x20, 0x18, 0x20, 0x7F },
    /* 0x58 'X' */ { 0x63, 0x14, 0x08, 0x14, 0x63 },
    /* 0x59 'Y' */ { 0x03, 0x04, 0x78, 0x04, 0x03 },
    /* 0x5A 'Z' */ { 0x61, 0x51, 0x49, 0x45, 0x43 },

    /* 0x5B '[' */ { 0x00, 0x00, 0x7F, 0x41, 0x41 },
    /* 0x5C '\\'*/ { 0x02, 0x04, 0x08, 0x10, 0x20 },
    /* 0x5D ']' */ { 0x41, 0x41, 0x7F, 0x00, 0x00 },
    /* 0x5E '^' */ { 0x04, 0x02, 0x01, 0x02, 0x04 },
    /* 0x5F '_' */ { 0x40, 0x40, 0x40, 0x40, 0x40 },
    /* 0x60 '`' */ { 0x00, 0x01, 0x02, 0x04, 0x00 },

    /* 0x61 'a' */ { 0x20, 0x54, 0x54, 0x54, 0x78 },
    /* 0x62 'b' */ { 0x7F, 0x48, 0x44, 0x44, 0x38 },
    /* 0x63 'c' */ { 0x38, 0x44, 0x44, 0x44, 0x20 },
    /* 0x64 'd' */ { 0x38, 0x44, 0x44, 0x48, 0x7F },
    /* 0x65 'e' */ { 0x38, 0x54, 0x54, 0x54, 0x18 },
    /* 0x66 'f' */ { 0x08, 0x7E, 0x09, 0x01, 0x02 },
    /* 0x67 'g' */ { 0x08, 0x14, 0x54, 0x54, 0x3C },
    /* 0x68 'h' */ { 0x7F, 0x08, 0x04, 0x04, 0x78 },
    /* 0x69 'i' */ { 0x00, 0x44, 0x7D, 0x40, 0x00 },
    /* 0x6A 'j' */ { 0x20, 0x40, 0x44, 0x3D, 0x00 },
    /* 0x6B 'k' */ { 0x00, 0x7F, 0x10, 0x28, 0x44 },
    /* 0x6C 'l' */ { 0x00, 0x41, 0x7F, 0x40, 0x00 },
    /* 0x6D 'm' */ { 0x7C, 0x04, 0x18, 0x04, 0x78 },
    /* 0x6E 'n' */ { 0x7C, 0x08, 0x04, 0x04, 0x78 },
    /* 0x6F 'o' */ { 0x38, 0x44, 0x44, 0x44, 0x38 },
    /* 0x70 'p' */ { 0x7C, 0x14, 0x14, 0x14, 0x08 },
    /* 0x71 'q' */ { 0x08, 0x14, 0x14, 0x18, 0x7C },
    /* 0x72 'r' */ { 0x7C, 0x08, 0x04, 0x04, 0x08 },
    /* 0x73 's' */ { 0x48, 0x54, 0x54, 0x54, 0x20 },
    /* 0x74 't' */ { 0x04, 0x3F, 0x44, 0x40, 0x20 },
    /* 0x75 'u' */ { 0x3C, 0x40, 0x40, 0x20, 0x7C },
    /* 0x76 'v' */ { 0x1C, 0x20, 0x40, 0x20, 0x1C },
    /* 0x77 'w' */ { 0x3C, 0x40, 0x30, 0x40, 0x3C },
    /* 0x78 'x' */ { 0x44, 0x28, 0x10, 0x28, 0x44 },
    /* 0x79 'y' */ { 0x0C, 0x50, 0x50, 0x50, 0x3C },
    /* 0x7A 'z' */ { 0x44, 0x64, 0x54, 0x4C, 0x44 },

    /* 0x7B '{' */ { 0x00, 0x08, 0x36, 0x41, 0x00 },
    /* 0x7C '|' */ { 0x00, 0x00, 0x7F, 0x00, 0x00 },
    /* 0x7D '}' */ { 0x00, 0x41, 0x36, 0x08, 0x00 },
    /* 0x7E '~' */ { 0x02, 0x01, 0x02, 0x04, 0x02 },
};

static inline const guint8 *
glyph_for_char (gchar c)
{
    guchar u = (guchar) c;
    if (u < FONT_FIRST || u > FONT_LAST)
        return NULL;
    return font5x7[u - FONT_FIRST];
}

/* ------------------------------------------------------------------ */
/*  Panel helpers                                                      */
/* ------------------------------------------------------------------ */

/* Trace a rounded-rectangle path — same helper the other readout
 * painters use, copied locally to keep this TU self-contained. */
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

/* ------------------------------------------------------------------ */
/*  Layout constants                                                   */
/*                                                                     */
/*  The character cell is 5 dot-pitches wide by 7 dot-pitches tall;    */
/*  cells are separated by one dot-pitch of LCD face.  The dot itself  */
/*  fills DOT_FILL of the pitch, leaving the rest as background grid   */
/*  so the readout has the unmistakable HD44780 dot-matrix texture.    */
/* ------------------------------------------------------------------ */

#define M57_BEZEL          6.0    /* black plastic frame thickness */
#define M57_BEZEL_RADIUS   8.0
#define M57_SCREEN_RADIUS  3.0
#define M57_SCREEN_PAD     6.0    /* face padding inside the screen */
#define M57_DOT_FILL       0.82   /* dot size as fraction of dot-pitch */

/* ------------------------------------------------------------------ */
/*  Painter                                                            */
/* ------------------------------------------------------------------ */

/* Draw one row of @cells character cells across the LCD face.  @line is
 * the substring for this row (already tailed to fit), bounded by @line_len
 * — *not* assumed to be NUL-terminated, so a multi-line walker can pass a
 * pointer into a longer "\n"-joined string. */
static void
paint_row (cairo_t *cr,
           const PnMatrix57PaintState *st,
           const gchar *line, gsize line_len,
           guint        cells,
           double       row_x, double row_y,
           double       pitch, double dot)
{
    guint  i;
    gsize  p = 0;
    int    col, rrow;

    /* Pass 1 — unlit dots, painted across the whole row in one fill so
     * the off-state ghost reads as panel texture rather than blank face. */
    cairo_set_source_rgba (cr, st->unlit_pixel_color.red,
                           st->unlit_pixel_color.green,
                           st->unlit_pixel_color.blue,
                           st->unlit_pixel_color.alpha);
    for (i = 0; i < cells; i++)
    {
        const guint8 *glyph = NULL;
        double        cell_x = row_x + (double) (6 * i) * pitch;

        if (p < line_len)
        {
            glyph = glyph_for_char (line[p]);
            p++;
        }

        for (rrow = 0; rrow < 7; rrow++)
        {
            for (col = 0; col < 5; col++)
            {
                gboolean lit = (glyph != NULL)
                               && ((glyph[col] >> rrow) & 1u);
                double   px, py;
                if (lit) continue;
                px = cell_x + (double) col * pitch + (pitch - dot) * 0.5;
                py = row_y  + (double) rrow * pitch + (pitch - dot) * 0.5;
                cairo_rectangle (cr, px, py, dot, dot);
            }
        }
    }
    cairo_fill (cr);

    /* Pass 2 — lit dots on top. */
    cairo_set_source_rgba (cr, st->pixel_color.red,
                           st->pixel_color.green,
                           st->pixel_color.blue,
                           st->pixel_color.alpha);
    p = 0;
    for (i = 0; i < cells; i++)
    {
        const guint8 *glyph = NULL;
        double        cell_x = row_x + (double) (6 * i) * pitch;

        if (p < line_len)
        {
            glyph = glyph_for_char (line[p]);
            p++;
        }
        if (glyph == NULL)
            continue;

        for (rrow = 0; rrow < 7; rrow++)
        {
            for (col = 0; col < 5; col++)
            {
                gboolean lit = ((glyph[col] >> rrow) & 1u) != 0u;
                double   px, py;
                if (!lit) continue;
                px = cell_x + (double) col * pitch + (pitch - dot) * 0.5;
                py = row_y  + (double) rrow * pitch + (pitch - dot) * 0.5;
                cairo_rectangle (cr, px, py, dot, dot);
            }
        }
    }
    cairo_fill (cr);
}

static void
pn_matrix57_paint_plot (PnNode  *node,
                        cairo_t *cr,
                        double   x,
                        double   y,
                        double   w,
                        double   h)
{
    PnMatrix57           *self = PN_MATRIX57 (node);
    PnMatrix57PaintState  st;
    guint                 cells;
    gint                  lines;
    double                screen_x, screen_y, screen_w, screen_h;
    double                avail_w, avail_h;
    double                pitch, dot;
    double                content_w, content_h;
    double                row_x, row_y0;
    const gchar          *p;
    gint                  ln;

    pn_matrix57_get_paint_state (self, &st);

    cells = st.cells;
    if (cells < 1)  cells = 1;
    if (cells > 40) cells = 40;
    lines = st.lines;
    if (lines < 1) lines = 1;
    if (lines > 2) lines = 2;

    /* ---- panel: a chunky bezel framing the LCD face. ---- */
    rounded_rect (cr, x, y, w, h, M57_BEZEL_RADIUS);
    cairo_set_source_rgba (cr, st.frame_color.red,
                           st.frame_color.green,
                           st.frame_color.blue,
                           st.frame_color.alpha);
    cairo_fill (cr);

    screen_x = x + M57_BEZEL;
    screen_y = y + M57_BEZEL;
    screen_w = w - 2.0 * M57_BEZEL;
    screen_h = h - 2.0 * M57_BEZEL;
    if (screen_w < 4.0 || screen_h < 4.0)
        return;

    rounded_rect (cr, screen_x, screen_y, screen_w, screen_h,
                  M57_SCREEN_RADIUS);
    cairo_set_source_rgba (cr, st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill (cr);

    avail_w = screen_w - 2.0 * M57_SCREEN_PAD;
    avail_h = screen_h - 2.0 * M57_SCREEN_PAD;
    if (avail_w < 1.0 || avail_h < 1.0)
        return;

    /* Solve dot-pitch so the rows fit both axes.  Horizontally a row of N
     * cells spans (6N - 1) pitches; vertically @lines stacked rows span
     * (8*lines - 1) pitches (7 per row + 1 pitch of LCD face between),
     * matching HD44780 line spacing. */
    {
        double by_width  = avail_w / (double) (6 * cells - 1);
        double by_height = avail_h / (double) (8 * lines - 1);
        pitch = fmin (by_width, by_height);
        if (pitch < 1.5) pitch = 1.5;
    }
    dot = pitch * M57_DOT_FILL;

    content_w = (double) (6 * cells - 1) * pitch;
    content_h = (double) (8 * lines - 1) * pitch;

    row_x  = screen_x + M57_SCREEN_PAD + (avail_w - content_w) * 0.5;
    row_y0 = screen_y + M57_SCREEN_PAD + (avail_h - content_h) * 0.5;

    /* Walk @text line by line, painting each row.  The paint state is
     * already tailed to the last @lines rows in the core; missing rows
     * render as all-off dots, an over-long line is cropped on the right
     * — same convention as Segment16's single-row truncation. */
    p = st.text;
    for (ln = 0; ln < lines; ln++)
    {
        const gchar *eol;
        gsize        len;
        double       row_y = row_y0 + (double) (8 * ln) * pitch;

        if (*p == '\0')
        {
            paint_row (cr, &st, "", 0, cells, row_x, row_y, pitch, dot);
            continue;
        }

        eol = strchr (p, '\n');
        len = (eol != NULL) ? (gsize) (eol - p) : strlen (p);
        paint_row (cr, &st, p, len, cells, row_x, row_y, pitch, dot);
        p = (eol != NULL) ? eol + 1 : p + len;
    }
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_matrix57_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_MATRIX57));

    node_class->paint_plot = pn_matrix57_paint_plot;
    /* The plot is a rounded plastic bezel; declare the corner radius so
     * the worksheet's drop shadow under the client area follows the
     * silhouette instead of leaking out around its corners.  The at-rest
     * display is already the readable surface, so a press should select
     * / drag rather than lift a zoomed copy — matching #PnSegment16 and
     * #PnCountdown. */
    node_class->paint_plot_corner_radius = M57_BEZEL_RADIUS;
    node_class->paint_plot_skip_zoom     = TRUE;

    /* Class ref held for the process lifetime so the slots we just
     * wrote stay valid (one leaked ref on a singleton class, mirroring
     * the other gui-tier installers). */
}
