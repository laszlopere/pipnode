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

/* Unit tests for PnFiat.  Nothing here touches the network.
 *
 * Two seams make that possible.  Clearing the "url" property leaves the
 * node un-configured, so the auto-trigger worker's periodic fetch is a
 * no-op for the whole life of the test.  The parse path is then driven
 * directly through the PnHttp vfunc:
 *
 *     PN_HTTP_GET_CLASS (node)->emit_message (http, TRUE, 200, body, NULL)
 *
 * with bodies trimmed from real replies of the default endpoint, so the
 * reader is checked against the shape the provider actually sends.
 *
 * The fetch path posts its canvas refresh to the default main context;
 * with no main loop running the tests drain it by hand (drain_main)
 * after each call. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-fiat.h"

/* A syntactically valid endpoint that is never contacted: every node
 * under test is built quiescent, so no worker thread exists to fetch
 * it.  It is here only to make is_configured() true. */
#define PN_TEST_FIAT_URL "https://api.frankfurter.dev/v1/latest"

/* A real reply, verbatim, for ?base=USD&symbols=EUR. */
static const gchar *BODY_USD_EUR =
"{\"amount\":1.0,\"base\":\"USD\",\"date\":\"2026-09-11\","
" \"rates\":{\"EUR\":0.86266}}";

/* The same call answered without the rate table — what an endpoint
 * that accepted the request but knows nothing of the pair sends. */
static const gchar *BODY_NO_RATES =
"{\"amount\":1.0,\"base\":\"USD\",\"date\":\"2026-09-11\"}";

/* A reply whose table omits the currency that was asked for. */
static const gchar *BODY_WRONG_SYMBOL =
"{\"amount\":1.0,\"base\":\"USD\",\"date\":\"2026-09-11\","
" \"rates\":{\"HUF\":314.4}}";

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

/* The fetch path hands its canvas refresh to the default main context.
 * No loop runs here, so pump it until it is empty. */
static void
drain_main (void)
{
    while (g_main_context_iteration (NULL, FALSE))
        ;
}

/* Build a converter that will never touch the network: an empty URL
 * makes it report itself un-configured, so the auto-trigger worker's
 * periodic fetch is a no-op.  The parse path is driven by hand. */
