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

/* Unit tests for PnTableModel: slice the whitespace-aligned text in
 * data.output into a structured data.table of { header?, rows }, where
 * each cell is an object { "text": ... }.  The message is forwarded with
 * the table grafted on.  Inputs here are deliberately column-aligned so
 * the fixed-width column detector produces a stable split. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-table-model.h"

#include <json-glib/json-glib.h>

static PnNode *
make_node (gboolean parse_header, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_TABLE_MODEL,
                                 "parse-header", parse_header, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Run @output (NULL for "no output member") through @node and return the
 * forwarded message; the caller owns the returned reference. */
static PnMessage *
run (PnNode *node, const gchar *output)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    if (output != NULL)
        pn_message_set_string (msg, "output", output);

    pn_node_receive_message (node, msg);
    return msg;
}

static JsonObject *
table_of (PnMessage *msg)
{
    JsonNode *n = pn_message_get_member (msg, "table");
    return (n != NULL && JSON_NODE_HOLDS_OBJECT (n))
           ? json_node_get_object (n) : NULL;
}

static const gchar *
cell_text (JsonArray *cells, guint i)
{
    JsonObject *cell = json_array_get_object_element (cells, i);
    return json_object_get_string_member (cell, "text");
}

static JsonArray *
row_cells (JsonArray *rows, guint i)
{
    JsonObject *row = json_array_get_object_element (rows, i);
    return json_object_get_array_member (row, "cells");
}

static void
test_no_output_empty_table (void)
{
    guint      emits;
    PnNode    *node  = make_node (TRUE, &emits);
    PnMessage *msg   = run (node, NULL);
    JsonObject *table = table_of (msg);
    JsonArray  *rows;

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK (table != NULL);
    if (table != NULL)
    {
        rows = json_object_get_array_member (table, "rows");
        PN_CHECK_CMPINT (json_array_get_length (rows), ==, 0);
        /* With nothing to parse there is no header either. */
        PN_CHECK_FALSE (json_object_has_member (table, "header"));
    }

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_parse_header_splits_columns (void)
{
    guint      emits;
    PnNode    *node  = make_node (TRUE, &emits);   /* the default */
    PnMessage *msg   = run (node, "aa bb\ncc dd");
    JsonObject *table = table_of (msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK (table != NULL);
    if (table != NULL)
    {
        JsonObject *header;
        JsonArray  *hcells;
        JsonArray  *rows;

        PN_CHECK (json_object_has_member (table, "header"));
        header = json_object_get_object_member (table, "header");
        hcells = json_object_get_array_member  (header, "cells");
        PN_CHECK_CMPINT (json_array_get_length (hcells), ==, 2);
        PN_CHECK_CMPSTR (cell_text (hcells, 0), ==, "aa");
        PN_CHECK_CMPSTR (cell_text (hcells, 1), ==, "bb");

        /* The header consumed the first line, leaving one data row. */
        rows = json_object_get_array_member (table, "rows");
        PN_CHECK_CMPINT (json_array_get_length (rows), ==, 1);
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 0), 0), ==, "cc");
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 0), 1), ==, "dd");
    }

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_header_keeps_first_line_as_row (void)
{
    guint      emits;
    PnNode    *node  = make_node (FALSE, &emits);
    PnMessage *msg   = run (node, "aa bb\ncc dd");
    JsonObject *table = table_of (msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK (table != NULL);
    if (table != NULL)
    {
        JsonArray *rows;

        PN_CHECK_FALSE (json_object_has_member (table, "header"));

        /* Both lines become rows when header parsing is off. */
        rows = json_object_get_array_member (table, "rows");
        PN_CHECK_CMPINT (json_array_get_length (rows), ==, 2);
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 0), 0), ==, "aa");
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 0), 1), ==, "bb");
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 1), 0), ==, "cc");
        PN_CHECK_CMPSTR (cell_text (row_cells (rows, 1), 1), ==, "dd");
    }

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-table-model");
    pn_test_add ("no_output_empty",        test_no_output_empty_table);
    pn_test_add ("parse_header_columns",   test_parse_header_splits_columns);
    pn_test_add ("no_header_first_row",    test_no_header_keeps_first_line_as_row);
    return pn_test_run ();
}
