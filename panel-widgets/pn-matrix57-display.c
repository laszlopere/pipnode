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
/*  PnMatrix57Display — a panel-sized 5x7 dot-matrix LCD               */
/*                                                                     */
/*  A miniature of the Matrix57 node's client area: black plastic      */
/*  bezel, greenish-yellow LCD face, and a row of N 5x7 character      */
/*  cells drawn as dark square pixels.  Fed plain text, it lights the  */
/*  dots from the same 5x7 ASCII font the worksheet node uses (the     */
/*  table is restated here so panel-widgets stays free of pipnode      */
/*  headers — keep the two copies in sync; lib/pn-matrix57-gui.c is    */
/*  the master).                                                       */
/* ------------------------------------------------------------------ */

#include "pn-matrix57-display.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  5x7 ASCII font (copy of lib/pn-matrix57-gui.c's table)             */
/*                                                                     */
/*  One glyph = 5 bytes, one byte per column (left to right).  Within  */
/*  a column, bit 0 is the top row and bit 6 the bottom; bit 7 unused. */
/*  Covers printable ASCII 0x20..0x7E; other bytes light no dots.      */
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
/*  Layout constants                                                   */
/*                                                                     */
/*  The panel widget is a scaled-down replica of the worksheet node's  */
/*  LCD body — same bezel and screen-pad ratios, same fixed external   */
/*  aspect (380 × 90, the worksheet body's PN_M57_WIDTH /              */
/*  PN_M57_BODY_HEIGHT).  Width is purely a function of @height, so    */
/*  changing @cells does NOT widen the widget; it shrinks the dot      */
/*  pitch within the same external rectangle, exactly the way the      */
/*  worksheet painter does.  The dot-pitch math is identical: a cell   */
/*  is 5 pitches wide, cells are separated by one pitch, so a row of   */
/*  N cells spans (6N - 1) pitches.                                    */
/* ------------------------------------------------------------------ */

/* 6/90 — bezel thickness and screen padding are each 1/15 of the     */
/* overall height, mirroring the worksheet's 6 px on a 90 px body.     */
#define M57_BEZEL_RATIO     (6.0 / 90.0)
#define M57_SCREEN_PAD_RATIO (6.0 / 90.0)
#define M57_BEZEL_MIN       1.0
#define M57_SCREEN_PAD_MIN  1.0
#define M57_SCREEN_RADIUS   2.0
#define M57_BEZEL_RADIUS    4.0
#define M57_DOT_FILL        0.82
/* External aspect of the LCD body, matching the worksheet node.       */
#define M57_BODY_ASPECT     (380.0 / 90.0)

#define M57_DEFAULT_CELLS   16
#define M57_DEFAULT_HEIGHT  24

struct _PnMatrix57Display
{
    GtkDrawingArea parent_instance;

    gchar  *text;        /* never %NULL; "" blanks the row */
    guint   cells;       /* visible cell count (1..40)     */
    gint    height;      /* requested overall pixel height */

    gdouble bg_r,   bg_g,   bg_b;     /* LCD face            */
    gdouble lit_r,  lit_g,  lit_b;    /* dark dot            */
    gdouble dim_r,  dim_g,  dim_b;    /* off-state ghost     */
};

