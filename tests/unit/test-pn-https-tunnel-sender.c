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

/* Unit tests for PnHttpsTunnelSender.  Constructing the node is I/O-free
 * (it only creates an idle SoupSession), but receive() POSTs to the
 * configured URL -- so these headless tests only ever feed it a message
 * after clearing the URL, which trips the "unconfigured sink" guard
 * that drops the message before any socket work.  What is covered: the
 * property defaults (loopback URL, verify-tls off, primary auth
 * profile) and round-trips; the sink contract (one input, no output);
 * the URL-governs has-error gating; and that an empty URL silently
 * drops an incoming message with no emission and no network call.
 *
 * The JSON body builder (build_request_body) and the Basic-auth header
 * builder (build_basic_auth_header) are static with no public seam, so
 * they cannot be asserted on directly here -- see the test report. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-https-tunnel-sender.h"

#define DEFAULT_URL "https://127.0.0.1:8443/"

static void
test_property_defaults (void)
{
    PnHttpsTunnelSender *s    = pn_https_tunnel_sender_new ();
    gchar               *url  = NULL;
    gchar               *prof = NULL;
    gboolean             tls  = TRUE;

    g_object_get (s,
                  "url",          &url,
                  "auth-profile", &prof,
                  "verify-tls",   &tls,
                  NULL);

    /* A fresh sender targets the loopback receiver on its default port
     * so two same-machine pipnodes talk with zero config, follows the
     * primary http-basic profile, and trusts the receiver's self-signed
     * cert (verify-tls off) out of the box. */
    PN_CHECK_CMPSTR (url, ==, DEFAULT_URL);
    PN_CHECK_CMPSTR (prof, ==, "");
    PN_CHECK_FALSE  (tls);

    g_free (url);
    g_free (prof);
    g_object_unref (s);
}

static void
test_property_round_trips (void)
{
    PnHttpsTunnelSender *s   = pn_https_tunnel_sender_new ();
    gchar               *got = NULL;
    gboolean             tls = FALSE;

    g_object_set (s, "url", "https://peer.example:8443/", NULL);
    g_object_get (s, "url", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "https://peer.example:8443/");
    g_clear_pointer (&got, g_free);

    g_object_set (s, "username", "alice", NULL);
    g_object_get (s, "username", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "alice");
    g_clear_pointer (&got, g_free);

    g_object_set (s, "password", "s3cr3t", NULL);
    g_object_get (s, "password", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "s3cr3t");
    g_clear_pointer (&got, g_free);

    g_object_set (s, "auth-profile", "other-id", NULL);
    g_object_get (s, "auth-profile", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "other-id");
    g_clear_pointer (&got, g_free);

    g_object_set (s, "verify-tls", TRUE, NULL);
    g_object_get (s, "verify-tls", &tls, NULL);
    PN_CHECK (tls);

    g_object_unref (s);
}

static void
test_password_is_secret (void)
{
    GParamSpec *pspec = g_object_class_find_property (
            g_type_class_peek (PN_TYPE_HTTPS_TUNNEL_SENDER), "password");

    /* The password is tagged secret so it is scrubbed from the saved
     * workflow file. */
    PN_CHECK (pspec != NULL && pn_param_spec_get_secret (pspec));
}

static void
test_is_sink (void)
{
    PnHttpsTunnelSender *s    = pn_https_tunnel_sender_new ();
    PnNode              *node = PN_NODE (s);

    /* A sink: it consumes a message and POSTs it, with nothing
     * downstream. */
    PN_CHECK       (pn_node_get_has_input  (node));
    PN_CHECK_FALSE (pn_node_get_has_output (node));

    g_object_unref (s);
}

static void
test_url_gates_error (void)
{
    PnHttpsTunnelSender *s    = pn_https_tunnel_sender_new ();
    PnNode              *node = PN_NODE (s);

    /* The non-empty default URL means a fresh node is configured. */
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (s, "url", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_set (s, "url", DEFAULT_URL, NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_unref (s);
}

static void
test_empty_url_drops_message (void)
{
    PnHttpsTunnelSender *s     = pn_https_tunnel_sender_new ();
    PnMessage           *msg   = pn_message_new (NULL, "demo/topic");
    guint                emits = 0;

    /* Clear the URL so receive() hits the unconfigured-sink guard and
     * returns before constructing any SoupMessage -- no POST, no
     * network. */
    g_object_set (s, "url", "", NULL);

    g_signal_connect (s, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_message_set_string (msg, "output", "payload");
    pn_node_receive_message (PN_NODE (s), msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (s);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-https-tunnel-sender");
    pn_test_add ("property_defaults",     test_property_defaults);
    pn_test_add ("property_round_trips",  test_property_round_trips);
    pn_test_add ("password_is_secret",    test_password_is_secret);
    pn_test_add ("is_sink",               test_is_sink);
    pn_test_add ("url_gates_error",       test_url_gates_error);
    pn_test_add ("empty_url_drops_msg",   test_empty_url_drops_message);
    return pn_test_run ();
}
