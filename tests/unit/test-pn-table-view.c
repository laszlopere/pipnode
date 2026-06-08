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

/* Unit tests for PnTableView.  The cairo grid only matters on screen, so
 * these headless tests cover the logic contract: it is a pure sink, every
 * received message REPLACES the displayed snapshot pulled from data.table,
 * the JSON cell parser accepts both the { "text": ... } wrapper PnTableModel
 * produces and bare scalars, n_cols is the max across header + rows, a
 * message without a recognisable table clears the view, the scroll
 * bookkeeping nudges + clamps, and the colour / alternate-row properties
 * round-trip.  The parsed snapshot is read through the GTK-free seam
 * (pn_table_view_peek_header / peek_rows / get_paint_state). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-table-view.h"

#include <json-glib/json-glib.h>

/* ------------------------------------------------------------------ */
/*  Table builders                                                     */
/* ------------------------------------------------------------------ */

/* Append a "cells" array of { "text": v } wrapper objects (the shape
 * PnTableModel emits). */
static void
add_text_cells (JsonBuilder *b, const gchar * const *vals, guint n)
{
    guint i;
    json_builder_set_member_name (b, "cells");
    json_builder_begin_array (b);
    for (i = 0; i < n; i++)
    {
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "text");
        json_builder_add_string_value (b, vals[i]);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
}

/* Append a "cells" array of bare scalar strings (a hand-built table that
 * never went through PnTableModel). */
static void
add_scalar_cells (JsonBuilder *b, const gchar * const *vals, guint n)
{
    guint i;
    json_builder_set_member_name (b, "cells");
    json_builder_begin_array (b);
    for (i = 0; i < n; i++)
        json_builder_add_string_value (b, vals[i]);
    json_builder_end_array (b);
}

/* Run @table (NULL for "no table member") through @node and drop the
 * message.  Returns the emit count seen so the caller can assert it. */
static guint
run_table (PnNode *node, JsonNode *table)
{
    guint      emits = 0;
    gulong     id;
    PnMessage *msg = pn_message_new (NULL, NULL);

    id = g_signal_connect (node, "message",
                           G_CALLBACK (pn_test_count_emits), &emits);

    if (table != NULL)
        pn_message_set_member (msg, "table", table);

    pn_node_receive_message (node, msg);

    g_signal_handler_disconnect (node, id);
    g_object_unref (msg);
    return emits;
}

/* A header { "cells": [...] } + two rows, the header & first row as
 * { text } wrappers, the second row (one wide) as bare scalars. */
static JsonNode *
make_sample_table (void)
{
    JsonBuilder *b   = json_builder_new ();
    JsonNode    *out;
    const gchar *hdr[] = { "Name", "Size" };
    const gchar *r0[]  = { "a.txt", "10" };
    const gchar *r1[]  = { "lonely" };

    json_builder_begin_object (b);

    json_builder_set_member_name (b, "header");
    json_builder_begin_object (b);
    add_text_cells (b, hdr, G_N_ELEMENTS (hdr));
    json_builder_end_object (b);

    json_builder_set_member_name (b, "rows");
    json_builder_begin_array (b);
    json_builder_begin_object (b);
    add_text_cells (b, r0, G_N_ELEMENTS (r0));
    json_builder_end_object (b);
    json_builder_begin_object (b);
    add_scalar_cells (b, r1, G_N_ELEMENTS (r1));
    json_builder_end_object (b);
    json_builder_end_array (b);

    json_builder_end_object (b);

    out = json_builder_get_root (b);   /* transfer full */
    g_object_unref (b);
    return out;
}

static const gchar *
row_cell (GPtrArray *rows, guint r, guint c)
{
    PnTableViewRow *row = g_ptr_array_index (rows, r);
    return (c < row->n_cells) ? row->cells[c] : NULL;
}

/* Drive the scroll vfunc the way the zoom overlay does — there is no
 * public pn_node_scroll() wrapper, the worksheet calls the class slot. */
