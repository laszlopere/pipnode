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

/* Unit tests for PnLxcLsCommand's `lxc-ls -f` output parser.  Canned
 * command text stands in for a live run.  Unlike `df` (six fixed
 * columns), the column count here is taken from the header word count,
 * and unlike `free`/`df` a header with no data rows still emits the
 * header (the parser builds a table from one or more cleaned lines).
 * The last column folds any trailing tokens back in, so a future
 * UNPRIVILEGED value like "true (custom)" stays one cell. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pntest.h"

/* Defined (non-static) in plugins/shell/pn-lxc-ls-command.c — see the
 * note there.  Returns a freshly-allocated JSON tree the caller owns. */
JsonNode *pn_lxc_ls_command_build_table (const gchar *output);

/* The third data row carries a spaced UNPRIVILEGED value to exercise the
 * tail-join; the fourth is short to exercise empty-cell padding. */
static const gchar LXC_OUTPUT[] =
        "NAME  STATE   AUTOSTART GROUPS IPV4      IPV6 UNPRIVILEGED\n"
        "web01 RUNNING 1         -      10.0.3.10 -    false\n"
        "db01  STOPPED 0         -      -         -    false\n"
        "c1    RUNNING 1         -      10.0.3.20 -    true (custom)\n"
        "minimal RUNNING\n";

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
    JsonNode  *node  = pn_lxc_ls_command_build_table (LXC_OUTPUT);
    JsonArray *cells = header_cells (node);

    /* Column count follows the header word count (seven here), with no
     * extra leading row-label column. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);

    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "NAME");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "header.name");
    PN_CHECK_CMPSTR (cell_text (cells, 4), ==, "IPV4");
    PN_CHECK_CMPSTR (cell_name (cells, 4), ==, "header.ipv4");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "UNPRIVILEGED");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "header.unprivileged");

    json_node_unref (node);
}

static void
test_simple_row (void)
{
    JsonNode  *node  = pn_lxc_ls_command_build_table (LXC_OUTPUT);
    JsonArray *cells = row_cells (node, 0);

    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "web01");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "web01.name");
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "RUNNING");
    PN_CHECK_CMPSTR (cell_name (cells, 1), ==, "web01.state");
    PN_CHECK_CMPSTR (cell_text (cells, 4), ==, "10.0.3.10");
    PN_CHECK_CMPSTR (cell_name (cells, 4), ==, "web01.ipv4");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "false");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "web01.unprivileged");

    json_node_unref (node);
}

static void
test_last_column_absorbs_spaces (void)
{
    JsonNode  *node  = pn_lxc_ls_command_build_table (LXC_OUTPUT);
    JsonArray *cells = row_cells (node, 2);

    /* "true (custom)" is two whitespace tokens but the last column folds
     * the tail back in, so the row stays seven cells. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "c1");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "true (custom)");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "c1.unprivileged");

    json_node_unref (node);
}

static void
test_short_row_is_padded (void)
{
    JsonNode  *node  = pn_lxc_ls_command_build_table (LXC_OUTPUT);
    JsonArray *cells = row_cells (node, 3);

    /* A row with fewer fields than the header is widened with empty
     * trailing cells (including the join_tail'd last column). */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 7);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "minimal");
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "RUNNING");
    PN_CHECK_CMPSTR (cell_text (cells, 2), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 2), ==, "minimal.autostart");
    PN_CHECK_CMPSTR (cell_text (cells, 6), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 6), ==, "minimal.unprivileged");

    json_node_unref (node);
}

static void
test_header_only_keeps_header (void)
{
    /* Distinct from free/df: a header with no data rows still produces a
     * header (the parser builds from >= 1 cleaned line), with empty rows. */
    JsonNode   *node  = pn_lxc_ls_command_build_table (
            "NAME  STATE   AUTOSTART\n");
    JsonObject *table = table_of (node);

    PN_CHECK (json_object_has_member (table, "header"));
    PN_CHECK_CMPINT (json_array_get_length (header_cells (node)), ==, 3);
    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 0);

    json_node_unref (node);
}

static void
test_null_output_is_empty (void)
{
    JsonNode   *node  = pn_lxc_ls_command_build_table (NULL);
    JsonObject *table = table_of (node);

    PN_CHECK (json_object_has_member (table, "rows"));
    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 0);
    PN_CHECK_FALSE (json_object_has_member (table, "header"));

    json_node_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-lxc-ls-command");
    pn_test_add ("header_ids",          test_header_ids);
    pn_test_add ("simple_row",          test_simple_row);
    pn_test_add ("last_col_spaces",     test_last_column_absorbs_spaces);
    pn_test_add ("short_row_padded",    test_short_row_is_padded);
    pn_test_add ("header_only",         test_header_only_keeps_header);
    pn_test_add ("null_is_empty",       test_null_output_is_empty);
    return pn_test_run ();
}
