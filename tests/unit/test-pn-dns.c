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

/* Unit tests for PnDns.  The probe shells out to dig(1) on the worker
 * thread (real DNS traffic), so these headless tests never let that
 * path run with names configured: nodes are created with the
 * construct-only "autostart" = FALSE, and the trigger is only driven
 * when the names list resolves to zero queries (empty, or only
 * separators), which the node short-circuits before spawning dig.
 * What is covered: the server/names property round-trips (the optional
 * server must not be coerced away from empty), the names-govern-only
 * has-error gating (an empty server with names set is still
 * configured), the source contract, the 30 s default period, and that
 * no-query inputs emit nothing. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-dns.h"
#include "pn-auto-trigger.h"

static PnDns *
quiescent_dns (void)
{
    return g_object_new (PN_TYPE_DNS, "autostart", FALSE, NULL);
}

static void
test_defaults_empty (void)
{
    PnDns *dns    = quiescent_dns ();
    gchar *server = NULL;
    gchar *names  = NULL;

    g_object_get (dns, "server", &server, "names", &names, NULL);
    PN_CHECK (server == NULL || *server == '\0');
    PN_CHECK (names  == NULL || *names  == '\0');

    g_free (server);
    g_free (names);
    g_object_unref (dns);
}

static void
test_names_round_trip (void)
{
    PnDns *dns   = quiescent_dns ();
    gchar *names = NULL;

    g_object_set (dns, "names", "example.com a.test", NULL);
    g_object_get (dns, "names", &names, NULL);
    PN_CHECK_CMPSTR (names, ==, "example.com a.test");

    g_free (names);
    g_object_unref (dns);
}

static void
test_server_round_trip (void)
{
    PnDns *dns    = quiescent_dns ();
    gchar *server = NULL;

    g_object_set (dns, "server", "8.8.8.8", NULL);
    g_object_get (dns, "server", &server, NULL);
    PN_CHECK_CMPSTR (server, ==, "8.8.8.8");
    g_clear_pointer (&server, g_free);

    /* Empty server falls back to the system resolver -- it stays empty
     * and is never coerced into a stand-in host. */
    g_object_set (dns, "server", "", NULL);
    g_object_get (dns, "server", &server, NULL);
    PN_CHECK (server == NULL || *server == '\0');

    g_free (server);
    g_object_unref (dns);
}

static void
test_names_gate_error (void)
{
    PnDns *dns  = quiescent_dns ();
    PnNode *node = PN_NODE (dns);

    /* Only the names list governs the configured state; the server is
     * optional.  A fresh node (no names) is in error. */
    PN_CHECK (pn_node_get_has_error (node));

    /* Setting a server but no names leaves the node unconfigured. */
    g_object_set (dns, "server", "1.1.1.1", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    /* Names clear the error; clearing names sets it again. */
    g_object_set (dns, "names", "example.com", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (dns, "names", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_unref (dns);
}

static void
test_is_source (void)
{
    PnDns  *dns  = quiescent_dns ();
    PnNode *node = PN_NODE (dns);

    PN_CHECK_FALSE (pn_node_get_has_input  (node));
    PN_CHECK       (pn_node_get_has_output (node));

    g_object_unref (dns);
}

static void
test_default_period (void)
{
    PnDns *dns = quiescent_dns ();

    /* Polite default: one batch every 30 s. */
    PN_CHECK_CMPINT (pn_auto_trigger_get_period (PN_AUTO_TRIGGER (dns)),
                     ==, 30u);

    g_object_unref (dns);
}

static void
test_empty_names_no_emit (void)
{
    PnDns *dns   = quiescent_dns ();
    guint  emits = 0;

    g_signal_connect (dns, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* No names at all: the trigger returns before building any query,
     * so it spawns no dig and emits nothing. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (dns));
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (dns);
}

static void
test_separator_only_no_emit (void)
{
    PnDns *dns   = quiescent_dns ();
    guint  emits = 0;

    /* A names string made of nothing but separators is "configured"
     * (non-empty) but splits into zero queryable tokens, so the
     * trigger still emits nothing and runs no dig. */
    g_object_set (dns, "names", " , ,\t", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (dns)));

    g_signal_connect (dns, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (dns));
    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (dns);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-dns");
    pn_test_add ("defaults_empty",        test_defaults_empty);
    pn_test_add ("names_round_trip",      test_names_round_trip);
    pn_test_add ("server_round_trip",     test_server_round_trip);
    pn_test_add ("names_gate_error",      test_names_gate_error);
    pn_test_add ("is_source",             test_is_source);
    pn_test_add ("default_period",        test_default_period);
    pn_test_add ("empty_names_no_emit",   test_empty_names_no_emit);
    pn_test_add ("separator_only_noemit", test_separator_only_no_emit);
    return pn_test_run ();
}
