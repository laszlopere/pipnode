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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-table-view.h"
#include <gtk/gtk.h>
#include "pn-message.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Match PnTable / PnGraph so a row of mixed sinks aligns visually.   */
/* ------------------------------------------------------------------ */

#define PN_TABLE_VIEW_WIDTH         280.0
#define PN_TABLE_VIEW_HEADER_HEIGHT  40.0
#define PN_TABLE_VIEW_GAP             4.0
#define PN_TABLE_VIEW_BODY_HEIGHT   173.0
#define PN_TABLE_VIEW_TOTAL_HEIGHT  (PN_TABLE_VIEW_HEADER_HEIGHT + \
                                     PN_TABLE_VIEW_GAP +           \
                                     PN_TABLE_VIEW_BODY_HEIGHT)

/* 10 Hz repaint cap, identical to PnTable / PnGraph. */
#define PN_TABLE_VIEW_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

#define PN_TABLE_VIEW_INSET           6.0
#define PN_TABLE_VIEW_HEADER_BAND_PX 22.0
#define PN_TABLE_VIEW_ROW_HEIGHT_PX  18.0
#define PN_TABLE_VIEW_FONT_PX        12.0

/* ------------------------------------------------------------------ */
/*  Per-row state                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar **cells;     /* owned strings; cells[i] for column i */
    guint   n_cells;
} PnTableViewRow;

struct _PnTableView
{
    PnNode parent_instance;

    /* Visual properties (same surface PnTable exposes; pinned to
     * matching defaults so the two sinks read as siblings on the
     * canvas). */
    GdkRGBA  background_color;
    GdkRGBA  header_background_color;
    GdkRGBA  grid_color;
    GdkRGBA  text_color;
    GdkRGBA  header_text_color;
    gboolean alternate_row_background;

    /* Latest snapshot of the table, replaced on every receive. */
    gchar    **header_cells;     /* owned; NULL when the message
                                  * carried no header object */
    guint      n_header_cells;
    GPtrArray *rows;              /* owns PnTableViewRow*; freed via
                                   * row_free below */
    guint      n_cols;            /* max cells across header + rows
                                   * (drives column-band geometry) */

    /* Scroll state (zoom overlay only, like PnTable). */
    int scroll_offset;

    /* Repaint throttle. */
    gint64 last_repaint_us;
    guint  pending_repaint_id;
};

G_DEFINE_TYPE (PnTableView, pn_table_view, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_BACKGROUND_COLOR,
    PROP_HEADER_BACKGROUND_COLOR,
    PROP_GRID_COLOR,
    PROP_TEXT_COLOR,
    PROP_HEADER_TEXT_COLOR,
    PROP_ALTERNATE_ROW_BACKGROUND,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* Forward decls. */
static void schedule_repaint (PnTableView *self);

/* ------------------------------------------------------------------ */
/*  Row / header lifecycle                                             */
/* ------------------------------------------------------------------ */

static void
row_free (gpointer ptr)
{
    PnTableViewRow *row = ptr;
    guint           i;

    if (row == NULL)
        return;

    for (i = 0; i < row->n_cells; i++)
        g_free (row->cells[i]);
    g_free (row->cells);
    g_free (row);
}

static void
clear_header (PnTableView *self)
{
    guint i;
    for (i = 0; i < self->n_header_cells; i++)
        g_free (self->header_cells[i]);
    g_clear_pointer (&self->header_cells, g_free);
    self->n_header_cells = 0;
}

static void
clear_rows (PnTableView *self)
{
    if (self->rows != NULL)
        g_ptr_array_set_size (self->rows, 0);
}

/* ------------------------------------------------------------------ */
/*  JSON parsing                                                       */
/*                                                                     */
/*  Pull a `{ "cells": [...] }` object into a fresh (gchar **, guint)  */
/*  pair.  Cells produced by `PnTableModel` are JSON objects of shape  */
/*  `{ "text": "..." }` -- the wrapping object lets a future filter    */
/*  decorate a cell with per-cell properties (colour, alignment, …)    */
/*  without changing the schema.  A cell that is a bare scalar (string */
/*  / int / bool / null) is stringified directly so a hand-built       */
/*  `data.table` can feed this sink without going through Table Model. */
/* ------------------------------------------------------------------ */

static gchar *
scalar_to_display_string (JsonNode *node)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return g_strdup ("");

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vt = json_node_get_value_type (node);

        if (vt == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));

        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));

        if (vt == G_TYPE_DOUBLE)
        {
            gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd (buf, sizeof buf, "%g",
                             json_node_get_double (node));
            return g_strdup (buf);
        }

        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node)
                                 ? "true" : "false");
    }

    if (JSON_NODE_HOLDS_ARRAY (node))
        return g_strdup ("[\xe2\x80\xa6]");   /* […] */

    return g_strdup ("?");
}

