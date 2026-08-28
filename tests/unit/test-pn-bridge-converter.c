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

/* Unit tests for PnBridgeConverter.  Nothing here touches the network.
 *
 * Three seams make that possible.  The construct-only "autostart"
 * property is FALSE, so the auto-trigger worker thread is never spawned
 * and the input-driven request only advances when a test drives it.
 * Clearing "url" leaves the node un-configured, so a trigger that does
 * run stops before it would open a socket.  And the parse path is driven
 * directly through the PnHttp vfunc:
 *
 *     PN_HTTP_GET_CLASS (node)->emit_message (http, TRUE, 200, body, NULL)
 *
 * with the amount installed through the "amount" property, which is
 * exactly where the trigger publishes it when a real message arrives.
 * The reply bodies are trimmed from real PulseLN and ChangeNOW captures
 * — the same fixtures the Bridge Quote tests use — so the arithmetic is
 * checked against numbers the providers actually returned.
 *
 * The node emits through pn_auto_trigger_emit_on_main(), which queues
 * onto the default main context; with no main loop running the tests
 * drain it by hand (drain_main) after each call. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-bridge-converter.h"

/* ------------------------------------------------------------------ */
/*  Fixtures                                                           */
/* ------------------------------------------------------------------ */

/* A PulseLN /c/prices reply trimmed to the members a PLS->BNB quote
 * reads, with the values verbatim from a live capture. */
static const gchar *PULSELN_BODY =
"{"
"  \"PLS\": { \"perBNB\": \"54205816.695114426314830780\","
"             \"perUSD\": \"76195.975112615167745389\","
"             \"diff\": 1.77174712706904e-07,"
"             \"min\": \"76195.975113\","
"             \"max\": \"226683025.960030\" },"
"  \"BNB\": { \"perPLS\": \"0.000000018448204657\","
"             \"perUSD\": \"0.001405678942929435\" },"
"  \"F\": 0.014,"
"  \"DIFF\": 0.0135,"
"  \"PF\": { \"PLSBNB\": \"0.014000\" },"
"  \"DLN_FEE\": 4,"
"  \"DLN_LIST\": [ \"SOL\", \"TRX\" ],"
"  \"STATUS\": { \"PLS\": \"online\" },"
"  \"PS\": { \"BNB\": \"online\" }"
"}";

/* ChangeNOW v1 /exchange-amount, verbatim shape. */
static const gchar *CHANGENOW_BODY =
"{ \"estimatedAmount\": 0.0158293,"
"  \"transactionSpeedForecast\": \"10-60\","
"  \"warningMessage\": null }";

/* What the PulseLN fixture works out to for 100,000,000 PLS — the same
 * number the Bridge Quote tests assert, because it is the same
 * arithmetic in the shared back-end. */
#define PULSELN_AMOUNT 100000000.0
#define PULSELN_QUOTE  1.794437

/* ------------------------------------------------------------------ */
/*  Harness                                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    guint      count;
    PnMessage *last;
} Capture;

static void
on_emit (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;

    (void) node;
    cap->count++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

/* The node hands finished messages to the default main context.  No loop
 * runs here, so pump it until it is empty. */
static void
drain_main (void)
{
    while (g_main_context_iteration (NULL, FALSE))
        ;
}

/* Build a node that will never touch the network: no worker thread, and
 * an empty URL so even a hand-driven trigger stops short of a socket. */
