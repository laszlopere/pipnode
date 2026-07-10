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

/* Unit tests for PnProm: the incoming data.value is an address, rounded
 * to the nearest cell and read out of the programmed image.  Addresses
 * that were never programmed read back as 0.0. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-prom.h"

static PnNode *
make_node (const gchar *contents, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_PROM, "contents", contents, NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

/* Drive @address into @node and return the word it read out.  The node
 * rewrites the message in place, so the caller reads the result off the
 * same message it sent. */
static gdouble
read_at (PnNode *node, gdouble address)
{
    PnMessage *msg = pn_message_new (NULL, NULL);
    gdouble    word;

    pn_message_set_double (msg, "value", address);
    pn_node_receive_message (node, msg);
    word = pn_test_num (msg, "value");

    g_object_unref (msg);
    return word;
}

static void
test_reads_programmed_cells (void)
{
    guint   emits;
    PnNode *node = make_node ("0x0000 0xff\n0x0001 0x1a\n", &emits);

    PN_CHECK_NEAR   (read_at (node, 0.0), 255.0, 1e-9);
    PN_CHECK_NEAR   (read_at (node, 1.0),  26.0, 1e-9);
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (node);
}

/* An address line settles on the nearest whole cell: halves round away
 * from zero, as llround() does. */
static void
test_address_is_rounded (void)
{
    guint   emits;
    PnNode *node = make_node ("0 0x10\n1 0x20\n2 0x30\n", &emits);

    PN_CHECK_NEAR (read_at (node, 0.49), 16.0, 1e-9);   /* -> 0 */
    PN_CHECK_NEAR (read_at (node, 0.5),  32.0, 1e-9);   /* -> 1 */
    PN_CHECK_NEAR (read_at (node, 1.4),  32.0, 1e-9);   /* -> 1 */
    PN_CHECK_NEAR (read_at (node, 1.5),  48.0, 1e-9);   /* -> 2 */

    g_object_unref (node);
}

/* An address that was never programmed reads back as an unburnt cell. */
static void
test_missing_address_reads_zero (void)
{
    guint   emits;
    PnNode *node = make_node ("0x0000 0xff\n", &emits);

    PN_CHECK_NEAR   (read_at (node, 7.0), 0.0, 1e-9);
    PN_CHECK_CMPINT (emits, ==, 1);   /* a miss still emits */

    g_object_unref (node);
}

/* Both columns accept hex or decimal, the word may be fractional, and
 * blank lines / #-comments (whole-line and trailing) are skipped. */
static void
test_image_syntax (void)
{
    guint   emits;
    PnNode *node = make_node ("# the image\n"
                              "\n"
                              "  10   1.5   \n"
                              "0x0b -2      # a negative word\n"
                              "12\t0XFF\n",
                              &emits);

    PN_CHECK_NEAR (read_at (node, 10.0),   1.5, 1e-9);
    PN_CHECK_NEAR (read_at (node, 11.0),  -2.0, 1e-9);
    PN_CHECK_NEAR (read_at (node, 12.0), 255.0, 1e-9);
    PN_CHECK      (!pn_node_get_has_error (node));

    g_object_unref (node);
}

/* A later line for the same address reprograms the cell. */
static void
test_last_line_wins (void)
{
    guint   emits;
    PnNode *node = make_node ("0x00 0x01\n0x00 0x02\n", &emits);

    PN_CHECK_NEAR (read_at (node, 0.0), 2.0, 1e-9);

    g_object_unref (node);
}

/* A malformed line raises the node's error state without discarding the
 * lines that did parse. */
static void
test_bad_line_flags_error (void)
{
    guint   emits;
    PnNode *node = make_node ("0x00 0x01\nnonsense\n0x01 0x02 0x03\n", &emits);

    PN_CHECK      (pn_node_get_has_error (node));
    PN_CHECK_NEAR (read_at (node, 0.0), 1.0, 1e-9);
    PN_CHECK_NEAR (read_at (node, 1.0), 0.0, 1e-9);   /* rejected line */

    /* Fixing the image clears the error again. */
    g_object_set (node, "contents", "0x00 0x01\n", NULL);
    PN_CHECK (!pn_node_get_has_error (node));

    g_object_unref (node);
}

/* No numeric data.value means no address on the bus: nothing to read
 * out, so nothing is emitted. */
static void
test_non_numeric_value_is_dropped (void)
{
    guint      emits;
    PnNode    *node = make_node ("0x00 0xff\n", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "value", "0x00");
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);

    msg = pn_message_new (NULL, NULL);
    pn_message_set_string (msg, "output", "no value at all");
    pn_node_receive_message (node, msg);
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (node);
}

/* The decoded address rides along on the outgoing message. */
static void
test_address_member_is_set (void)
{
    guint      emits;
    PnNode    *node = make_node ("3 0x2a\n", &emits);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 2.7);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (msg, "address"),  3.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (msg, "value"),   42.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-prom");
    pn_test_add ("reads_programmed_cells", test_reads_programmed_cells);
    pn_test_add ("address_is_rounded",     test_address_is_rounded);
    pn_test_add ("missing_reads_zero",     test_missing_address_reads_zero);
    pn_test_add ("image_syntax",           test_image_syntax);
    pn_test_add ("last_line_wins",         test_last_line_wins);
    pn_test_add ("bad_line_flags_error",   test_bad_line_flags_error);
    pn_test_add ("non_numeric_dropped",    test_non_numeric_value_is_dropped);
    pn_test_add ("address_member_set",     test_address_member_is_set);
    return pn_test_run ();
}