static void
scroll (PnNode *node, double dy)
{
    PnNodeClass *klass = PN_NODE_GET_CLASS (node);
    if (klass->scroll != NULL)
        klass->scroll (node, dy);
}

/* ------------------------------------------------------------------ */
/*  Cases                                                              */
/* ------------------------------------------------------------------ */

/* A sink never forwards: feeding a table emits nothing downstream. */
static void
test_is_a_sink (void)
{
    PnNode *node = PN_NODE (pn_table_view_new ());
    PN_CHECK_CMPINT (run_table (node, make_sample_table ()), ==, 0);
    g_object_unref (node);
}

/* The header, rows and column count are pulled out of data.table and held
 * for the painter, with both cell shapes ({text} and bare scalar) decoded. */
static void
test_ingests_table (void)
{
    PnTableView          *tv = pn_table_view_new ();
    const gchar * const  *hdr;
    guint                 n_hdr = 0;
    GPtrArray            *rows;
    PnTableViewPaintState st;

    run_table (PN_NODE (tv), make_sample_table ());

    hdr = pn_table_view_peek_header (tv, &n_hdr);
    PN_CHECK_CMPINT (n_hdr, ==, 2);
    PN_CHECK (hdr != NULL);
    PN_CHECK_CMPSTR (hdr[0], ==, "Name");
    PN_CHECK_CMPSTR (hdr[1], ==, "Size");

    rows = pn_table_view_peek_rows (tv);
    PN_CHECK_CMPINT (rows->len, ==, 2);
    PN_CHECK_CMPSTR (row_cell (rows, 0, 0), ==, "a.txt");   /* {text} */
    PN_CHECK_CMPSTR (row_cell (rows, 0, 1), ==, "10");
    PN_CHECK_CMPSTR (row_cell (rows, 1, 0), ==, "lonely");  /* bare scalar */

    /* n_cols is the max cell count across header + rows (2 here). */
    pn_table_view_get_paint_state (tv, &st);
    PN_CHECK_CMPINT (st.n_cols, ==, 2);

    g_object_unref (tv);
}

/* Each received message REPLACES the displayed table rather than appending. */
static void
test_replaces_on_receive (void)
{
    PnTableView *tv = pn_table_view_new ();
    JsonBuilder *b;
    JsonNode    *table;
    GPtrArray   *rows;
    const gchar *r0[] = { "x", "y", "z" };

    run_table (PN_NODE (tv), make_sample_table ());
    PN_CHECK_CMPINT (pn_table_view_peek_rows (tv)->len, ==, 2);

    /* A second, single-row three-wide table replaces the first. */
    b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "rows");
    json_builder_begin_array (b);
    json_builder_begin_object (b);
    add_scalar_cells (b, r0, G_N_ELEMENTS (r0));
    json_builder_end_object (b);
    json_builder_end_array (b);
    json_builder_end_object (b);
    table = json_builder_get_root (b);
    g_object_unref (b);

    run_table (PN_NODE (tv), table);

    rows = pn_table_view_peek_rows (tv);
    PN_CHECK_CMPINT (rows->len, ==, 1);
    PN_CHECK_CMPSTR (row_cell (rows, 0, 2), ==, "z");

    /* No header this time -> peek_header reports none. */
    {
        guint n = 99;
        PN_CHECK (pn_table_view_peek_header (tv, &n) == NULL);
        PN_CHECK_CMPINT (n, ==, 0);
    }

    /* n_cols tracks the widest row of the *new* table. */
    {
        PnTableViewPaintState st;
        pn_table_view_get_paint_state (tv, &st);
        PN_CHECK_CMPINT (st.n_cols, ==, 3);
    }

    g_object_unref (tv);
}

/* A message without a recognisable table clears the view to the empty
 * state rather than leaving the previous render stuck. */