static PnNode *
make_node (Capture *cap)
{
    PnNode *node = PN_NODE (g_object_new (PN_TYPE_BRIDGE_CONVERTER,
                                          "autostart", FALSE, NULL));

    g_object_set (node, "url", "", NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

/* Feed one reply straight to the parse path, then let the emission
 * land. */
static void
feed (PnNode      *node,
      gboolean     ok,
      gint         status,
      const gchar *body)
{
    PnHttp *http = PN_HTTP (node);

    PN_HTTP_GET_CLASS (http)->emit_message (http, ok, status, body, NULL);
    drain_main ();
}

/* Deliver a message carrying @value on the input port, the way a wire
 * would. */
static void
send_value (PnNode  *node,
            gdouble  value)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", value);
    PN_NODE_GET_CLASS (node)->receive (node, msg);
    g_object_unref (msg);
    drain_main ();
}

static void
clear (Capture *cap, PnNode *node)
{
    g_clear_object (&cap->last);
    g_object_unref (node);
    drain_main ();
}

/* ------------------------------------------------------------------ */
/*  Shape                                                              */
/* ------------------------------------------------------------------ */

/* The whole point of this node next to Bridge Quote: it has an input,
 * and it does not tick. */
static void
test_is_a_converter (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    PN_CHECK (pn_node_get_has_input  (node));
    PN_CHECK (pn_node_get_has_output (node));
    PN_CHECK_CMPSTR (pn_node_get_class_name (node), ==, "Bridge Converter");

    clear (&cap, node);
}

/* An idle tick — one the auto-trigger's own clock would produce — must
 * do nothing at all: no request, no message.  The worker exists to carry
 * a blocking socket off the main thread, not to poll a provider. */
static void
test_idle_tick_is_silent (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 0);

    /* And nothing was recorded either — the node has simply not been
     * asked anything yet. */
    {
        gchar *status = NULL;

        g_object_get (node, "status", &status, NULL);
        PN_CHECK_CMPSTR (status, ==, "Never converted");
        g_free (status);
    }

    clear (&cap, node);
}

/* Configuration is the pair and the endpoint.  Unlike Bridge Quote there
 * is no amount to check: the amount arrives on the wire, and a converter
 * waiting for its first message is idle, not misconfigured. */
static void
test_is_configured (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    PnHttp  *http = PN_HTTP (node);

    PN_CHECK_FALSE (PN_HTTP_GET_CLASS (http)->is_configured (http));

    g_object_set (node, "url", "https://example.invalid/quote", NULL);
    PN_CHECK (PN_HTTP_GET_CLASS (http)->is_configured (http));

    /* Quoting an asset against itself is not a swap. */
    g_object_set (node, "to", PN_CURRENCY_PLS, NULL);
    PN_CHECK_FALSE (PN_HTTP_GET_CLASS (http)->is_configured (http));
    g_object_set (node, "to", PN_CURRENCY_BNB, NULL);
    PN_CHECK (PN_HTTP_GET_CLASS (http)->is_configured (http));

    /* PulseLN does not trade XMR; ChangeNOW does, so switching provider
     * fixes the same pair.  (Changing `bridge` re-points the URL.) */
    g_object_set (node, "to", PN_CURRENCY_XMR, NULL);
    PN_CHECK_FALSE (PN_HTTP_GET_CLASS (http)->is_configured (http));
    g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
    PN_CHECK (PN_HTTP_GET_CLASS (http)->is_configured (http));

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  Conversion                                                         */
/* ------------------------------------------------------------------ */

/* The amount from the wire is what gets quoted, and the payout comes
 * back as the canonical `value`. */
static void
test_pulseln_conversion (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    g_object_set (node,
                  "bridge", PN_BRIDGE_PULSELN,
                  "from",   PN_CURRENCY_PLS,
                  "to",     PN_CURRENCY_BNB,
                  "amount", PULSELN_AMOUNT,
                  NULL);

    feed (node, TRUE, 200, PULSELN_BODY);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK        (pn_test_bool (cap.last, "swappable"));

    PN_CHECK_NEAR (pn_test_num (cap.last, "value"),  PULSELN_QUOTE, 1e-9);
    PN_CHECK_NEAR (pn_test_num (cap.last, "amount"), PULSELN_AMOUNT, 1e-6);
    PN_CHECK_NEAR (pn_test_num (cap.last, "rate"),
                   PULSELN_QUOTE / PULSELN_AMOUNT, 1e-17);

    PN_CHECK_CMPSTR (pn_test_str (cap.last, "from"),   ==, "PLS");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "to"),     ==, "BNB");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "bridge"), ==, "PulseLN");

    /* PulseLN publishes its cut and its band, so all three ride along. */
    PN_CHECK_NEAR (pn_test_num (cap.last, "fee"),        0.014,        1e-9);
    PN_CHECK_NEAR (pn_test_num (cap.last, "min_amount"), 76195.975113, 1e-6);
    PN_CHECK_NEAR (pn_test_num (cap.last, "max_amount"),
                   226683025.960030, 1e-6);

    g_object_get (node, "status", &status, NULL);
    PN_CHECK_CMPSTR (status, ==, "OK");
    g_free (status);

    /* The result is readable back off the node, which is what the
     * dialog's read-only rows show. */
    {
        gdouble quote = 0.0;

        g_object_get (node, "quote", &quote, NULL);
        PN_CHECK_NEAR (quote, PULSELN_QUOTE, 1e-9);
    }

    clear (&cap, node);
}