static PnNode *
make_node (Capture *cap)
{
    PnNode *node = PN_NODE (pn_fiat_new ());

    g_object_set (node, "url", "", NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

/* Feed one reply straight to the parse path, then let the refresh land. */
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

static gchar *
status_of (PnNode *node)
{
    gchar *status = NULL;

    g_object_get (node, "status", &status, NULL);
    return status;
}

/* ------------------------------------------------------------------ */
/*  Currency table                                                     */
/* ------------------------------------------------------------------ */

static void
test_currency_table (void)
{
    /* Pure table lookups — no node required. */
    PN_CHECK_CMPSTR (pn_fiat_currency_get_code (PN_FIAT_CURRENCY_EUR),
                     ==, "EUR");
    PN_CHECK_CMPSTR (pn_fiat_currency_get_code (PN_FIAT_CURRENCY_HUF),
                     ==, "HUF");
    PN_CHECK_CMPSTR (pn_fiat_currency_get_name (PN_FIAT_CURRENCY_HUF),
                     ==, "Hungarian forint");
    PN_CHECK_CMPSTR (pn_fiat_currency_get_name (PN_FIAT_CURRENCY_JPY),
                     ==, "Japanese yen");

    /* An out-of-range enum value falls back to the euro row rather than
     * dereferencing past the table — the documented safety net that
     * lets every caller use the result unconditionally. */
    PN_CHECK_CMPSTR (pn_fiat_currency_get_code ((PnFiatCurrency) 99999),
                     ==, "EUR");
    PN_CHECK_CMPSTR (pn_fiat_currency_get_name ((PnFiatCurrency) 99999),
                     ==, "Euro");
}

/* ------------------------------------------------------------------ */
/*  Conversion (receive path)                                          */
/* ------------------------------------------------------------------ */

static void
test_default_rate_passes_value_through (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);          /* default to=USD, rate=1.0 */
    PnMessage *msg  = pn_message_new (NULL, NULL);

    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 2.0, 1e-9);   /* ×1.0 */
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"),  1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "USD");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_applies_configured_rate (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A known rate stands in for whatever the last fetch would have
     * stored, so the conversion is deterministic: 250 EUR at 390.5
     * forints to the euro. */
    g_object_set (node, "from", PN_FIAT_CURRENCY_EUR,
                        "to",   PN_FIAT_CURRENCY_HUF,
                        "rate", 390.5, NULL);

    pn_message_set_double (msg, "value", 250.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 97625.0, 1e-6);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"),  390.5,   1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "HUF");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_converts_integer_value (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* An integer data.value (stored as a JSON int, not a double) must
     * convert just like a double — receive() accepts both numeric
     * types.  4 × 3 = 12, emitted back as a double. */
    g_object_set (node, "to", PN_FIAT_CURRENCY_GBP, "rate", 3.0, NULL);

    pn_message_set_int (msg, "value", 4);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 12.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "GBP");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_leaves_non_numeric_value_alone (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* A non-numeric data.value is not something to convert: it passes
     * through untouched (no multiply, no type clobber) while the
     * conversion metadata is still stamped on alongside it. */
    g_object_set (node, "to", PN_FIAT_CURRENCY_CHF, "rate", 5.0, NULL);

    pn_message_set_string (msg, "value", "hello");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "value"), ==, "hello");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"), 5.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "CHF");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_stamps_even_without_value (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    /* No numeric data.value: nothing to convert, but the message still
     * passes through stamped with the conversion metadata. */
    g_object_set (node, "to", PN_FIAT_CURRENCY_SEK, "rate", 5.0, NULL);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_has (cap.last, "value"));
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"), 5.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "SEK");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Cache state                                                        */
/* ------------------------------------------------------------------ */

/* A node that has never recorded a successful fetch (empty last-update)
 * stamps every conversion as `deprecated`, and reports "Never updated"
 * as its status — the user-visible signals that the rate is not to be
 * trusted yet. */
static void
test_deprecated_without_last_update (void)
{
    Capture    cap;
    PnNode    *node   = make_node (&cap);
    PnMessage *msg    = pn_message_new (NULL, NULL);
    gchar     *status;

    g_object_set (node, "to", PN_FIAT_CURRENCY_HUF, "rate", 390.5, NULL);

    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK       (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK_NEAR  (pn_test_num (cap.last, "value"), 781.0, 1e-6); /* still converts */

    status = status_of (node);
    PN_CHECK_CMPSTR (status, ==, "Never updated");
    g_free (status);

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

/* A recorded last-update timestamp clears the deprecated flag. */
static void
test_not_deprecated_with_last_update (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);

    g_object_set (node, "to", PN_FIAT_CURRENCY_HUF, "rate", 390.5,
                  "last-update", "2026-06-07T08:00:00+00", NULL);

    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "deprecated"));

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

/* Changing the currency pair deprecates the cached rate at once: the
 * last-update timestamp is dropped and the status falls back to "Never
 * updated" so a stale outcome is not left next to the new pair. */
static void
test_pair_change_deprecates (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *last = NULL;
    gchar   *status;

    g_object_set (node, "from", PN_FIAT_CURRENCY_EUR,
                        "to",   PN_FIAT_CURRENCY_USD,
                        "rate", 1.16,
                        "last-update", "2026-06-07T08:00:00+00",
                        "status", "OK", NULL);

    /* Pick a different source currency. */
    g_object_set (node, "from", PN_FIAT_CURRENCY_GBP, NULL);

    g_object_get (node, "last-update", &last, NULL);
    status = status_of (node);
    PN_CHECK_CMPSTR (last,   ==, "");               /* timestamp dropped */
    PN_CHECK_CMPSTR (status, ==, "Never updated");  /* status reset */
    g_free (last);
    g_free (status);

    g_object_unref (node);
}

