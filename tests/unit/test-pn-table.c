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

/* Unit tests for PnTable.  The cell rendering only matters on screen,
 * so the headless tests cover the receive() contract: it is a pure sink
 * (never forwards), it appends one buffered row per message when at
 * least one column is configured, a message received with no columns is
 * a safe no-op, and the buffer is capped at #limit (oldest rows
 * dropped).  Row accumulation is observed through pn_table_get_row_count
 * since the row store is private.  These pin the logic half the
 * headless/core split (TODO #23) must keep loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-table.h"

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnTable   *table = pn_table_new ();
    PnNode    *node  = PN_NODE (table);
    PnMessage *msg   = pn_message_new (NULL, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_default_columns_accept_rows (void)
{
    PnTable   *table = pn_table_new ();
    PnNode    *node  = PN_NODE (table);
    PnMessage *msg   = pn_message_new (NULL, NULL);

    /* The default "topic:topic,id:id" spec means a fresh table already
     * has columns, so the first message lands as a row. */
    PN_CHECK_CMPINT (pn_table_get_row_count (table), ==, 0u);
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (pn_table_get_row_count (table), ==, 1u);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_no_columns_is_noop (void)
{
    PnTable   *table = pn_table_new ();
    PnNode    *node  = PN_NODE (table);
    PnMessage *msg   = pn_message_new (NULL, NULL);

    /* Clearing the spec leaves zero columns; a message then has no cell
     * to fill, so no row is buffered. */
    g_object_set (table, "columns", "", NULL);
    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (pn_table_get_row_count (table), ==, 0u);

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_rows_accumulate_and_trim (void)
{
    PnTable   *table = pn_table_new ();
    PnNode    *node  = PN_NODE (table);
    PnMessage *msg   = pn_message_new (NULL, NULL);

    /* One real column, history capped at two rows. */
    g_object_set (table, "columns", "Val:data/value", "limit", 2u, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (pn_table_get_row_count (table), ==, 2u);

    /* The third message pushes the buffer past its limit; the oldest
     * row is dropped so the count holds at the cap. */
    pn_message_set_double (msg, "value", 3.0);
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (pn_table_get_row_count (table), ==, 2u);

    g_object_unref (msg);
    g_object_unref (node);
}

/* A table draws a grid below its standard header, so it reports a
 * client area: the worksheet's palette drag-preview outlines it.  The
 * rectangle is node-local (offset from the node origin) and sits under
 * the header past the 4 px header→body gap. */
static void
test_reports_client_area (void)
{
    PnTable *table = pn_table_new ();
    PnNode  *node  = PN_NODE (table);
    double   x = -1.0, y = -1.0, w = -1.0, h = -1.0;
    double   full_w, full_h, hh;

    PN_CHECK (pn_node_get_client_area (node, &x, &y, &w, &h));

    pn_node_get_size (node, &full_w, &full_h);
    hh = pn_node_get_header_height (node);

    PN_CHECK_NEAR (x, 0.0,               0.01);
    PN_CHECK_NEAR (y, hh + 4.0,          0.01);
    PN_CHECK_NEAR (w, full_w,            0.01);
    PN_CHECK_NEAR (h, full_h - hh - 4.0, 0.01);
    PN_CHECK (h > 0.0);

    g_object_unref (table);
}

static void
test_limit_default_and_round_trip (void)
{
    PnTable    *table = pn_table_new ();
    GParamSpec *pspec;
    guint       limit = 0;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (table), "limit");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    PN_CHECK_CMPINT (G_PARAM_SPEC_UINT (pspec)->default_value, ==, 200u);

    g_object_set (table, "limit", 50u, NULL);
    g_object_get (table, "limit", &limit, NULL);
    PN_CHECK_CMPINT (limit, ==, 50u);

    g_object_unref (table);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-table");
    pn_test_add ("is_a_sink",                 test_is_a_sink);
    pn_test_add ("default_columns_accept_rows", test_default_columns_accept_rows);
    pn_test_add ("no_columns_is_noop",        test_no_columns_is_noop);
    pn_test_add ("rows_accumulate_and_trim",  test_rows_accumulate_and_trim);
    pn_test_add ("limit_default_round_trip",  test_limit_default_and_round_trip);
    pn_test_add ("reports_client_area",       test_reports_client_area);
    return pn_test_run ();
}