/* The other provider, quoting the amount directly. */
static void
test_changenow_conversion (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    /* Setting the bridge re-points the URL at ChangeNOW; blank it again
     * so the range lookup in emit_message stays off the network. */
    g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
    g_object_set (node, "url", "", "amount", 1000000.0, NULL);

    feed (node, TRUE, 200, CHANGENOW_BODY);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 0.0158293, 1e-12);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "bridge"), ==, "ChangeNOW");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "speed_forecast"), ==, "10-60");

    /* v1 folds the provider's cut into the estimate rather than
     * publishing it, so `fee` is left off rather than claimed to be 0. */
    PN_CHECK_FALSE (pn_test_has (cap.last, "fee"));

    clear (&cap, node);
}

/* Fields an upstream node set survive the conversion: the answer is
 * stamped onto the message that asked, not onto a fresh one, the way the
 * FX Converter forwards what it converts.  Driven here through the
 * unconfigured path — the only one a headless test can complete without
 * a socket — but both outcomes hand the same message to the same
 * bridge_converter_emit(). */
static void
test_upstream_fields_survive (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);   /* url is "" */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value",  PULSELN_AMOUNT);
    pn_message_set_string (msg, "wallet", "0xdead");
    PN_NODE_GET_CLASS (node)->receive (node, msg);
    g_object_unref (msg);
    drain_main ();

    /* Parked, not answered: the provider has not replied yet. */
    PN_CHECK_CMPINT (cap.count, ==, 0);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "wallet"), ==, "0xdead");
    /* The amount that was asked about is reported back alongside it. */
    PN_CHECK_NEAR   (pn_test_num (cap.last, "amount"), PULSELN_AMOUNT, 1e-6);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  The input path                                                     */
/* ------------------------------------------------------------------ */

/* A message with no numeric `data.value` has no amount to quote.  It is
 * answered here rather than sent to a provider that would only reject
 * it — and it *is* answered: silence would stall the chain. */
static void
test_missing_value_fails_locally (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    gchar     *status = NULL;

    g_object_set (node, "url", "https://example.invalid/quote", NULL);

    pn_message_set_string (msg, "output", "not a number");
    PN_NODE_GET_CLASS (node)->receive (node, msg);
    g_object_unref (msg);
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));
    PN_CHECK        (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK (strstr (pn_test_str (cap.last, "output"), "data.value") != NULL);

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "Conversion failed:"));
    g_free (status);

    /* A conversion that failed is what paints the error marker — not
     * merely never having converted anything. */
    PN_CHECK (pn_node_get_has_error (node));

    clear (&cap, node);
}

/* Zero or negative is nothing to swap, and is caught the same way. */
static void
test_non_positive_value_fails_locally (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "url", "https://example.invalid/quote", NULL);

    send_value (node, 0.0);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    send_value (node, -5.0);
    PN_CHECK_CMPINT (cap.count, ==, 2);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    clear (&cap, node);
}

/* An amount that can be quoted is parked silently — the answer is a
 * network round-trip away, and passing the message straight through
 * would look exactly like a conversion that was the identity. */
static void
test_receive_does_not_emit (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "url", "https://example.invalid/quote", NULL);
    send_value (node, PULSELN_AMOUNT);

    PN_CHECK_CMPINT (cap.count, ==, 0);

    clear (&cap, node);
}