G_DEFINE_TYPE (PnMatrix57Display, pn_matrix57_display, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

static inline double
bezel_for_height (gint height)
{
    double b = (double) height * M57_BEZEL_RATIO;
    return b < M57_BEZEL_MIN ? M57_BEZEL_MIN : b;
}

static inline double
screen_pad_for_height (gint height)
{
    double p = (double) height * M57_SCREEN_PAD_RATIO;
    return p < M57_SCREEN_PAD_MIN ? M57_SCREEN_PAD_MIN : p;
}

/* Preferred widget width — purely a function of @height, the same fixed
 * aspect the worksheet body has.  @cells does not enter the formula: a
 * wider row shrinks the dot pitch inside the same external rectangle,
 * matching the worksheet painter rather than the widget growing wider. */
static gint
preferred_width (PnMatrix57Display *self)
{
    return (gint) ceil ((double) self->height * M57_BODY_ASPECT);
}

/* ------------------------------------------------------------------ */
/*  Cairo helpers                                                      */
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

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */

static gboolean
pn_matrix57_display_draw (GtkWidget *widget, cairo_t *cr)
{
    PnMatrix57Display *self = PN_MATRIX57_DISPLAY (widget);
    GtkAllocation      alloc;
    double             bezel, pad;
    double             screen_x, screen_y, screen_w, screen_h;
    double             avail_w, avail_h;
    double             pitch_by_w, pitch_by_h, pitch, dot;
    double             content_w, content_h;
    double             row_x, row_y;
    const gchar       *p;
    guint              i;
    int                col, rrow;

    gtk_widget_get_allocation (widget, &alloc);
    if (alloc.width < 4 || alloc.height < 4)
        return FALSE;

    bezel = bezel_for_height (alloc.height);
    pad   = screen_pad_for_height (alloc.height);

    /* Bezel — chunky black plastic frame, vertically centred so the
     * widget reads the same when the panel allocates extra row space. */
    {
        double bezel_w = (double) alloc.width;
        double bezel_h = (double) alloc.height;
        rounded_rect (cr, 0.0, 0.0, bezel_w, bezel_h, M57_BEZEL_RADIUS);
        cairo_set_source_rgba (cr, 0.08, 0.08, 0.09, 1.0);
        cairo_fill (cr);
    }

    screen_x = bezel;
    screen_y = bezel;
    screen_w = (double) alloc.width  - 2.0 * bezel;
    screen_h = (double) alloc.height - 2.0 * bezel;
    if (screen_w < 4.0 || screen_h < 4.0)
        return FALSE;

    rounded_rect (cr, screen_x, screen_y, screen_w, screen_h,
                  M57_SCREEN_RADIUS);
    cairo_set_source_rgba (cr, self->bg_r, self->bg_g, self->bg_b, 1.0);
    cairo_fill (cr);

    avail_w = screen_w - 2.0 * pad;
    avail_h = screen_h - 2.0 * pad;
    if (avail_w < 1.0 || avail_h < 1.0)
        return FALSE;

    /* Solve dot-pitch so the row fits both axes, as in the worksheet
     * painter.  A row of N cells spans (6N - 1) pitches horizontally. */
    pitch_by_w = avail_w / (double) (6 * (gint) self->cells - 1);
    pitch_by_h = avail_h / 7.0;
    pitch      = fmin (pitch_by_w, pitch_by_h);
    if (pitch < 1.0) pitch = 1.0;
    dot        = pitch * M57_DOT_FILL;

    content_w = (double) (6 * (gint) self->cells - 1) * pitch;
    content_h = 7.0 * pitch;

    row_x = screen_x + pad + (avail_w - content_w) * 0.5;
    row_y = screen_y + pad + (avail_h - content_h) * 0.5;

    /* Walk text and cells together.  When the text runs out, remaining
     * cells render as all-off dots; when the text is longer than the
     * row, the tail is silently dropped. */
    p = self->text != NULL ? self->text : "";
    for (i = 0; i < self->cells; i++)
    {
        const guint8 *glyph = NULL;
        double        cell_x = row_x + (double) (6u * i) * pitch;

        if (*p != '\0')
        {
            glyph = glyph_for_char (*p);
            p++;
        }

        /* Pass 1 — unlit dots so the off-state ghost reads as panel
         * texture rather than blank face. */
        cairo_set_source_rgba (cr, self->dim_r, self->dim_g, self->dim_b, 1.0);
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
        cairo_fill (cr);

        /* Pass 2 — lit dots on top. */
        if (glyph == NULL)
            continue;
        cairo_set_source_rgba (cr, self->lit_r, self->lit_g, self->lit_b, 1.0);
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
        cairo_fill (cr);
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_display_get_preferred_width (GtkWidget *widget,
                                         gint      *minimum,
                                         gint      *natural)
{
    PnMatrix57Display *self = PN_MATRIX57_DISPLAY (widget);
    gint               w    = preferred_width (self);

    *minimum = *natural = w;
}

static void
pn_matrix57_display_get_preferred_height (GtkWidget *widget,
                                          gint      *minimum,
                                          gint      *natural)
{
    PnMatrix57Display *self = PN_MATRIX57_DISPLAY (widget);

    *minimum = *natural = self->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_display_finalize (GObject *object)
{
    PnMatrix57Display *self = PN_MATRIX57_DISPLAY (object);

    g_free (self->text);
    G_OBJECT_CLASS (pn_matrix57_display_parent_class)->finalize (object);
}

static void
pn_matrix57_display_class_init (PnMatrix57DisplayClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize             = pn_matrix57_display_finalize;
    widget_class->draw                 = pn_matrix57_display_draw;
    widget_class->get_preferred_width  = pn_matrix57_display_get_preferred_width;
    widget_class->get_preferred_height = pn_matrix57_display_get_preferred_height;
}

static void
pn_matrix57_display_init (PnMatrix57Display *self)
{
    self->text   = NULL;
    self->cells  = M57_DEFAULT_CELLS;
    self->height = M57_DEFAULT_HEIGHT;

    /* Greenish-yellow LCD face, dark dots, faint ghost — same defaults
     * the worksheet node ships. */
    self->bg_r = 0.72; self->bg_g = 0.80; self->bg_b = 0.28;
    self->lit_r = 0.06; self->lit_g = 0.08; self->lit_b = 0.04;
    self->dim_r = 0.66; self->dim_g = 0.74; self->dim_b = 0.26;

    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_matrix57_display_new (void)
{
    return g_object_new (PN_TYPE_MATRIX57_DISPLAY, NULL);
}

void
pn_matrix57_display_set_text (PnMatrix57Display *self, const gchar *text)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));

    if (g_strcmp0 (self->text, text) == 0)
        return;

    g_free (self->text);
    self->text = (text != NULL) ? g_strdup (text) : NULL;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_matrix57_display_set_cells (PnMatrix57Display *self, guint cells)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));

    if (cells < 1)  cells = 1;
    if (cells > 40) cells = 40;
    if (cells == self->cells)
        return;

    self->cells = cells;
    gtk_widget_queue_resize (GTK_WIDGET (self));  /* width changes */
}