static gchar *
cell_node_to_display_string (JsonNode *node)
{
    if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
    {
        JsonObject *obj = json_node_get_object (node);
        if (json_object_has_member (obj, "text"))
            return scalar_to_display_string (
                    json_object_get_member (obj, "text"));
        return g_strdup ("");
    }

    return scalar_to_display_string (node);
}

static gboolean
extract_cells (
        JsonObject  *wrapper,
        gchar     ***out_cells,
        guint       *out_n)
{
    JsonNode  *cells_node;
    JsonArray *cells_arr;
    guint      len, i;
    gchar    **arr;

    *out_cells = NULL;
    *out_n     = 0;

    if (wrapper == NULL || !json_object_has_member (wrapper, "cells"))
        return FALSE;

    cells_node = json_object_get_member (wrapper, "cells");
    if (!JSON_NODE_HOLDS_ARRAY (cells_node))
        return FALSE;

    cells_arr = json_node_get_array (cells_node);
    len       = json_array_get_length (cells_arr);
    arr       = g_new0 (gchar *, len > 0 ? len : 1);

    for (i = 0; i < len; i++)
        arr[i] = cell_node_to_display_string (
                json_array_get_element (cells_arr, i));

    *out_cells = arr;
    *out_n     = len;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle (mirrors PnTable's, kept independent so the two   */
/*  sinks don't share state)                                           */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnTableView *self = PN_TABLE_VIEW (user_data);

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnTableView *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_TABLE_VIEW_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_TABLE_VIEW_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/*                                                                     */
/*  Pull `data.table` out of the incoming message and replace the      */
/*  internal snapshot with what it carries.  A message without a       */
/*  recognisable table simply clears the view -- the canonical empty   */
/*  state -- so an upstream node that has not produced a table yet     */
/*  (or that emitted a payload of a different shape) is visible as     */
/*  "no data" rather than as a stuck previous render.                  */
/* ------------------------------------------------------------------ */

static void
pn_table_view_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTableView *self = PN_TABLE_VIEW (node);
    JsonObject  *data;
    JsonNode    *table_node;
    JsonObject  *table_obj;
    guint        max_cols = 0;

    clear_header (self);
    clear_rows   (self);
    self->scroll_offset = 0;

    data = pn_message_get_data (message);
    if (data == NULL || !json_object_has_member (data, "table"))
        goto done;

    table_node = json_object_get_member (data, "table");
    if (!JSON_NODE_HOLDS_OBJECT (table_node))
        goto done;

    table_obj = json_node_get_object (table_node);

    /* Header (optional). */
    if (json_object_has_member (table_obj, "header"))
    {
        JsonNode *hn = json_object_get_member (table_obj, "header");
        if (JSON_NODE_HOLDS_OBJECT (hn))
        {
            JsonObject *ho = json_node_get_object (hn);
            extract_cells (ho,
                           &self->header_cells,
                           &self->n_header_cells);
            if (self->n_header_cells > max_cols)
                max_cols = self->n_header_cells;
        }
    }

    /* Rows. */
    if (json_object_has_member (table_obj, "rows"))
    {
        JsonNode *rn = json_object_get_member (table_obj, "rows");
        if (JSON_NODE_HOLDS_ARRAY (rn))
        {
            JsonArray *rows_arr = json_node_get_array (rn);
            guint      n        = json_array_get_length (rows_arr);
            guint      i;

            for (i = 0; i < n; i++)
            {
                JsonNode   *row_n = json_array_get_element (rows_arr, i);
                JsonObject *row_o;
                PnTableViewRow *row;

                if (!JSON_NODE_HOLDS_OBJECT (row_n))
                    continue;

                row_o = json_node_get_object (row_n);
                row   = g_new0 (PnTableViewRow, 1);

                extract_cells (row_o, &row->cells, &row->n_cells);

                if (row->n_cells > max_cols)
                    max_cols = row->n_cells;

                g_ptr_array_add (self->rows, row);
            }
        }
    }

done:
    self->n_cols = max_cols;
    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Truncate @str in place (mutating its tail) until its
 *  cairo_text_extents width fits within @max_w.  Same UTF-8-safe
 *  algorithm PnTable uses; duplicated here so the two sinks do not
 *  share a private helper across files. */
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
    const gboolean have_header =
            (self->header_cells != NULL && self->n_header_cells > 0);
    const guint  n_rows   = (self->rows != NULL) ? self->rows->len : 0;
    int          visible_rows;
    int          max_offset;
    double       col_w;
    guint        i;
    guint        shown;

    /* Background + frame. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, &self->background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, &self->grid_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    /* Empty state: nothing received yet (or the latest message had no
     * recognisable table).  Mirror PnTable's blank-canvas hint so the
     * sink reads as "waiting for input" rather than as broken. */
    if (self->n_cols == 0)
    {
        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_ITALIC,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, font_px);
        gdk_cairo_set_source_rgba (cr, &self->grid_color);
        cairo_move_to (cr, x + inset, y + font_px + 6.0);
        cairo_show_text (cr, "waiting for table");
        cairo_restore (cr);
        return;
    }

    col_w = inner_w / (double) self->n_cols;

    /* Header band (drawn only when the latest message carried one). */
    if (have_header)
    {
        cairo_set_source_rgba (cr,
                               self->header_background_color.red,
                               self->header_background_color.green,
                               self->header_background_color.blue,
                               self->header_background_color.alpha);
        cairo_rectangle (cr, x, y, w, header_h);
        cairo_fill (cr);

        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size (cr, font_px);
        gdk_cairo_set_source_rgba (cr, &self->header_text_color);

        for (i = 0; i < self->n_cols; i++)
        {
            const double cx = inner_x + i * col_w;
            const gchar *raw = (i < self->n_header_cells &&
                                self->header_cells[i] != NULL)
                                   ? self->header_cells[i]
                                   : "";
            gchar *t = g_strdup (raw);

            fit_text_with_ellipsis (cr, t, col_w - 6.0);
            cairo_move_to (cr, cx + 2.0, y + header_h * 0.7);
            cairo_show_text (cr, t);
            g_free (t);
        }

        /* Underline below the header band. */
        gdk_cairo_set_source_rgba (cr, &self->grid_color);
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
        if (self->scroll_offset > max_offset)
            self->scroll_offset = max_offset;
        if (self->scroll_offset < 0)
            self->scroll_offset = 0;

        cairo_select_font_face (cr, "sans-serif",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, font_px);

        shown = 0;
        for (i = (guint) self->scroll_offset;
             i < n_rows && (int) shown < visible_rows;
             i++)
        {
            PnTableViewRow *row    = g_ptr_array_index (self->rows, i);
            const double    row_y  = body_top + shown * row_h;
            guint           ci;

            /* Alternating row backgrounds. */
            if (self->alternate_row_background && (shown & 1) == 1)
            {
                cairo_set_source_rgba (cr,
                                       self->header_background_color.red,
                                       self->header_background_color.green,
                                       self->header_background_color.blue,
                                       self->header_background_color.alpha * 0.30);
                cairo_rectangle (cr, x, row_y, w, row_h);
                cairo_fill (cr);
            }

            gdk_cairo_set_source_rgba (cr, &self->text_color);

            for (ci = 0; ci < self->n_cols; ci++)
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
    gdk_cairo_set_source_rgba (cr, &self->grid_color);
    cairo_set_line_width (cr, 1.0);
    for (i = 1; i < self->n_cols; i++)
    {
        const double sx = inner_x + i * col_w;
        cairo_move_to (cr, sx, y);
        cairo_line_to (cr, sx, y + h);
        cairo_stroke (cr);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Scroll                                                             */
/* ------------------------------------------------------------------ */

static void
pn_table_view_scroll (PnNode *node, double dy)
{
    PnTableView *self = PN_TABLE_VIEW (node);
    int          step = (int) lround (dy * 3.0);

    if (step == 0)
        step = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    if (step == 0)
        return;

    self->scroll_offset += step;
    if (self->scroll_offset < 0)
        self->scroll_offset = 0;

    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_table_view_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    (void) self;
    if (out_width  != NULL) *out_width  = PN_TABLE_VIEW_WIDTH;
    if (out_height != NULL) *out_height = PN_TABLE_VIEW_TOTAL_HEIGHT;
}

static double
pn_table_view_get_header_height (PnNode *self)
{
    (void) self;
    return PN_TABLE_VIEW_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_table_view_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTableView *self = PN_TABLE_VIEW (object);

    switch (prop_id)
    {
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_HEADER_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->header_background_color);
        break;
    case PROP_GRID_COLOR:
        g_value_set_boxed (value, &self->grid_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    case PROP_HEADER_TEXT_COLOR:
        g_value_set_boxed (value, &self->header_text_color);
        break;
    case PROP_ALTERNATE_ROW_BACKGROUND:
        g_value_set_boolean (value, self->alternate_row_background);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_table_view_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTableView *self = PN_TABLE_VIEW (object);

    switch (prop_id)
    {
    case PROP_BACKGROUND_COLOR:
    {
        const GdkRGBA *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_BACKGROUND_COLOR:
    {
        const GdkRGBA *c = g_value_get_boxed (value);
        if (c != NULL) self->header_background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_GRID_COLOR:
    {
        const GdkRGBA *c = g_value_get_boxed (value);
        if (c != NULL) self->grid_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const GdkRGBA *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_TEXT_COLOR:
    {
        const GdkRGBA *c = g_value_get_boxed (value);
        if (c != NULL) self->header_text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_ALTERNATE_ROW_BACKGROUND:
        self->alternate_row_background = g_value_get_boolean (value);
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_table_view_finalize (GObject *object)
{
    PnTableView *self = PN_TABLE_VIEW (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    clear_header (self);
    if (self->rows != NULL)
    {
        g_ptr_array_free (self->rows, TRUE);
        self->rows = NULL;
    }

    G_OBJECT_CLASS (pn_table_view_parent_class)->finalize (object);
}

static void
pn_table_view_class_init (PnTableViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_table_view_get_property;
    object_class->set_property = pn_table_view_set_property;
    object_class->finalize     = pn_table_view_finalize;

    node_class->receive           = pn_table_view_receive;
    node_class->get_size          = pn_table_view_get_size;
    node_class->get_header_height = pn_table_view_get_header_height;
    node_class->paint_plot        = pn_table_view_paint_plot;
    node_class->scroll            = pn_table_view_scroll;

    node_class->class_name        = "Table View";
    node_class->icon              = "\xef\x83\x8e";  /* fa-table U+F0CE */
    node_class->color             = (PnColor){ 0.27, 0.71, 0.85, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the table rectangle behind the rows",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_BACKGROUND_COLOR] = g_param_spec_boxed (
            "header-background-color", "Header background colour",
            "Fill colour of the header band (and, at 30%% alpha, the "
            "alternating-row stripe when that option is enabled)",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GRID_COLOR] = g_param_spec_boxed (
            "grid-color", "Grid colour",
            "Colour of the frame, the column separators, and the "
            "header underline",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the body cell text",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_TEXT_COLOR] = g_param_spec_boxed (
            "header-text-color", "Header text colour",
            "Colour of the bold column titles in the header band",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ALTERNATE_ROW_BACKGROUND] = g_param_spec_boolean (
            "alternate-row-background", "Alternate row background",
            "Tint every second body row with the header background "
            "colour at 30%% alpha to make wide tables easier to read",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_table_view_init (PnTableView *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  cyan = { 0.27, 0.71, 0.85, 1.0 };

    self->background_color        = (GdkRGBA) { 1.0, 1.0, 1.0, 1.0 };
    self->header_background_color = (GdkRGBA) { 0.92, 0.92, 0.92, 1.0 };
    self->grid_color              = (GdkRGBA) { 0.55, 0.55, 0.55, 1.0 };
    self->text_color              = (GdkRGBA) { 0.10, 0.10, 0.10, 1.0 };
    self->header_text_color       = (GdkRGBA) { 0.10, 0.10, 0.10, 1.0 };
    self->alternate_row_background = TRUE;

    self->header_cells       = NULL;
    self->n_header_cells     = 0;
    self->rows               = g_ptr_array_new_with_free_func (row_free);
    self->n_cols             = 0;
    self->scroll_offset      = 0;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    pn_node_set_class_name (node, "Table View");
    pn_node_set_icon       (node, "\xef\x83\x8e");  /* fa-table */
    pn_node_set_color (node, (const PnColor *)&cyan);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTableView *
pn_table_view_new (void)
{
    return g_object_new (PN_TYPE_TABLE_VIEW, NULL);
}