/* Taking the request publishes its amount, so build_request, the parse
 * and the dialog all read the one number. */
static void
test_trigger_publishes_amount (void)
{
    Capture  cap;
    PnNode  *node   = make_node (&cap);
    gdouble  amount = 0.0;

    g_object_set (node, "url", "https://example.invalid/quote", NULL);
    send_value (node, 4242.0);

    g_object_get (node, "amount", &amount, NULL);
    PN_CHECK_NEAR (amount, 0.0, 1e-12);      /* not in flight yet */

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    g_object_get (node, "amount", &amount, NULL);
    PN_CHECK_NEAR (amount, 4242.0, 1e-12);

    clear (&cap, node);
}

/* An unconfigured node must still answer the message it took, or a chain
 * waiting on it stalls with no explanation anywhere. */
static void
test_unconfigured_trigger_answers (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);   /* url is "" */

    send_value (node, PULSELN_AMOUNT);
    PN_CHECK_CMPINT (cap.count, ==, 0);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));
    PN_CHECK (strstr (pn_test_str (cap.last, "output"), "endpoint") != NULL);

    clear (&cap, node);
}

/* Only one amount waits.  A converter fed faster than the network
 * answers should quote the newest amount rather than work through a
 * backlog of stale ones, so the older parked request is dropped. */
static void
test_newer_amount_supersedes (void)
{
    Capture  cap;
    PnNode  *node   = make_node (&cap);
    gdouble  amount = 0.0;

    g_object_set (node, "url", "https://example.invalid/quote", NULL);

    send_value (node, 1000.0);
    send_value (node, 2000.0);
    send_value (node, 3000.0);
    PN_CHECK_CMPINT (cap.count, ==, 0);      /* none answered yet */

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    g_object_get (node, "amount", &amount, NULL);
    PN_CHECK_NEAR (amount, 3000.0, 1e-12);   /* the newest, not the first */

    /* And the slot is empty again: a second tick finds nothing. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (node));
    g_object_get (node, "amount", &amount, NULL);
    PN_CHECK_NEAR (amount, 3000.0, 1e-12);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  Failures                                                           */
/* ------------------------------------------------------------------ */

static void
test_transport_failure (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    PnHttp  *http = PN_HTTP (node);
    gchar   *status = NULL;

    PN_HTTP_GET_CLASS (http)->emit_message (http, FALSE, 0, "",
                                            "Could not resolve host");
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (strstr (status, "Could not resolve host") != NULL);
    g_free (status);

    clear (&cap, node);
}

static void
test_unparseable_body (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    g_object_set (node, "amount", PULSELN_AMOUNT, NULL);
    feed (node, TRUE, 200, "<html>gateway timeout</html>");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "Conversion failed:"));
    g_free (status);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  Settings                                                           */
/* ------------------------------------------------------------------ */

/* Every setting that changes *what* is being converted drops the stored
 * result: it answered a different question. */
static void
test_settings_change_invalidates (void)
{
    const gchar *const knobs[] = { "bridge", "from", "to" };
    guint              i;

    for (i = 0; i < G_N_ELEMENTS (knobs); i++)
    {
        Capture  cap;
        PnNode  *node   = make_node (&cap);
        gchar   *last   = NULL;
        gchar   *status = NULL;

        /* Stand in for a conversion that landed a moment ago. */
        g_object_set (node,
                      "quote",       1.5,
                      "rate",        1.5e-08,
                      "fee",         0.014,
                      "min-amount",  100.0,
                      "max-amount",  200.0,
                      "last-update", "2026-06-07T08:00:00+00",
                      "status",      "OK",
                      NULL);

        if (g_strcmp0 (knobs[i], "bridge") == 0)
            g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
        else if (g_strcmp0 (knobs[i], "from") == 0)
            g_object_set (node, "from", PN_CURRENCY_ETH, NULL);
        else
            g_object_set (node, "to", PN_CURRENCY_ETH, NULL);

        g_object_get (node, "last-update", &last, "status", &status, NULL);
        PN_CHECK_CMPSTR (last,   ==, "");
        PN_CHECK_CMPSTR (status, ==, "Never converted");
        g_free (last);
        g_free (status);

        {
            gdouble fee = -1.0, min_amount = -1.0, max_amount = -1.0;

            g_object_get (node, "fee", &fee, "min-amount", &min_amount,
                          "max-amount", &max_amount, NULL);
            PN_CHECK_NEAR (fee,        0.0, 1e-12);
            PN_CHECK_NEAR (min_amount, 0.0, 1e-12);
            PN_CHECK_NEAR (max_amount, 0.0, 1e-12);
        }

        clear (&cap, node);
    }
}

