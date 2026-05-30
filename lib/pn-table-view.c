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
/*  PnTableView — core (GTK-free) half.                                 */
/*                                                                     */
/*  The node's GType, properties, receive() logic (pull `data.table`    */
/*  out of the incoming message and replace the displayed snapshot),    */
/*  the JSON cell parser, the repaint throttle and the scroll           */
/*  bookkeeping.  The cairo painter lives in the gui-tier file          */
/*  pn-table-view-gui.c, which installs the paint_plot vfunc onto this   */
/*  class at editor startup (pn_table_view_gui_install) and reads the    */
/*  snapshot through the GTK-free accessors published in the header.     */
/*  The headless runtime never loads that half, so the Table View logic  */
/*  runs without GTK (TODO #23, Phase 5).                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-table-view.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Match PnTable / PnGraph so a row of mixed sinks aligns visually.    */
/*  The painter-side constants (inset, band / row / font px) live in    */
/*  the gui file; the core only needs the node's footprint and the      */
/*  header band height for layout + the repaint cap.                    */
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

struct _PnTableView
{
    PnNode parent_instance;

    /* Visual properties (same surface PnTable exposes; pinned to
     * matching defaults so the two sinks read as siblings on the
     * canvas). */
    PnColor  background_color;
    PnColor  header_background_color;
    PnColor  grid_color;
    PnColor  text_color;
    PnColor  header_text_color;
    gboolean alternate_row_background;

    /* Latest snapshot of the table, replaced on every receive. */
    gchar    **header_cells;     /* owned; NULL when the message
                                  * carried no header object */
    guint      n_header_cells;
    GPtrArray *rows;             /* owns PnTableViewRow*; freed via
                                  * row_free below */
    guint      n_cols;           /* max cells across header + rows
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
    /* Clamping to the live extents happens in the painter (it has the
     * dimensions to compute the maximum), so this just nudges and lets
     * the next paint pin the value. */
    if (self->scroll_offset < 0)
        self->scroll_offset = 0;

    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The gui-tier cairo painter (pn-table-view-gui.c) reads the scalar   */
/*  drawing config + the live column count through a snapshot, and the  */
/*  header cells + row buffer through borrowed-pointer accessors.       */
/*  Scroll clamping crosses back the other way: the painter knows the   */
/*  live extents, so it asks the core to pin the stored offset before   */
/*  reading it.                                                         */
/* ------------------------------------------------------------------ */

void
pn_table_view_get_paint_state (PnTableView           *self,
                               PnTableViewPaintState *out)
{
    g_return_if_fail (PN_IS_TABLE_VIEW (self));
    g_return_if_fail (out != NULL);

    out->background_color         = self->background_color;
    out->header_background_color  = self->header_background_color;
    out->grid_color               = self->grid_color;
    out->text_color               = self->text_color;
    out->header_text_color        = self->header_text_color;
    out->alternate_row_background = self->alternate_row_background;
    out->n_cols                   = self->n_cols;
}

const gchar * const *
pn_table_view_peek_header (PnTableView *self, guint *n_out)
{
    g_return_val_if_fail (PN_IS_TABLE_VIEW (self), NULL);
    if (n_out != NULL)
        *n_out = self->n_header_cells;
    return (const gchar * const *) self->header_cells;
}

GPtrArray *
pn_table_view_peek_rows (PnTableView *self)
{
    g_return_val_if_fail (PN_IS_TABLE_VIEW (self), NULL);
    return self->rows;
}

int
pn_table_view_get_scroll_offset (PnTableView *self)
{
    g_return_val_if_fail (PN_IS_TABLE_VIEW (self), 0);
    return self->scroll_offset;
}

void
pn_table_view_clamp_scroll_offset (PnTableView *self, int max_offset)
{
    g_return_if_fail (PN_IS_TABLE_VIEW (self));
    if (max_offset < 0) max_offset = 0;
    if (self->scroll_offset > max_offset) self->scroll_offset = max_offset;
    if (self->scroll_offset < 0)          self->scroll_offset = 0;
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
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->header_background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_GRID_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->grid_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
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
    node_class->scroll            = pn_table_view_scroll;
    /* node_class->paint_plot is installed by the gui tier
     * (pn_table_view_gui_install); it stays NULL in a headless run. */

    node_class->class_name        = "Table View";
    node_class->icon              = "\xef\x83\x8e";  /* fa-table U+F0CE */
    node_class->color             = (PnColor){ 0.27, 0.71, 0.85, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the table rectangle behind the rows",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_BACKGROUND_COLOR] = g_param_spec_boxed (
            "header-background-color", "Header background colour",
            "Fill colour of the header band (and, at 30%% alpha, the "
            "alternating-row stripe when that option is enabled)",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GRID_COLOR] = g_param_spec_boxed (
            "grid-color", "Grid colour",
            "Colour of the frame, the column separators, and the "
            "header underline",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the body cell text",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_TEXT_COLOR] = g_param_spec_boxed (
            "header-text-color", "Header text colour",
            "Colour of the bold column titles in the header band",
            PN_TYPE_COLOR,
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
    PnColor  cyan = { 0.27, 0.71, 0.85, 1.0 };

    self->background_color        = (PnColor) { 1.0, 1.0, 1.0, 1.0 };
    self->header_background_color = (PnColor) { 0.92, 0.92, 0.92, 1.0 };
    self->grid_color              = (PnColor) { 0.55, 0.55, 0.55, 1.0 };
    self->text_color              = (PnColor) { 0.10, 0.10, 0.10, 1.0 };
    self->header_text_color       = (PnColor) { 0.10, 0.10, 0.10, 1.0 };
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
    pn_node_set_color      (node, &cyan);
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
