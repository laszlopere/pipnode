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

#include "pn-table-model.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _PnTableModel
{
    PnNode parent_instance;

    gboolean parse_header;
};

G_DEFINE_TYPE (PnTableModel, pn_table_model, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PARSE_HEADER,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Column-position detection                                          */
/*                                                                     */
/*  Two-pass.  First, the reference line (the header, or the first     */
/*  non-empty row when parse-header is off) is scanned for every byte  */
/*  offset at which a non-space character follows a space (or starts   */
/*  the line) -- the same word-start detector the original             */
/*  implementation used.  Second, each non-zero start is shifted left  */
/*  past any character position where some data row carries a          */
/*  non-space character immediately before it, until the position just */
/*  before the start is whitespace (or past end of line) in *every*    */
/*  row.  That second pass keeps a right-aligned numeric column whose  */
/*  values are wider than the column's header (e.g. `df`'s             */
/*  `1K-blocks` header with an 11-digit value below it) from           */
/*  bleeding into the previous column when it is sliced -- the wider   */
/*  value pulls the boundary left to the position where every row is   */
/*  guaranteed to be blank.                                            */
/* ------------------------------------------------------------------ */

static gboolean
position_blank_in_all_rows (
        gsize         pos,
        const gchar **lines,
        guint         n_lines)
{
    guint i;

    for (i = 0; i < n_lines; i++)
    {
        gsize len = strlen (lines[i]);
        gchar c;

        if (pos >= len)
            continue;   /* past end of this line == blank */

        c = lines[i][pos];
        if (c != ' ' && c != '\t')
            return FALSE;
    }
    return TRUE;
}

static GArray *
detect_column_starts (
        const gchar  *reference,
        const gchar **lines,
        guint         n_lines)
{
    GArray      *starts = g_array_new (FALSE, FALSE, sizeof (gsize));
    gboolean     in_run = FALSE;
    const gchar *p;
    guint        i;

    /* Pass 1: word-start detection on the reference line. */
    for (p = reference; *p != '\0'; p++)
    {
        gboolean is_space = (*p == ' ' || *p == '\t');

        if (!is_space && !in_run)
        {
            gsize offset = (gsize) (p - reference);
            g_array_append_val (starts, offset);
            in_run = TRUE;
        }
        else if (is_space)
        {
            in_run = FALSE;
        }
    }

    /* Pass 2: adjust each non-zero start leftward until the position
     * just before it is whitespace in every row.  Capped by the
     * previous column's start so two adjacent columns can never cross. */
    for (i = 1; i < starts->len; i++)
    {
        gsize prev = g_array_index (starts, gsize, i - 1);
        gsize cur  = g_array_index (starts, gsize, i);

        while (cur > prev + 1 &&
               !position_blank_in_all_rows (cur - 1, lines, n_lines))
        {
            cur--;
        }

        g_array_index (starts, gsize, i) = cur;
    }

    return starts;
}

/* ------------------------------------------------------------------ */
/*  Cell extraction                                                    */
/*                                                                     */
/*  Slice @line at the offsets in @starts and trim each slice.  Each   */
/*  cell is emitted as a JSON object of shape `{ "text": "..." }`      */
/*  rather than as a bare string so future filters can decorate it     */
/*  with per-cell properties (colour, alignment, units, …) without     */
/*  breaking the schema.  When the line is shorter than a given start  */
/*  the missing cells render as `{ "text": "" }`, keeping every row's  */
/*  cell count identical to the header's so a downstream consumer can  */
/*  index by column without guarding against ragged rows.              */
/* ------------------------------------------------------------------ */

static JsonObject *
make_cell (const gchar *text)
{
    JsonObject *cell = json_object_new ();
    json_object_set_string_member (cell, "text", text != NULL ? text : "");
    return cell;
}

static JsonArray *
extract_cells (
        const gchar *line,
        GArray      *starts)
{
    JsonArray *cells   = json_array_new ();
    gsize      line_len = (line != NULL) ? strlen (line) : 0;
    guint      n        = starts->len;
    guint      i;

    for (i = 0; i < n; i++)
    {
        gsize start = g_array_index (starts, gsize, i);
        gsize end   = (i + 1 < n)
                          ? g_array_index (starts, gsize, i + 1)
                          : line_len;
        gchar *slice;
        gchar *trimmed;

        if (start >= line_len)
        {
            json_array_add_object_element (cells, make_cell (""));
            continue;
        }
        if (end > line_len)
            end = line_len;

        slice   = g_strndup (line + start, end - start);
        trimmed = g_strstrip (slice);
        json_array_add_object_element (cells, make_cell (trimmed));
        g_free (slice);
    }

    return cells;
}

static JsonObject *
wrap_cells (JsonArray *cells)
{
    JsonObject *row = json_object_new ();
    json_object_set_array_member (row, "cells", cells);
    return row;
}

/* ------------------------------------------------------------------ */
/*  Table build                                                        */
/* ------------------------------------------------------------------ */

static JsonNode *
build_table_from_output (
        const gchar *output,
        gboolean     parse_header)
{
    JsonObject  *table     = json_object_new ();
    JsonArray   *rows      = json_array_new ();
    gchar      **raw_lines = NULL;
    GPtrArray   *cleaned;
    GArray      *starts    = NULL;
    JsonNode    *node;
    guint        i;

    if (output != NULL)
        raw_lines = g_strsplit (output, "\n", -1);

    /* Right-strip every line and drop the empty ones up front so the
     * column detector sees only real content (no trailing g_strsplit
     * empty from a closing "\n", no blank separator lines).  The
     * cleaned strings are owned by @cleaned. */
    cleaned = g_ptr_array_new_with_free_func (g_free);
    if (raw_lines != NULL)
    {
        for (i = 0; raw_lines[i] != NULL; i++)
        {
            gchar *trimmed = g_strdup (raw_lines[i]);
            g_strchomp (trimmed);
            if (*trimmed != '\0')
                g_ptr_array_add (cleaned, trimmed);
            else
                g_free (trimmed);
        }
    }

    if (cleaned->len > 0)
    {
        const gchar  *reference = g_ptr_array_index (cleaned, 0);
        const gchar **all_lines = (const gchar **) cleaned->pdata;
        guint         start_row = parse_header ? 1u : 0u;

        starts = detect_column_starts (reference, all_lines, cleaned->len);

        if (parse_header)
        {
            JsonArray *cells = extract_cells (reference, starts);
            json_object_set_object_member (table, "header",
                                           wrap_cells (cells));
        }

        for (i = start_row; i < cleaned->len; i++)
        {
            const gchar *line  = g_ptr_array_index (cleaned, i);
            JsonArray   *cells = extract_cells (line, starts);
            json_array_add_object_element (rows, wrap_cells (cells));
        }
    }

    json_object_set_array_member (table, "rows", rows);

    if (starts != NULL)
        g_array_free (starts, TRUE);
    g_ptr_array_free (cleaned, TRUE);
    g_strfreev (raw_lines);

    node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (node, table);
    return node;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_table_model_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTableModel *self = PN_TABLE_MODEL (node);
    JsonObject   *data = pn_message_get_data (message);
    const gchar  *output = NULL;
    JsonNode     *table_node;

    if (data != NULL && json_object_has_member (data, "output"))
    {
        JsonNode *o = json_object_get_member (data, "output");
        if (JSON_NODE_HOLDS_VALUE (o) &&
            json_node_get_value_type (o) == G_TYPE_STRING)
            output = json_node_get_string (o);
    }

    table_node = build_table_from_output (output, self->parse_header);
    pn_message_set_member (message, "table", table_node);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_table_model_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTableModel *self = PN_TABLE_MODEL (object);

    switch (prop_id)
    {
    case PROP_PARSE_HEADER:
        g_value_set_boolean (value, self->parse_header);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_table_model_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTableModel *self = PN_TABLE_MODEL (object);

    switch (prop_id)
    {
    case PROP_PARSE_HEADER:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->parse_header != v)
            {
                self->parse_header = v;
                g_object_notify_by_pspec (object, props[PROP_PARSE_HEADER]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_table_model_class_init (PnTableModelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_table_model_get_property;
    object_class->set_property = pn_table_model_set_property;
    node_class->receive        = pn_table_model_receive;

    node_class->class_name     = "Table Model";
    node_class->icon           = "\xef\x83\x8e";  /* fa-table U+F0CE */
    node_class->color          = (PnColor){ 0.40, 0.66, 0.78, 1.0 };
    node_class->category       = "Filters/Reshape";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_PARSE_HEADER] = g_param_spec_boolean (
            "parse-header", "Parse header",
            "When true the first non-empty line of data.output is "
            "emitted as data.table.header.cells; when false every "
            "line including the first becomes a row",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_table_model_init (PnTableModel *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.40, 0.66, 0.78, 1.0 };

    self->parse_header = TRUE;

    pn_node_set_class_name (node, "Table Model");
    pn_node_set_icon       (node, "\xef\x83\x8e");  /* fa-table */
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTableModel *
pn_table_model_new (void)
{
    return g_object_new (PN_TYPE_TABLE_MODEL, NULL);
}