/* Reloading a saved worksheet must not leave the node marked in error.
 * pn-flow replays the properties bag in file order — `from` / `to`
 * before `last-update` — so a saved pair that differs from the
 * constructor default first trips the pair-change branch (which drops
 * the timestamp and paints the error marker) and only then restores the
 * cached timestamp.  The node is built quiescent (autostart off) with a
 * plausible URL so it counts as configured without any worker thread
 * ever reaching the network. */
static void
test_load_order_clears_error_marker (void)
{
    PnNode *node = PN_NODE (g_object_new (PN_TYPE_FIAT,
                                          "autostart", FALSE,
                                          "url", PN_TEST_FIAT_URL,
                                          NULL));

    /* Property bag, in the order pn-flow applies it. */
    g_object_set (node, "from", PN_FIAT_CURRENCY_HUF, NULL);
    PN_CHECK (pn_node_get_has_error (node));        /* pair change deprecates */

    g_object_set (node, "to", PN_FIAT_CURRENCY_EUR, NULL);
    g_object_set (node, "rate", 0.0025608, NULL);
    g_object_set (node, "last-update", "2026-06-07T08:00:00+00", NULL);

    PN_CHECK_FALSE (pn_node_get_has_error (node));  /* cache restored */

    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Configuration gate and period floor                                */
/* ------------------------------------------------------------------ */

static gboolean
configured (PnNode *node)
{
    PnHttp *http = PN_HTTP (node);

    return PN_HTTP_GET_CLASS (http)->is_configured (http);
}

static void
test_is_configured (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    /* An empty URL is the unconfigured state make_node() puts us in. */
    PN_CHECK_FALSE (configured (node));

    g_object_set (node, "url", PN_TEST_FIAT_URL, NULL);
    PN_CHECK (configured (node));

    /* Converting a currency into itself asks the endpoint to confirm
     * that one euro is one euro, every period, forever. */
    g_object_set (node, "to", PN_FIAT_CURRENCY_EUR, NULL);
    PN_CHECK_FALSE (configured (node));

    g_object_unref (node);
}

/* The ECB fixes once a working day, so the node starts at the base
 * class's hourly ceiling and refuses to poll faster than every ten
 * minutes.  A deserialiser or a spinner can still try to go below. */
static void
test_period_floor (void)
{
    Capture  cap;
    PnNode  *node   = make_node (&cap);
    guint    period = 0;

    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 3600);

    g_object_set (node, "period", 5u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 600);

    /* Above the floor the user's choice stands. */
    g_object_set (node, "period", 900u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 900);

    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Reply parsing (fetch path)                                         */
/* ------------------------------------------------------------------ */

static void
test_parses_reply (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gdouble  rate = 0.0;
    gchar   *last = NULL;
    gchar   *status;

    g_object_set (node, "from", PN_FIAT_CURRENCY_USD,
                        "to",   PN_FIAT_CURRENCY_EUR, NULL);
    feed (node, TRUE, 200, BODY_USD_EUR);

    g_object_get (node, "rate", &rate, "last-update", &last, NULL);
    status = status_of (node);

    PN_CHECK_NEAR (rate, 0.86266, 1e-9);
    PN_CHECK      (last != NULL && *last != '\0');   /* timestamp stamped */
    /* The fixing date the reply carried is quoted in the status: a rate
     * read on a Sunday is Friday's rate, and the user comparing it
     * against a bank's board wants to see which day it is. */
    PN_CHECK_CMPSTR (status, ==, "OK (reference rate of 2026-09-11)");

    /* A message arriving now converts with the fetched rate and is no
     * longer flagged deprecated. */
    {
        PnMessage *msg = pn_message_new (NULL, NULL);

        pn_message_set_double (msg, "value", 100.0);
        pn_node_receive_message (node, msg);

        PN_CHECK_NEAR  (pn_test_num (cap.last, "value"), 86.266, 1e-6);
        PN_CHECK_FALSE (pn_test_bool (cap.last, "deprecated"));
        g_object_unref (msg);
    }

    g_free (last);
    g_free (status);
    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* The fetch path never emits a message of its own: the rate is internal
 * state, and a downstream sink expects one message out per message in. */
static void
test_fetch_emits_nothing (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "from", PN_FIAT_CURRENCY_USD,
                        "to",   PN_FIAT_CURRENCY_EUR, NULL);
    feed (node, TRUE, 200, BODY_USD_EUR);

    PN_CHECK_CMPINT (cap.count, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* Every way a fetch can fail records why, and none of them touches the
 * cached rate: a converter that cannot refresh keeps converting with
 * the last rate it trusted, visibly deprecated. */
static void
test_failed_fetch_keeps_rate (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gdouble  rate = 0.0;
    gchar   *status;

    g_object_set (node, "from", PN_FIAT_CURRENCY_USD,
                        "to",   PN_FIAT_CURRENCY_EUR,
                        "rate", 0.85,
                        "last-update", "2026-06-07T08:00:00+00", NULL);

    /* An HTTP error status: the body is an API error object, not a
     * rate table, so it must not be parsed as one. */
    feed (node, TRUE, 429, "{\"message\":\"rate limited\"}");
    status = status_of (node);
    PN_CHECK_CMPSTR (status, ==,
                     "Update failed: exchange-rate API returned HTTP 429");
    g_free (status);

    /* No response at all. */
    PN_HTTP_GET_CLASS (PN_HTTP (node))->emit_message (
            PN_HTTP (node), FALSE, 0, NULL, "Could not resolve host");
    drain_main ();
    status = status_of (node);
    PN_CHECK_CMPSTR (status, ==,
                     "Update failed: Request failed: Could not resolve host");
    g_free (status);

    /* A 200 with no rate table at all. */
    feed (node, TRUE, 200, BODY_NO_RATES);
    status = status_of (node);
    PN_CHECK_CMPSTR (status, ==, "Update failed: no rate table in the reply");
    g_free (status);

    /* A 200 whose table answers about some other currency. */
    feed (node, TRUE, 200, BODY_WRONG_SYMBOL);
    status = status_of (node);
    PN_CHECK_CMPSTR (status, ==,
                     "Update failed: no usable USD/EUR rate in the reply");
    g_free (status);

    /* Not JSON at all — a captive portal or a proxy error page. */
    feed (node, TRUE, 200, "<html>nope</html>");
    status = status_of (node);
    PN_CHECK (g_str_has_prefix (status,
                                "Update failed: could not parse the "
                                "exchange-rate reply:"));
    g_free (status);

    /* Through all of that the cached rate stood. */
    g_object_get (node, "rate", &rate, NULL);
    PN_CHECK_NEAR (rate, 0.85, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-fiat");
    pn_test_add ("currency_table",         test_currency_table);
    pn_test_add ("default_passthrough",    test_default_rate_passes_value_through);
    pn_test_add ("applies_rate",           test_applies_configured_rate);
    pn_test_add ("converts_integer",       test_converts_integer_value);
    pn_test_add ("non_numeric_value",      test_leaves_non_numeric_value_alone);
    pn_test_add ("stamps_without_value",   test_stamps_even_without_value);
    pn_test_add ("deprecated_no_update",   test_deprecated_without_last_update);
    pn_test_add ("not_deprecated_update",  test_not_deprecated_with_last_update);
    pn_test_add ("pair_change_deprecates", test_pair_change_deprecates);
    pn_test_add ("load_order_no_error",    test_load_order_clears_error_marker);
    pn_test_add ("is_configured",          test_is_configured);
    pn_test_add ("period_floor",           test_period_floor);
    pn_test_add ("parses_reply",           test_parses_reply);
    pn_test_add ("fetch_emits_nothing",    test_fetch_emits_nothing);
    pn_test_add ("failed_fetch_keeps",     test_failed_fetch_keeps_rate);
    return pn_test_run ();
}