static void
test_no_table_clears (void)
{
    PnTableView          *tv = pn_table_view_new ();
    PnTableViewPaintState st;
    guint                 n = 99;

    run_table (PN_NODE (tv), make_sample_table ());
    PN_CHECK_CMPINT (pn_table_view_peek_rows (tv)->len, ==, 2);

    /* No "table" member at all. */
    run_table (PN_NODE (tv), NULL);

    PN_CHECK_CMPINT (pn_table_view_peek_rows (tv)->len, ==, 0);
    PN_CHECK (pn_table_view_peek_header (tv, &n) == NULL);
    PN_CHECK_CMPINT (n, ==, 0);

    pn_table_view_get_paint_state (tv, &st);
    PN_CHECK_CMPINT (st.n_cols, ==, 0);

    g_object_unref (tv);
}

/* The scroll bookkeeping nudges the offset (rounded from the wheel delta),
 * never goes below zero, and is pinned by the painter-driven clamp. */
static void
test_scroll_and_clamp (void)
{
    PnTableView *tv = pn_table_view_new ();

    PN_CHECK_CMPINT (pn_table_view_get_scroll_offset (tv), ==, 0);

    /* dy 2.0 -> step lround(6.0) = 6. */
    scroll (PN_NODE (tv), 2.0);
    PN_CHECK_CMPINT (pn_table_view_get_scroll_offset (tv), ==, 6);

    /* The painter knows the live extents and clamps to a max of 3. */
    pn_table_view_clamp_scroll_offset (tv, 3);
    PN_CHECK_CMPINT (pn_table_view_get_scroll_offset (tv), ==, 3);

    /* A big upward nudge can never drive the offset negative. */
    scroll (PN_NODE (tv), -10.0);
    PN_CHECK_CMPINT (pn_table_view_get_scroll_offset (tv), ==, 0);

    /* Receiving a fresh table resets the scroll. */
    scroll (PN_NODE (tv), 5.0);
    run_table (PN_NODE (tv), make_sample_table ());
    PN_CHECK_CMPINT (pn_table_view_get_scroll_offset (tv), ==, 0);

    g_object_unref (tv);
}

/* The colour properties and the alternate-row toggle round-trip and the
 * scalar drawing config surfaces in the snapshot. */
static void
test_properties_round_trip (void)
{
    PnTableView          *tv  = pn_table_view_new ();
    PnColor               bg  = { 0.10, 0.20, 0.30, 1.0 };
    PnColor               grd = { 0.40, 0.50, 0.60, 1.0 };
    PnColor              *bg_out = NULL;
    gboolean              alt = TRUE;
    PnTableViewPaintState st;

    /* Default alternate-row background is on. */
    g_object_get (tv, "alternate-row-background", &alt, NULL);
    PN_CHECK (alt == TRUE);

    g_object_set (tv,
                  "background-color",         &bg,
                  "grid-color",               &grd,
                  "alternate-row-background", FALSE,
                  NULL);
    g_object_get (tv,
                  "background-color",         &bg_out,
                  "alternate-row-background", &alt,
                  NULL);

    PN_CHECK (bg_out != NULL && pn_color_equal (bg_out, &bg));
    PN_CHECK (alt == FALSE);

    pn_table_view_get_paint_state (tv, &st);
    PN_CHECK (pn_color_equal (&st.background_color, &bg));
    PN_CHECK (pn_color_equal (&st.grid_color,       &grd));
    PN_CHECK (st.alternate_row_background == FALSE);

    pn_color_free (bg_out);
    g_object_unref (tv);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-table-view");
    pn_test_add ("is_a_sink",            test_is_a_sink);
    pn_test_add ("ingests_table",        test_ingests_table);
    pn_test_add ("replaces_on_receive",  test_replaces_on_receive);
    pn_test_add ("no_table_clears",      test_no_table_clears);
    pn_test_add ("scroll_and_clamp",     test_scroll_and_clamp);
    pn_test_add ("properties_round_trip", test_properties_round_trip);
    return pn_test_run ();
}
