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

/* Unit tests for PnRegister: a wire-writable value latch with three
 * inputs — 0 write (silent), 1 read/clock (emits the stored word), 2
 * reset (silent, restores `initial`).  Emission is synchronous in
 * receive(), so no main loop is needed.  Headless: one node, no IO, no
 * GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-register.h"

#define IN_WRITE 0
#define IN_READ  1
#define IN_RESET 2

typedef struct
{
    guint   count;
    gdouble seen[16];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
        r->seen[r->count] = pn_test_num (message, "value");
    r->count++;
}

static PnNode *
make_node (gdouble initial, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_register_new ());
    g_object_set (node, "initial", initial, NULL);
    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

static void
feed (PnNode *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message_on_input (node, m, input);
    g_object_unref (m);
}

/* A read before any write emits the initial value, and the read port is
 * the only one that ever emits. */
static void
test_read_emits_initial (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    feed (node, IN_READ, 0.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-9);

    g_object_unref (node);
}

/* Writing latches the value silently; a subsequent read emits it. */
static void
test_write_then_read (void)
{
    Recorder rec;
    PnNode  *node = make_node (0.0, &rec);

    feed (node, IN_WRITE, 42.0);   /* silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, IN_READ, 0.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 42.0, 1e-9);

    /* Last write wins; the read carrier value is irrelevant. */
    feed (node, IN_WRITE, -7.5);
    feed (node, IN_READ, 999.0);
    PN_CHECK_CMPINT (rec.count, ==, 2);
    PN_CHECK_NEAR   (rec.seen[1], -7.5, 1e-9);

    g_object_unref (node);
}

/* Reset restores the initial value (silently); the next read sees it. */
static void
test_reset_restores_initial (void)
{
    Recorder rec;
    PnNode  *node = make_node (3.0, &rec);

    feed (node, IN_WRITE, 100.0);
    feed (node, IN_RESET, 0.0);    /* silent */
    PN_CHECK_CMPINT (rec.count, ==, 0);

    feed (node, IN_READ, 0.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 3.0, 1e-9);

    g_object_unref (node);
}

/* A write carrying no usable number leaves the latch untouched. */
static void
test_non_numeric_write_ignored (void)
{
    Recorder   rec;
    PnNode    *node = make_node (0.0, &rec);
    PnMessage *m;

    feed (node, IN_WRITE, 55.0);

    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "oops");
    pn_node_receive_message_on_input (node, m, IN_WRITE);
    g_object_unref (m);

    feed (node, IN_READ, 0.0);
    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 55.0, 1e-9);

    g_object_unref (node);
}

/* The read message carries the standard output/success members so it
 * slots straight into a Debug/Numeric sink. */
static void
test_read_sets_output_and_success (void)
{
    Recorder   rec;
    PnNode    *node = make_node (0.0, &rec);
    PnMessage *m;

    feed (node, IN_WRITE, 12.0);

    m = pn_message_new (NULL, "acc");
    pn_message_set_double (m, "value", 0.0);
    pn_node_receive_message_on_input (node, m, IN_READ);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m, "value"), 12.0, 1e-9);
    PN_CHECK        (pn_test_bool (m, "success"));
    PN_CHECK_CMPSTR (pn_test_str (m, "output"), ==, "12");
    PN_CHECK_CMPSTR (pn_message_get_topic (m), ==, "acc");

    g_object_unref (m);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-register");
    pn_test_add ("read_emits_initial",     test_read_emits_initial);
    pn_test_add ("write_then_read",        test_write_then_read);
    pn_test_add ("reset_restores_initial", test_reset_restores_initial);
    pn_test_add ("non_numeric_ignored",    test_non_numeric_write_ignored);
    pn_test_add ("read_output_success",    test_read_sets_output_and_success);
    return pn_test_run ();
}
