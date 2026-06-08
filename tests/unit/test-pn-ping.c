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

/* Unit tests for PnPing.  The actual probe shells out to ping(8) on the
 * worker thread, which is real network I/O, so these headless tests
 * never let that path run: nodes are created with the construct-only
 * "autostart" = FALSE so no worker thread ticks, and the trigger is
 * driven only when the host is empty (the node's "configuration
 * required" short-circuit returns before spawning ping).  What is
 * covered: the host property round-trips (including the empty string,
 * which the project convention forbids coercing to "localhost"), the
 * unconfigured-host has-error gating, the source contract (no input,
 * one output), the relaxed 5 s default period, and that an empty host
 * emits nothing. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ping.h"
#include "pn-auto-trigger.h"

/* A quiescent ping node: autostart off, so no worker thread spawns and
 * nothing reaches ping(8) unless we explicitly drive a tick. */
static PnPing *
quiescent_ping (void)
{
    return g_object_new (PN_TYPE_PING, "autostart", FALSE, NULL);
}

static void
test_host_default_empty (void)
{
    PnPing *ping = quiescent_ping ();
    gchar  *host = NULL;

    /* A fresh ping has no host.  The project convention is that an
     * empty host is NOT coerced to "localhost" -- it stays the
     * unconfigured value. */
    g_object_get (ping, "host", &host, NULL);
    PN_CHECK (host == NULL || *host == '\0');
    PN_CHECK_CMPSTR (host, !=, "localhost");

    g_free (host);
    g_object_unref (ping);
}

static void
test_host_round_trip (void)
{
    PnPing *ping = quiescent_ping ();
    gchar  *host = NULL;

    g_object_set (ping, "host", "router.example", NULL);
    g_object_get (ping, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "router.example");
    g_clear_pointer (&host, g_free);

    /* The empty string round-trips as the unconfigured state and is
     * never turned into a loopback default. */
    g_object_set (ping, "host", "", NULL);
    g_object_get (ping, "host", &host, NULL);
    PN_CHECK (host == NULL || *host == '\0');
    PN_CHECK_CMPSTR (host, !=, "localhost");

    g_free (host);
    g_object_unref (ping);
}

static void
test_unconfigured_gating (void)
{
    PnPing *ping = quiescent_ping ();
    PnNode *node = PN_NODE (ping);

    /* No host -> the node advertises the error/needs-configuration
     * state; a host clears it; clearing the host sets it again. */
    PN_CHECK (pn_node_get_has_error (node));

    g_object_set (ping, "host", "10.0.0.1", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (ping, "host", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_unref (ping);
}

static void
test_is_source (void)
{
    PnPing *ping = quiescent_ping ();
    PnNode *node = PN_NODE (ping);

    /* A pure source: probes on a timer, emits downstream, takes no
     * upstream input. */
    PN_CHECK_FALSE (pn_node_get_has_input  (node));
    PN_CHECK       (pn_node_get_has_output (node));

    g_object_unref (ping);
}

static void
test_default_period (void)
{
    PnPing *ping = quiescent_ping ();

    /* The ping node relaxes the base-class 1 s cadence to 5 s so a
     * freshly-configured node does not immediately flood. */
    PN_CHECK_CMPINT (pn_auto_trigger_get_period (PN_AUTO_TRIGGER (ping)),
                     ==, 5u);

    g_object_unref (ping);
}

static void
test_empty_host_no_emit (void)
{
    PnPing *ping  = quiescent_ping ();
    guint   emits = 0;

    g_signal_connect (ping, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Drive exactly one trigger on this thread with no host set.  The
     * "configuration required" short-circuit must return before any
     * ping(8) is spawned, so nothing is emitted -- and crucially no
     * network call happens. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (ping));

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (ping);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ping");
    pn_test_add ("host_default_empty",   test_host_default_empty);
    pn_test_add ("host_round_trip",      test_host_round_trip);
    pn_test_add ("unconfigured_gating",  test_unconfigured_gating);
    pn_test_add ("is_source",            test_is_source);
    pn_test_add ("default_period",       test_default_period);
    pn_test_add ("empty_host_no_emit",   test_empty_host_no_emit);
    return pn_test_run ();
}