/* Selecting a provider follows its endpoint, so the user never has to
 * paste a URL to switch bridges. */
static void
test_bridge_change_moves_url (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *url  = NULL;

    g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
    g_object_get (node, "url", &url, NULL);
    PN_CHECK_CMPSTR (url, ==, pn_bridge_get_default_url (PN_BRIDGE_CHANGENOW));
    g_free (url);

    g_object_set (node, "bridge", PN_BRIDGE_PULSELN, NULL);
    g_object_get (node, "url", &url, NULL);
    PN_CHECK_CMPSTR (url, ==, pn_bridge_get_default_url (PN_BRIDGE_PULSELN));
    g_free (url);

    clear (&cap, node);
}

/* The node has no schedule, so `period` is not a poll interval here: it
 * is pinned, and only serves as the per-request socket timeout.  A
 * hand-edited worksheet that sets it is put back. */
static void
test_period_is_pinned (void)
{
    Capture  cap;
    PnNode  *node   = make_node (&cap);
    guint    period = 0;

    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 30);

    g_object_set (node, "period", 900u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 30);

    g_object_set (node, "period", 1u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 30);

    clear (&cap, node);
}

/* A freshly loaded worksheet must not paint the node red.  Unlike the
 * polling sibling, "nothing has come through yet" is not an error state
 * here: a converter with nothing wired into it is idle, not broken. */
static void
test_fresh_node_is_not_in_error (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "url",
                  "https://api.pulseln.com/c/prices", NULL);
    g_object_set (node, "bridge", PN_BRIDGE_PULSELN, NULL);
    g_object_set (node, "from",   PN_CURRENCY_PLS, NULL);
    g_object_set (node, "to",     PN_CURRENCY_BNB, NULL);
    g_object_set (node, "amount", 100000000.0, NULL);
    g_object_set (node, "quote",  1.767541, NULL);
    drain_main ();

    PN_CHECK_FALSE (pn_node_get_has_error (node));

    clear (&cap, node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-bridge-converter");
    pn_test_add ("is_a_converter",       test_is_a_converter);
    pn_test_add ("idle_tick_silent",     test_idle_tick_is_silent);
    pn_test_add ("is_configured",        test_is_configured);
    pn_test_add ("pulseln_conversion",   test_pulseln_conversion);
    pn_test_add ("changenow_conversion", test_changenow_conversion);
    pn_test_add ("upstream_fields",      test_upstream_fields_survive);
    pn_test_add ("missing_value",        test_missing_value_fails_locally);
    pn_test_add ("non_positive_value",   test_non_positive_value_fails_locally);
    pn_test_add ("receive_is_silent",    test_receive_does_not_emit);
    pn_test_add ("trigger_publishes",    test_trigger_publishes_amount);
    pn_test_add ("unconfigured_answers", test_unconfigured_trigger_answers);
    pn_test_add ("newer_supersedes",     test_newer_amount_supersedes);
    pn_test_add ("transport_failure",    test_transport_failure);
    pn_test_add ("unparseable_body",     test_unparseable_body);
    pn_test_add ("settings_invalidate",  test_settings_change_invalidates);
    pn_test_add ("bridge_moves_url",     test_bridge_change_moves_url);
    pn_test_add ("period_pinned",        test_period_is_pinned);
    pn_test_add ("fresh_node_no_error",  test_fresh_node_is_not_in_error);
    return pn_test_run ();
}
