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

/* Unit tests for PnRam: addressable read/write memory.  The address is
 * data.value (PROM-compatible); a read emits the stored word on
 * data.value + the cell on data.address, a write (data.write true)
 * stores data.word silently.  Headless: one node, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ram.h"

typedef struct
{
    guint      count;
    PnMessage *last;   /* ref to the most recently emitted message */
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    (void) node;
    r->count++;
    g_clear_object (&r->last);
    r->last = g_object_ref (message);
}

static PnNode *
make_node (const gchar *contents, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_ram_new ());
    if (contents != NULL)
        g_object_set (node, "contents", contents, NULL);
    rec->count = 0;
    rec->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Read address @addr; asserts the node emitted and returns the word. */
static void
do_read (PnNode *node, gdouble addr)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", addr);
    pn_node_receive_message_on_input (node, m, 0);
    g_object_unref (m);
}

/* Write @word at @addr with a boolean write strobe. */
static void
do_write (PnNode *node, gdouble addr, gdouble word)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double  (m, "value", addr);
    pn_message_set_double  (m, "word",  word);
    pn_message_set_boolean (m, "write", TRUE);
    pn_node_receive_message_on_input (node, m, 0);
    g_object_unref (m);
}

/* An address that was never written reads back 0.0, and the decoded
 * cell is echoed on data.address (the PROM output shape). */
static void
test_unwritten_reads_zero (void)
{
    Recorder rec;
    PnNode  *node = make_node (NULL, &rec);

    do_read (node, 5.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (rec.last, "value"),   0.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (rec.last, "address"), 5.0, 1e-9);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* A write is silent, and a later read of the same cell returns the word. */
static void
test_write_then_read (void)
{
    Recorder rec;
    PnNode  *node = make_node (NULL, &rec);

    do_write (node, 3.0, 42.0);
    PN_CHECK_CMPINT (rec.count, ==, 0);   /* write emits nothing */

    do_read (node, 3.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (rec.last, "value"), 42.0, 1e-9);

    /* A different cell is still empty. */
    do_read (node, 4.0);
    PN_CHECK_NEAR (pn_test_num (rec.last, "value"), 0.0, 1e-9);

    /* Overwrite wins. */
    do_write (node, 3.0, -1.0);
    do_read  (node, 3.0);
    PN_CHECK_NEAR (pn_test_num (rec.last, "value"), -1.0, 1e-9);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* A numeric write strobe past the 0.5 midpoint also triggers a write. */
static void
test_numeric_write_flag (void)
{
    Recorder   rec;
    PnNode    *node = make_node (NULL, &rec);
    PnMessage *m;

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", 7.0);
    pn_message_set_double (m, "word",  99.0);
    pn_message_set_double (m, "write", 1.0);   /* numeric strobe */
    pn_node_receive_message_on_input (node, m, 0);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 0);
    do_read (node, 7.0);
    PN_CHECK_NEAR (pn_test_num (rec.last, "value"), 99.0, 1e-9);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* The contents property seeds the memory the same way PROM's image does. */
static void
test_seed_contents (void)
{
    Recorder rec;
    PnNode  *node = make_node ("0x10 0xff\n2 1.5\n", &rec);

    do_read (node, 16.0);
    PN_CHECK_NEAR (pn_test_num (rec.last, "value"), 255.0, 1e-9);

    do_read (node, 2.0);
    PN_CHECK_NEAR (pn_test_num (rec.last, "value"), 1.5, 1e-9);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* A message with no numeric data.value drives no address line: it is
 * dropped without emitting. */
static void
test_no_address_dropped (void)
{
    Recorder   rec;
    PnNode    *node = make_node (NULL, &rec);
    PnMessage *m;

    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "nope");
    pn_node_receive_message_on_input (node, m, 0);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 0);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ram");
    pn_test_add ("unwritten_reads_zero", test_unwritten_reads_zero);
    pn_test_add ("write_then_read",      test_write_then_read);
    pn_test_add ("numeric_write_flag",   test_numeric_write_flag);
    pn_test_add ("seed_contents",        test_seed_contents);
    pn_test_add ("no_address_dropped",   test_no_address_dropped);
    return pn_test_run ();
}
