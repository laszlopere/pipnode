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
/*  PnTable — gui tier.                                                 */
/*                                                                     */
/*  The cairo table painter (background + frame, header band with bold  */
/*  ellipsis-fitted titles, the body rows with alternating-row stripes  */
/*  and column separators).  The node's GType, properties, receive()    */
/*  logic, the column-spec parser, the row buffer + #limit trimming and  */
/*  the scroll bookkeeping live in the GTK-free core file pn-table.c;    */
/*  this file installs the paint_plot vfunc onto that class at editor    */
/*  startup (pn_table_gui_install), reading the parsed columns + rows    */
/*  through the core's GTK-free data accessors.  The headless runtime    */
/*  never loads this file's half, so the Table logic runs without GTK.   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-table-gui.h"
#include "pn-table.h"

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_TABLE_WIDTH         280.0

#define PN_TABLE_INSET           6.0
#define PN_TABLE_HEADER_BAND_PX 22.0
#define PN_TABLE_ROW_HEIGHT_PX  18.0
#define PN_TABLE_FONT_PX        12.0

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Truncate @str in place (mutating its tail) until the
 *  cairo_text_extents width fits within @max_w.  Falls back to an
 *  empty string when even a single ellipsis doesn't fit, so an
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
        memcpy (buf, str, copy);
        buf[copy]     = '\xe2';  /* '…' = 0xE2 0x80 0xA6 */
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

        /* Step back over one (possibly multi-byte) UTF-8 char. */
        do {
            len--;
        } while (len > 0 && (((unsigned char) str[len]) & 0xC0) == 0x80);
    }

    str[0] = '\0';
}

static void
pn_table_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnTable     *self = PN_TABLE (node);
    const double scale = MIN (1.0, (w / PN_TABLE_WIDTH) * 0.6 + 0.4);
    /* Scale plays gently with overlay size:
     *   w == 280   → scale ≈ 1.00
     *   w == 560   → scale ≈ 1.00 (capped)
     * Overlay grows the row count, not the type. */
    const double font_px      = PN_TABLE_FONT_PX        * scale;
    const double row_h        = PN_TABLE_ROW_HEIGHT_PX  * scale;
    const double header_h     = PN_TABLE_HEADER_BAND_PX * scale;
    const double inset        = PN_TABLE_INSET;
    const double inner_x      = x + inset;
    const double inner_w      = w - 2 * inset;
    PnTablePaintState    ps;
    const PnTableColumn *cols;
    guint                n_cols;
    GQueue              *rows;
    guint                queue_len;
    int                  visible_rows;
    int                  max_offset;
    int                  scroll_offset;
    guint                i;
    GList               *iter;
    int                  shown;
    double               col_w;

    pn_table_get_paint_state (self, &ps);
    cols      = pn_table_peek_columns (self, &n_cols);
    rows      = pn_table_peek_rows (self);
    queue_len = g_queue_get_length (rows);

    /* Background + frame. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Clip to the rectangle so an unexpectedly long header label can
     * never spill onto adjacent nodes. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    /* No columns?  The empty rectangle plus a hint at the top is more
     * helpful than a blank canvas. */
    if (n_cols == 0)
    {
        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_ITALIC,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, font_px);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
        cairo_move_to (cr, x + inset, y + font_px + 6.0);
        cairo_show_text (cr, "no columns configured");
        cairo_restore (cr);
        return;
    }

    col_w = inner_w / (double) n_cols;

    /* Header band: solid fill + bold titles + 1 px underline. */
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

    for (i = 0; i < n_cols; i++)
    {
        const double cx = inner_x + i * col_w;
        gchar       *t  = g_strdup (cols[i].title != NULL
                                        ? cols[i].title
                                        : "");
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

    /* Body row count.  Floor so the last row is fully visible. */
    visible_rows = (int) floor ((h - header_h) / row_h);
    if (visible_rows < 0) visible_rows = 0;

    max_offset = (int) queue_len - visible_rows;
    if (max_offset < 0) max_offset = 0;
    /* The painter is the only place that knows the live extents, so it
     * clamps the core's stored offset here and reads the pinned value
     * back. */
    pn_table_clamp_scroll_offset (self, max_offset);
    scroll_offset = pn_table_get_scroll_offset (self);

    /* Body rows. */
    cairo_select_font_face (cr, "sans-serif",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, font_px);

    iter  = g_queue_peek_head_link (rows);
    /* Skip past the scroll offset.  The list is borrowed; iter advances
     * through the same nodes the queue owns. */
    for (i = 0; iter != NULL && (int) i < scroll_offset; i++)
        iter = iter->next;

    shown = 0;
    while (iter != NULL && shown < visible_rows)
    {
        PnTableRow  *row    = iter->data;
        const double row_y  = y + header_h + shown * row_h;
        guint        ci;

        /* Alternating row background — drawn before the text so it
         * doesn't paint over the cell glyphs. */
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

        for (ci = 0; ci < n_cols; ci++)
        {
            const double cx = inner_x + ci * col_w;
            gchar       *t;

            /* Older rows that were stamped before a column-spec
             * change won't have a value at this index — show the
             * em-dash placeholder so the misalignment reads as a
             * deliberate "no data here" rather than as a bug. */
            if (ci < row->n_values && row->values[ci] != NULL)
                t = g_strdup (row->values[ci]);
            else
                t = g_strdup ("\xe2\x80\x94");

            fit_text_with_ellipsis (cr, t, col_w - 6.0);
            cairo_move_to (cr, cx + 2.0, row_y + row_h * 0.72);
            cairo_show_text (cr, t);
            g_free (t);
        }

        iter = iter->next;
        shown++;
    }

    /* Vertical column separators, painted last so they sit on top of
     * the alternating-row backgrounds. */
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.grid_color);
    cairo_set_line_width (cr, 1.0);
    for (i = 1; i < n_cols; i++)
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
pn_table_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_TABLE));

    node_class->paint_plot = pn_table_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
