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

/* Unit tests for PnOllama.  receive() POSTs to a local Ollama server,
 * but only once a model is configured; with no model it returns before
 * touching the network, which is the only receive path these headless
 * tests drive.  What is covered: the property defaults (empty hostname,
 * port 11434, empty model, "5m" keep-alive) and round-trips; that the
 * empty hostname is the intended default and is NOT coerced to a
 * literal at the property layer (the loopback fallback lives in the
 * internal URL builder); the model-governs-only has-error gating; the
 * filter contract (one input, one output); and that a message received
 * with no model set emits nothing and makes no request.
 *
 * The request-body builder (build_generate_body) and reply parser
 * (parse_generate_reply) are static with no public seam, so they are
 * exercised only indirectly via the no-model short-circuit -- see the
 * test report for that limitation. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ollama.h"

static void
test_property_defaults (void)
{
    PnOllama *o        = pn_ollama_new ();
    gchar    *hostname = NULL;
    gint      port     = 0;
    gchar    *model    = NULL;
    gchar    *keep     = NULL;
    gchar    *prefix   = NULL;
    gchar    *suffix   = NULL;

    g_object_get (o,
                  "hostname",   &hostname,
                  "port",       &port,
                  "model",      &model,
                  "keep-alive", &keep,
                  "prefix",     &prefix,
                  "suffix",     &suffix,
                  NULL);

    /* Empty hostname is the documented default -- it resolves to the
     * loopback only deep inside the URL builder, never at this layer. */
    PN_CHECK (hostname == NULL || *hostname == '\0');
    PN_CHECK_CMPSTR (hostname, !=, "localhost");
    PN_CHECK_CMPINT (port, ==, 11434);
    PN_CHECK (model == NULL || *model == '\0');
    PN_CHECK_CMPSTR (keep, ==, "5m");
    PN_CHECK (prefix == NULL || *prefix == '\0');
    PN_CHECK (suffix == NULL || *suffix == '\0');

    g_free (hostname);
    g_free (model);
    g_free (keep);
    g_free (prefix);
    g_free (suffix);
    g_object_unref (o);
}

static void
test_string_round_trips (void)
{
    PnOllama *o   = pn_ollama_new ();
    gchar    *got = NULL;

    g_object_set (o, "hostname", "gpu.lan", NULL);
    g_object_get (o, "hostname", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "gpu.lan");
    g_clear_pointer (&got, g_free);

    g_object_set (o, "model", "llama3.2:latest", NULL);
    g_object_get (o, "model", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "llama3.2:latest");
    g_clear_pointer (&got, g_free);

    g_object_set (o, "keep-alive", "1h", NULL);
    g_object_get (o, "keep-alive", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "1h");
    g_clear_pointer (&got, g_free);

    g_object_set (o, "prefix", "You are terse.\n", NULL);
    g_object_get (o, "prefix", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "You are terse.\n");
    g_clear_pointer (&got, g_free);

    g_object_set (o, "suffix", "\nAnswer:", NULL);
    g_object_get (o, "suffix", &got, NULL);
    PN_CHECK_CMPSTR (got, ==, "\nAnswer:");
    g_clear_pointer (&got, g_free);

    g_object_unref (o);
}

static void
test_port_round_trip (void)
{
    PnOllama *o    = pn_ollama_new ();
    gint      port = 0;

    g_object_set (o, "port", 11500, NULL);
    g_object_get (o, "port", &port, NULL);
    PN_CHECK_CMPINT (port, ==, 11500);

    g_object_unref (o);
}

static void
test_model_gates_error (void)
{
    PnOllama *o    = pn_ollama_new ();
    PnNode   *node = PN_NODE (o);

    /* Only the model is mandatory; an empty hostname is fine.  A fresh
     * node has no model and is therefore in the error state. */
    PN_CHECK (pn_node_get_has_error (node));

    /* An empty hostname must not flip the configured state on its own. */
    g_object_set (o, "hostname", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_set (o, "model", "llama3.2:latest", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (o, "model", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_unref (o);
}

static void
test_is_filter (void)
{
    PnOllama *o    = pn_ollama_new ();
    PnNode   *node = PN_NODE (o);

    PN_CHECK (pn_node_get_has_input  (node));
    PN_CHECK (pn_node_get_has_output (node));

    g_object_unref (o);
}

static void
test_no_model_no_emit (void)
{
    PnOllama  *o     = pn_ollama_new ();
    PnMessage *msg   = pn_message_new (NULL, NULL);
    guint      emits = 0;

    g_signal_connect (o, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* With no model set, receive() returns before building any request:
     * a message in (even one carrying data.output) produces nothing
     * downstream and makes no HTTP call. */
    pn_message_set_string (msg, "output", "hello there");
    pn_node_receive_message (PN_NODE (o), msg);
    pn_node_receive_message (PN_NODE (o), msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (o);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ollama");
    pn_test_add ("property_defaults",   test_property_defaults);
    pn_test_add ("string_round_trips",  test_string_round_trips);
    pn_test_add ("port_round_trip",     test_port_round_trip);
    pn_test_add ("model_gates_error",   test_model_gates_error);
    pn_test_add ("is_filter",           test_is_filter);
    pn_test_add ("no_model_no_emit",    test_no_model_no_emit);
    return pn_test_run ();
}
