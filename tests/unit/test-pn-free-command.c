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

/* Unit tests for PnFreeCommand's `free` output parser.  Canned command
 * text stands in for a live `free` run (the node's trigger captures
 * exactly this string before handing it to the parser).  The default
 * output is space-separated and fixed:
 *
 *     "<spaces>total used free shared buff/cache available"
 *     "Mem:     <total> <used> <free> <shared> <buff/cache> <avail>"
 *     "Swap:    <total> <used> <free>"
 *
 * so the column count comes from the header word count, and the shorter
 * Swap line is padded with empty trailing cells. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pntest.h"

/* Defined (non-static) in plugins/shell/pn-free-command.c — see the note
 * there.  Returns a freshly-allocated JSON tree the caller owns. */
JsonNode *pn_free_command_build_table (const gchar *output);

/* Real `free` output (the trailing column headers and the short Swap row
 * are the two things the parser has to get right). */
static const gchar FREE_OUTPUT[] =
        "               total        used        free      shared  buff/cache   available\n"
        "Mem:        16384000     8000000     2000000      500000     6384000     7500000\n"
        "Swap:        2097148           0     2097148\n";

/* ---- JSON navigation helpers ------------------------------------- */

static JsonObject *
table_of (JsonNode *node)
{
    return json_node_get_object (node);
}

static JsonArray *
header_cells (JsonNode *node)
{
    JsonObject *table  = table_of (node);
    JsonObject *header = json_object_get_object_member (table, "header");
    return json_object_get_array_member (header, "cells");
}

static JsonArray *
rows (JsonNode *node)
{
    return json_object_get_array_member (table_of (node), "rows");
}

static JsonArray *
row_cells (JsonNode *node, guint row)
{
    JsonObject *r = json_array_get_object_element (rows (node), row);
    return json_object_get_array_member (r, "cells");
}

static const gchar *
cell_text (JsonArray *cells, guint i)
{
    JsonObject *cell = json_array_get_object_element (cells, i);
    return json_object_get_string_member (cell, "text");
}

static const gchar *
cell_name (JsonArray *cells, guint i)
{
    JsonObject *cell = json_array_get_object_element (cells, i);
    return json_object_get_string_member (cell, "name");
}

/* ---- tests ------------------------------------------------------- */

static void
test_header_ids (void)
{
    JsonNode  *node  = pn_free_command_build_table (FREE_OUTPUT);
    JsonArray *cells = header_cells (node);

    /* Row-label column + the six header words. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);

    /* Leading row-label column carries no header text but a fixed id. */
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "header.name");

    /* The header text is preserved verbatim while the cell name uses the
     * sanitised id ("buff/cache" -> "buff_cache"). */
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "total");
    PN_CHECK_CMPSTR (cell_name (cells, 1), ==, "header.total");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "buff/cache");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "header.buff_cache");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "available");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "header.available");

    json_node_unref (node);
}

static void
test_mem_row (void)
{
    JsonNode  *node  = pn_free_command_build_table (FREE_OUTPUT);
    JsonArray *cells = row_cells (node, 0);

    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);

    /* The "Mem:" label keeps its text but loses the colon in the id, and
     * the row id pairs with each column id. */
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "Mem:");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "mem.name");
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "16384000");
    PN_CHECK_CMPSTR (cell_name (cells, 1), ==, "mem.total");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "6384000");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "mem.buff_cache");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "7500000");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "mem.available");

    json_node_unref (node);
}

static void
test_swap_row_is_padded (void)
{
    JsonNode  *node  = pn_free_command_build_table (FREE_OUTPUT);
    JsonArray *cells = row_cells (node, 1);

    /* Swap carries only total/used/free, but every row is widened to the
     * header's column count so a consumer can address shared/buff_cache/
     * available by name without guarding against a ragged row. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);

    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "Swap:");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "swap.name");
    PN_CHECK_CMPSTR (cell_text (cells, 3), ==, "2097148");
    PN_CHECK_CMPSTR (cell_name (cells, 3), ==, "swap.free");

    /* The three columns past `free` are present but empty. */
    PN_CHECK_CMPSTR (cell_text (cells, 4), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 4), ==, "swap.shared");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "swap.available");

    json_node_unref (node);
}

static void
test_row_count (void)
{
    JsonNode *node = pn_free_command_build_table (FREE_OUTPUT);

    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 2);

    json_node_unref (node);
}

static void
test_null_output_is_empty (void)
{
    /* The trigger passes NULL when `free` failed; the parser must still
     * return a well-formed (empty) table rather than crash. */
    JsonNode   *node  = pn_free_command_build_table (NULL);
    JsonObject *table = table_of (node);

    PN_CHECK (json_object_has_member (table, "rows"));
    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 0);
    /* Too little input to identify columns -> no header emitted. */
    PN_CHECK_FALSE (json_object_has_member (table, "header"));

    json_node_unref (node);
}

static void
test_header_only_is_empty (void)
{
    /* A header with no data row is degenerate (needs >= 2 lines); the
     * parser yields an empty table rather than a header-only one. */
    JsonNode   *node  = pn_free_command_build_table (
            "               total        used        free\n");
    JsonObject *table = table_of (node);

    PN_CHECK_FALSE (json_object_has_member (table, "header"));
    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 0);

    json_node_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-free-command");
    pn_test_add ("header_ids",        test_header_ids);
    pn_test_add ("mem_row",           test_mem_row);
    pn_test_add ("swap_row_padded",   test_swap_row_is_padded);
    pn_test_add ("row_count",         test_row_count);
    pn_test_add ("null_is_empty",     test_null_output_is_empty);
    pn_test_add ("header_only_empty", test_header_only_is_empty);
    return pn_test_run ();
}
