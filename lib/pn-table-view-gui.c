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
/*  PnTableView — gui tier.                                             */
/*                                                                     */
/*  The cairo painter that renders the latest received table (background */
/*  + frame, optional header band with bold ellipsis-fitted titles, the  */
/*  body rows with alternating-row stripes and column separators).  The  */
/*  node's GType, properties, receive() logic, the JSON cell parser, the  */
/*  snapshot + repaint throttle and the scroll bookkeeping live in the    */
/*  GTK-free core file pn-table-view.c; this file installs the paint_plot  */
/*  vfunc onto that class at editor startup (pn_table_view_gui_install),   */
/*  reading the snapshot through the core's GTK-free accessors.  The       */
/*  headless runtime never loads this file's half, so the Table View       */
/*  logic runs without GTK.                                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-table-view-gui.h"
#include "pn-table-view.h"

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_TABLE_VIEW_WIDTH         280.0

#define PN_TABLE_VIEW_INSET           6.0
#define PN_TABLE_VIEW_HEADER_BAND_PX 22.0
#define PN_TABLE_VIEW_ROW_HEIGHT_PX  18.0
#define PN_TABLE_VIEW_FONT_PX        12.0

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Truncate @str in place (mutating its tail) until its
 *  cairo_text_extents width fits within @max_w.  UTF-8-safe; falls back
 *  to an empty string when even a single ellipsis doesn't fit, so an
 *  ultra-narrow column never produces text spilling into its
 *  neighbour. */
static void
fit_text_with_ellipsis (
        cairo_t *cr,
        gchar   *str,
        double   max_w)
{
    cairo_text_extents_t ext;
    gsize                len;

    if (str == NULL || *str == '\0')
        return;

    cairo_text_extents (cr, str, &ext);
    if (ext.width <= max_w)
        return;

    len = strlen (str);
    while (len > 0)
    {
        gchar buf[256];
        gsize copy = (len < sizeof buf - 4) ? len : (sizeof buf - 4);

        /* Align the cut down to a UTF-8 character boundary so the
         * ellipsis never follows a partial code point (the initial
         * 252-byte cut above is not codepoint-aligned). */
        while (copy > 0 && (((unsigned char) str[copy]) & 0xC0) == 0x80)
            copy--;

        memcpy (buf, str, copy);
        buf[copy]     = '\xe2';   /* '…' = E2 80 A6 */
        buf[copy + 1] = '\x80';
        buf[copy + 2] = '\xa6';
        buf[copy + 3] = '\0';

        cairo_text_extents (cr, buf, &ext);
        if (ext.width <= max_w)
        {
            memcpy (str, buf, copy + 3);
            str[copy + 3] = '\0';
            return;
        }

        do {
            len--;
        } while (len > 0 && (((unsigned char) str[len]) & 0xC0) == 0x80);
    }

    str[0] = '\0';
}

