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

/* Unit tests for PnHttpsTunnelReceiver.
 *
 * This node cannot be instantiated headlessly: pn_https_tunnel_receiver_init()
 * immediately calls restart_server(), which mints (and persists to
 * XDG_DATA_HOME) a self-signed certificate and binds a localhost HTTPS
 * listening socket -- filesystem and network I/O.  Every property setter
 * likewise restarts the server.  The JSON-to-PnMessage request parser
 * (build_message_from_json) is static and only reachable through that
 * live SoupServer, so it has no headless seam either (noted in the test
 * report).
 *
 * What is fully I/O-free, and what these tests assert, is the node's
 * GObject class: registering and inspecting the class runs only
 * class_init (pure parameter-spec setup) -- it constructs no instance
 * and therefore opens no socket and writes no files.  Covered: the
 * source contract (no input, one output), the class name, and the
 * property schema -- port default/range, the primary auth-profile
 * default, the secret-tagged password, and the presence of the cert/key
 * path properties. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-https-tunnel-receiver.h"
#include "pn-node.h"

/* Borrow the class without ever building an instance: g_type_class_ref
 * runs class_init (which only installs param specs) and hands back the
 * shared class struct.  No instance => no init => no server, no cert. */
static void
test_source_contract (void)
{
    PnNodeClass *klass = g_type_class_ref (PN_TYPE_HTTPS_TUNNEL_RECEIVER);

    /* A source: it listens and emits, with no upstream input. */
    PN_CHECK_FALSE (pn_node_class_get_has_input  (klass));
    PN_CHECK       (pn_node_class_get_has_output (klass));
    PN_CHECK_CMPSTR (pn_node_class_get_class_name (klass), ==,
                     "HTTPS Tunnel Receiver");

    g_type_class_unref (klass);
}

static void
test_port_schema (void)
{
    GObjectClass   *oklass = g_type_class_ref (PN_TYPE_HTTPS_TUNNEL_RECEIVER);
    GParamSpec     *pspec  = g_object_class_find_property (oklass, "port");
    GParamSpecUInt *uspec;

    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    if (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec))
    {
        uspec = G_PARAM_SPEC_UINT (pspec);
        /* Default 8443 (unprivileged HTTPS), full 0..65535 range with 0
         * meaning "disabled". */
        PN_CHECK_CMPINT (uspec->default_value, ==, 8443u);
        PN_CHECK_CMPINT (uspec->minimum,       ==, 0u);
        PN_CHECK_CMPINT (uspec->maximum,       ==, 65535u);
    }

    g_type_class_unref (oklass);
}

static void
test_auth_profile_default (void)
{
    GObjectClass *oklass = g_type_class_ref (PN_TYPE_HTTPS_TUNNEL_RECEIVER);
    GParamSpec   *pspec  = g_object_class_find_property (oklass, "auth-profile");

    /* Empty string => follow the primary http-basic profile. */
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_STRING (pspec));
    if (pspec != NULL && G_IS_PARAM_SPEC_STRING (pspec))
        PN_CHECK_CMPSTR (G_PARAM_SPEC_STRING (pspec)->default_value, ==, "");

    g_type_class_unref (oklass);
}

static void
test_password_is_secret (void)
{
    GObjectClass *oklass = g_type_class_ref (PN_TYPE_HTTPS_TUNNEL_RECEIVER);
    GParamSpec   *pspec  = g_object_class_find_property (oklass, "password");

    /* The password is tagged secret so it is scrubbed from the saved
     * workflow file. */
    PN_CHECK (pspec != NULL && pn_param_spec_get_secret (pspec));

    g_type_class_unref (oklass);
}

static void
test_cert_key_path_props (void)
{
    GObjectClass *oklass = g_type_class_ref (PN_TYPE_HTTPS_TUNNEL_RECEIVER);

    /* The TLS identity is configurable through these two paths. */
    PN_CHECK (g_object_class_find_property (oklass, "cert-path") != NULL);
    PN_CHECK (g_object_class_find_property (oklass, "key-path")  != NULL);

    g_type_class_unref (oklass);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-https-tunnel-receiver");
    pn_test_add ("source_contract",       test_source_contract);
    pn_test_add ("port_schema",           test_port_schema);
    pn_test_add ("auth_profile_default",  test_auth_profile_default);
    pn_test_add ("password_is_secret",    test_password_is_secret);
    pn_test_add ("cert_key_path_props",   test_cert_key_path_props);
    return pn_test_run ();
}
