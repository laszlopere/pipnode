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

/* Unit tests for PnDfCommand's `df` output parser.  Canned command text
 * stands in for a live `df` run.  `df` is always six logical columns:
 *
 *     Filesystem  1K-blocks  Used  Available  Use%  Mounted on
 *
 * The first five are whitespace-free; the sixth (the mount path) may
 * contain spaces, so the parser folds every trailing token back into the
 * last column.  The column count is fixed at six rather than derived from
 * the header, so the internal-space "Mounted on" header does not spawn a
 * seventh column. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pntest.h"

/* Defined (non-static) in plugins/shell/pn-df-command.c — see the note
 * there.  Returns a freshly-allocated JSON tree the caller owns. */
JsonNode *pn_df_command_build_table (const gchar *output);

/* The last data row's mount point holds a space ("/mnt/My Documents") to
 * prove the tail-join keeps it in a single cell rather than splitting it
 * into a phantom seventh column. */
static const gchar DF_OUTPUT[] =
        "Filesystem      1K-blocks      Used  Available Use% Mounted on\n"
        "/dev/nvme0n1p2  500000000 200000000  280000000  42% /\n"
        "tmpfs             8000000         0    8000000   0% /dev/shm\n"
        "/dev/sda1          100000     50000      50000  50% /mnt/My Documents\n";

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
test_header_is_six_columns (void)
{
    JsonNode  *node  = pn_df_command_build_table (DF_OUTPUT);
    JsonArray *cells = header_cells (node);

    /* Fixed six columns, even though "Mounted on" is two whitespace
     * tokens — the sixth column absorbs the tail. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 6);

    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "Filesystem");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "header.filesystem");
    /* "1K-blocks" -> lowercased, '-' -> '_'. */
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "1K-blocks");
    PN_CHECK_CMPSTR (cell_name (cells, 1), ==, "header.1k_blocks");
    /* "Use%" -> trailing non-alnum stripped. */
    PN_CHECK_CMPSTR (cell_text (cells, 4), ==, "Use%");
    PN_CHECK_CMPSTR (cell_name (cells, 4), ==, "header.use");
    /* The two header tokens fold into one "Mounted on" cell. */
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "Mounted on");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "header.mounted_on");

    json_node_unref (node);
}

static void
test_simple_rows (void)
{
    JsonNode  *node  = pn_df_command_build_table (DF_OUTPUT);
    JsonArray *cells;

    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 3);

    /* A device path's leading slash is dropped from the row id. */
    cells = row_cells (node, 0);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "/dev/nvme0n1p2");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "dev_nvme0n1p2.filesystem");
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "500000000");
    PN_CHECK_CMPSTR (cell_name (cells, 1), ==, "dev_nvme0n1p2.1k_blocks");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "/");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "dev_nvme0n1p2.mounted_on");

    /* A plain name keeps its id. */
    cells = row_cells (node, 1);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "tmpfs");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "tmpfs.filesystem");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "/dev/shm");

    json_node_unref (node);
}

static void
test_mount_path_with_space (void)
{
    JsonNode  *node  = pn_df_command_build_table (DF_OUTPUT);
    JsonArray *cells = row_cells (node, 2);

    /* The spaced mount point stays a single cell; the row still has
     * exactly six columns. */
    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 6);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "/dev/sda1");
    PN_CHECK_CMPSTR (cell_name (cells, 0), ==, "dev_sda1.filesystem");
    PN_CHECK_CMPSTR (cell_text (cells, 4), ==, "50%");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "/mnt/My Documents");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "dev_sda1.mounted_on");

    json_node_unref (node);
}

static void
test_short_row_is_padded (void)
{
    /* A row with fewer than six fields must pad the missing trailing
     * cells with "" rather than read past the token array's terminator. */
    JsonNode  *node  = pn_df_command_build_table (
            "Filesystem 1K-blocks Used Available Use% Mounted on\n"
            "tmpfs 8000000\n");
    JsonArray *cells = row_cells (node, 0);

    PN_CHECK_CMPINT (json_array_get_length (cells), ==, 6);
    PN_CHECK_CMPSTR (cell_text (cells, 0), ==, "tmpfs");
    PN_CHECK_CMPSTR (cell_text (cells, 1), ==, "8000000");
    PN_CHECK_CMPSTR (cell_text (cells, 2), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 2), ==, "tmpfs.used");
    PN_CHECK_CMPSTR (cell_text (cells, 5), ==, "");
    PN_CHECK_CMPSTR (cell_name (cells, 5), ==, "tmpfs.mounted_on");

    json_node_unref (node);
}

static void
test_null_output_is_empty (void)
{
    JsonNode   *node  = pn_df_command_build_table (NULL);
    JsonObject *table = table_of (node);

    PN_CHECK (json_object_has_member (table, "rows"));
    PN_CHECK_CMPINT (json_array_get_length (rows (node)), ==, 0);
    PN_CHECK_FALSE (json_object_has_member (table, "header"));

    json_node_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-df-command");
    pn_test_add ("header_six_columns", test_header_is_six_columns);
    pn_test_add ("simple_rows",        test_simple_rows);
    pn_test_add ("mount_path_space",   test_mount_path_with_space);
    pn_test_add ("short_row_padded",   test_short_row_is_padded);
    pn_test_add ("null_is_empty",      test_null_output_is_empty);
    return pn_test_run ();
}