static void
pn_table_view_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnTableView *self     = PN_TABLE_VIEW (node);
    const double scale    = MIN (1.0,
                                 (w / PN_TABLE_VIEW_WIDTH) * 0.6 + 0.4);
    const double font_px  = PN_TABLE_VIEW_FONT_PX        * scale;
    const double row_h    = PN_TABLE_VIEW_ROW_HEIGHT_PX  * scale;
    const double header_h = PN_TABLE_VIEW_HEADER_BAND_PX * scale;
    const double inset    = PN_TABLE_VIEW_INSET;
    const double inner_x  = x + inset;
    const double inner_w  = w - 2 * inset;
    PnTableViewPaintState ps;
    const gchar * const  *header_cells;
    guint                 n_header_cells;
    gboolean              have_header;
    GPtrArray            *rows;
    guint                 n_rows;
    int                   visible_rows;
    int                   max_offset;
    int                   scroll_offset;
    double                col_w;
    guint                 i;
    guint                 shown;

    pn_table_view_get_paint_state (self, &ps);
    header_cells = pn_table_view_peek_header (self, &n_header_cells);
    rows         = pn_table_view_peek_rows (self);
    n_rows       = (rows != NULL) ? rows->len : 0;
    have_header  = (header_cells != NULL && n_header_cells > 0);

    /* Background + frame. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    /* Empty state: nothing received yet (or the latest message had no
     * recognisable table).  Mirror PnTable's blank-canvas hint so the
     * sink reads as "waiting for input" rather than as broken. */
    if (ps.n_cols == 0)
    {
        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_ITALIC,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, font_px);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
        cairo_move_to (cr, x + inset, y + font_px + 6.0);
        cairo_show_text (cr, "waiting for table");
        cairo_restore (cr);
        return;
    }

    col_w = inner_w / (double) ps.n_cols;

    /* Header band (drawn only when the latest message carried one). */
    if (have_header)
    {
        cairo_set_source_rgba (cr,
                               ps.header_background_color.red,
                               ps.header_background_color.green,
                               ps.header_background_color.blue,
                               ps.header_background_color.alpha);
        cairo_rectangle (cr, x, y, w, header_h);
        cairo_fill (cr);

        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size (cr, font_px);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.header_text_color);

        for (i = 0; i < ps.n_cols; i++)
        {
            const double cx = inner_x + i * col_w;
            const gchar *raw = (i < n_header_cells &&
                                header_cells[i] != NULL)
                                   ? header_cells[i]
                                   : "";
            gchar *t = g_strdup (raw);

            fit_text_with_ellipsis (cr, t, col_w - 6.0);
            cairo_move_to (cr, cx + 2.0, y + header_h * 0.7);
            cairo_show_text (cr, t);
            g_free (t);
        }

        /* Underline below the header band. */
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
        cairo_set_line_width (cr, 1.0);
        cairo_move_to (cr, x,     y + header_h);
        cairo_line_to (cr, x + w, y + header_h);
        cairo_stroke (cr);
    }

    /* Body row count.  When the message had no header, the band's
     * vertical real estate goes to rows instead. */
    {
        double body_top = have_header ? (y + header_h) : y;
        double body_h   = (y + h) - body_top;

        visible_rows = (int) floor (body_h / row_h);
        if (visible_rows < 0) visible_rows = 0;

        max_offset = (int) n_rows - visible_rows;
        if (max_offset < 0) max_offset = 0;
        /* The painter is the only place that knows the live extents, so
         * it clamps the core's stored offset here and reads it back. */
        pn_table_view_clamp_scroll_offset (self, max_offset);
        scroll_offset = pn_table_view_get_scroll_offset (self);

        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, font_px);

        shown = 0;
        for (i = (guint) scroll_offset;
             i < n_rows && (int) shown < visible_rows;
             i++)
        {
            PnTableViewRow *row    = g_ptr_array_index (rows, i);
            const double    row_y  = body_top + shown * row_h;
            guint           ci;

            /* Alternating row backgrounds. */
            if (ps.alternate_row_background && (shown & 1) == 1)
            {
                cairo_set_source_rgba (cr,
                                       ps.header_background_color.red,
                                       ps.header_background_color.green,
                                       ps.header_background_color.blue,
                                       ps.header_background_color.alpha * 0.30);
                cairo_rectangle (cr, x, row_y, w, row_h);
                cairo_fill (cr);
            }

            gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);

            for (ci = 0; ci < ps.n_cols; ci++)
            {
                const double cx = inner_x + ci * col_w;
                gchar       *t;

                if (ci < row->n_cells && row->cells[ci] != NULL)
                    t = g_strdup (row->cells[ci]);
                else
                    t = g_strdup ("");

                fit_text_with_ellipsis (cr, t, col_w - 6.0);
                cairo_move_to (cr, cx + 2.0, row_y + row_h * 0.72);
                cairo_show_text (cr, t);
                g_free (t);
            }

            shown++;
        }
    }

    /* Vertical column separators on top of the alternating backgrounds. */
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
    cairo_set_line_width (cr, 1.0);
    for (i = 1; i < ps.n_cols; i++)
    {
        const double sx = inner_x + i * col_w;
        cairo_move_to (cr, sx, y);
        cairo_line_to (cr, sx, y + h);
        cairo_stroke (cr);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_table_view_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_TABLE_VIEW));

    node_class->paint_plot = pn_table_view_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