static gboolean
set_rgb (gdouble *r, gdouble *g, gdouble *b,
         gdouble  nr, gdouble  ng, gdouble  nb)
{
    if (*r == nr && *g == ng && *b == nb)
        return FALSE;
    *r = nr; *g = ng; *b = nb;
    return TRUE;
}

void
pn_matrix57_display_set_background_color (PnMatrix57Display *self,
                                          gdouble r, gdouble g, gdouble b)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));
    if (set_rgb (&self->bg_r, &self->bg_g, &self->bg_b, r, g, b))
        gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_matrix57_display_set_pixel_color (PnMatrix57Display *self,
                                     gdouble r, gdouble g, gdouble b)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));
    if (set_rgb (&self->lit_r, &self->lit_g, &self->lit_b, r, g, b))
        gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_matrix57_display_set_unlit_pixel_color (PnMatrix57Display *self,
                                           gdouble r, gdouble g, gdouble b)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));
    if (set_rgb (&self->dim_r, &self->dim_g, &self->dim_b, r, g, b))
        gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_matrix57_display_set_height (PnMatrix57Display *self, gint height)
{
    g_return_if_fail (PN_IS_MATRIX57_DISPLAY (self));

    if (height < 8)
        height = 8;
    if (height == self->height)
        return;

    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}
