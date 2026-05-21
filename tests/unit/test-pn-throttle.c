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

/* Unit tests for PnThrottle: forward the first message, then drop any
 * that arrive within the configured interval.  Back-to-back in-process
 * sends are microseconds apart, so the second always falls inside the
 * window — no wall-clock waiting required. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-throttle.h"

static PnNode *
make_node (guint interval_seconds, guint *out_emits)
{
    PnNode *node = g_object_new (PN_TYPE_THROTTLE,
                                 "interval", interval_seconds,
                                 NULL);

    *out_emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), out_emits);
    return node;
}

static void
send (PnNode *node)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 1.0);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

static void
test_first_passes_burst_dropped (void)
{
    guint   emits;
    PnNode *node = make_node (60, &emits);

    send (node);                      /* first -> forwarded */
    PN_CHECK_CMPINT (emits, ==, 1);

    send (node);                      /* within 60s -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    send (node);                      /* still within window -> dropped */
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

static void
test_default_interval_drops_burst (void)
{
    guint   emits;
    PnNode *node = make_node (1, &emits);   /* minimum / default interval */

    send (node);
    send (node);
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-throttle");
    pn_test_add ("first_passes_burst_dropped", test_first_passes_burst_dropped);
    pn_test_add ("default_interval_drops",     test_default_interval_drops_burst);
    return pn_test_run ();
}
