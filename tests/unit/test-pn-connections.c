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

/* Unit tests for PnConnections' pure output parser.  The node counts a
 * host's established TCP sockets by running a counting shell script
 * (locally, or over SSH), which is not reproducible headless, so the
 * testable contract is pn_connections_parse_count: it pulls the leading
 * non-negative integer out of the script's stdout, skipping leading
 * whitespace, and rejects anything without a leading digit (empty buffer,
 * NULL, error chatter).  Canned strings stand in for the script output. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-connections.h"

static void
test_plain_number (void)
{
    guint v = 12345;

    /* The script prints the count followed by a newline. */
    PN_CHECK (pn_connections_parse_count ("42\n", &v));
    PN_CHECK_CMPINT (v, ==, 42u);

    /* Zero is a legitimate count, not a parse failure. */
    PN_CHECK (pn_connections_parse_count ("0\n", &v));
    PN_CHECK_CMPINT (v, ==, 0u);
}

static void
test_leading_whitespace (void)
{
    guint v = 0;

    /* Leading spaces / tabs / newlines are skipped before the digits. */
    PN_CHECK (pn_connections_parse_count ("   \t\n 137\n", &v));
    PN_CHECK_CMPINT (v, ==, 137u);
}

static void
test_stops_at_non_digit (void)
{
    guint v = 0;

    /* Only the leading run of digits is consumed; trailing chatter (a
     * stray awk warning glued onto the line) is ignored. */
    PN_CHECK (pn_connections_parse_count ("256 connections\n", &v));
    PN_CHECK_CMPINT (v, ==, 256u);
}

static void
test_rejects_unparseable (void)
{
    guint v = 999;

    /* No leading digit -> failure, and the out value is left untouched. */
    PN_CHECK_FALSE (pn_connections_parse_count ("", &v));
    PN_CHECK_FALSE (pn_connections_parse_count (NULL, &v));
    PN_CHECK_FALSE (pn_connections_parse_count (
                        "awk: cannot open file\n", &v));
    PN_CHECK_CMPINT (v, ==, 999u);
}

static void
test_property_round_trip (void)
{
    PnConnections *node = pn_connections_new ();
    gchar         *host = NULL;

    /* Default hostname is the empty "this machine" marker. */
    g_object_get (node, "hostname", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    /* A set value round-trips back out unchanged. */
    g_object_set (node, "hostname", "mini02", NULL);
    g_object_get (node, "hostname", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "mini02");
    g_free (host);

    /* NULL collapses back to the empty default rather than crashing. */
    g_object_set (node, "hostname", NULL, NULL);
    g_object_get (node, "hostname", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-connections");
    pn_test_add ("plain_number",        test_plain_number);
    pn_test_add ("leading_whitespace",  test_leading_whitespace);
    pn_test_add ("stops_at_non_digit",  test_stops_at_non_digit);
    pn_test_add ("rejects_unparseable", test_rejects_unparseable);
    pn_test_add ("property_round_trip", test_property_round_trip);
    return pn_test_run ();
}
