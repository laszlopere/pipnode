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

/* Unit tests for PnNetIo's /proc/net/dev parser.  Canned kernel text
 * stands in for the live file (the "stub" for /proc).  Two header lines
 * are skipped; each data row is "iface: rx_bytes ... tx_bytes ..." with
 * rx_bytes in the first column past the colon and tx_bytes in the ninth.
 * An empty selector sums every non-loopback interface (label joined with
 * '+'); a named selector matches exactly, loopback included. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"

/* Defined (non-static) in lib/pn-net-io.c — see the note there. */
gboolean pn_net_io_parse_net_dev (const gchar *text,
                                  const gchar *want,
                                  guint64 *out_rx,
                                  guint64 *out_tx,
                                  gchar **out_matched_label);

/* Columns past the colon: rx_bytes, then 7 rx_* columns, then tx_bytes. */
static const gchar NETDEV[] =
        "Inter-|   Receive                    |  Transmit\n"
        " face |bytes packets errs drop fifo frame compressed multicast|bytes\n"
        "    lo:  1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
        "  eth0: 5000 50 0 0 0 0 0 0 3000 30 0 0 0 0 0 0\n"
        " wlan0: 2000 20 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n";

static void
test_sum_skips_loopback (void)
{
    guint64  rx = 0, tx = 0;
    gchar   *label = NULL;

    /* Empty selector -> every non-loopback interface, summed. */
    PN_CHECK (pn_net_io_parse_net_dev (NETDEV, "", &rx, &tx, &label));
    PN_CHECK_CMPINT (rx, ==, 5000 + 2000);
    PN_CHECK_CMPINT (tx, ==, 3000 + 1000);
    PN_CHECK_CMPSTR (label, ==, "eth0+wlan0");

    g_free (label);
}

static void
test_named_interface (void)
{
    guint64  rx = 0, tx = 0;
    gchar   *label = NULL;

    PN_CHECK (pn_net_io_parse_net_dev (NETDEV, "eth0", &rx, &tx, &label));
    PN_CHECK_CMPINT (rx, ==, 5000);
    PN_CHECK_CMPINT (tx, ==, 3000);
    PN_CHECK_CMPSTR (label, ==, "eth0");

    g_free (label);
}

static void
test_named_loopback_allowed (void)
{
    guint64  rx = 0, tx = 0;
    gchar   *label = NULL;

    /* The loopback skip only applies to the "all interfaces" sum; an
     * explicit request for "lo" still matches. */
    PN_CHECK (pn_net_io_parse_net_dev (NETDEV, "lo", &rx, &tx, &label));
    PN_CHECK_CMPINT (rx, ==, 1000);
    PN_CHECK_CMPSTR (label, ==, "lo");

    g_free (label);
}

static void
test_unknown_interface_fails (void)
{
    guint64  rx, tx;
    gchar   *label = (gchar *) 0x1;

    PN_CHECK_FALSE (pn_net_io_parse_net_dev (NETDEV, "eth9", &rx, &tx, &label));
    PN_CHECK (label == NULL);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-net-io");
    pn_test_add ("sum_skips_loopback",     test_sum_skips_loopback);
    pn_test_add ("named_interface",        test_named_interface);
    pn_test_add ("named_loopback_ok",      test_named_loopback_allowed);
    pn_test_add ("unknown_fails",          test_unknown_interface_fails);
    return pn_test_run ();
}
