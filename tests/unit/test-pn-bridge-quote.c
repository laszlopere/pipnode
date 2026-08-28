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

/* Unit tests for PnBridgeQuote.  Nothing here touches the network.
 *
 * Two seams make that possible.  Clearing the "url" property leaves the
 * node un-configured, so the auto-trigger worker's periodic fetch is a
 * no-op for the whole life of the test.  The parse path is then driven
 * directly through the PnHttp vfunc:
 *
 *     PN_HTTP_GET_CLASS (node)->emit_message (http, TRUE, 200, body, NULL)
 *
 * with bodies trimmed from real PulseLN and ChangeNOW replies, so the
 * quote arithmetic is checked against numbers the providers actually
 * returned rather than invented ones.
 *
 * The node emits through pn_auto_trigger_emit_on_main(), which queues onto
 * the default main context; with no main loop running the tests drain it
 * by hand (drain_main) after each call. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-bridge-quote.h"

/* ------------------------------------------------------------------ */
/*  Fixtures                                                           */
/* ------------------------------------------------------------------ */

/* A PulseLN /c/prices reply trimmed to the members a PLS->BNB quote
 * reads, with the values verbatim from a live capture.  Note the mix of
 * string and JSON-number encodings in the same object — that is how the
 * provider really sends it, and the reason every read goes through a
 * tolerant number parser. */
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

/* The same capture with the payout pool taken down. */
static const gchar *PULSELN_BODY_OFFLINE =
"{"
"  \"PLS\": { \"perBNB\": \"54205816.695114426314830780\","
"             \"perUSD\": \"76195.975112615167745389\","
"             \"diff\": 1.77174712706904e-07 },"
"  \"BNB\": { \"perPLS\": \"0.000000018448204657\","
"             \"perUSD\": \"0.001405678942929435\" },"
"  \"F\": 0.014,"
"  \"DIFF\": 0.0135,"
"  \"STATUS\": { \"PLS\": \"online\" },"
"  \"PS\": { \"BNB\": \"offline\" }"
"}";

/* ChangeNOW v1 /exchange-amount, verbatim shape. */
static const gchar *CHANGENOW_BODY =
"{ \"estimatedAmount\": 0.0158293,"
"  \"transactionSpeedForecast\": \"10-60\","
"  \"warningMessage\": null }";

static const gchar *CHANGENOW_BODY_MAX =
"{ \"error\": \"max_amount_exceeded\" }";

/* The quote the PulseLN fixture works out to for 100,000,000 PLS:
 *
 *   rate = BNB.perPLS - PLS.diff * PLS.perUSD / PLS.perBNB
 *        = 1.8199153894128332e-08
 *   out  = 100000000 * rate, less the 1.4% pair fee, rounded to 1e-6
 *
 * which is what the swap page shows for the same input. */
#define PULSELN_AMOUNT 100000000.0
#define PULSELN_QUOTE  1.794437
#define PULSELN_RATE   (PULSELN_QUOTE / PULSELN_AMOUNT)

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

/* Build a node that will never touch the network: an empty URL makes it
 * report itself un-configured, so the auto-trigger worker's periodic
 * fetch is a no-op.  The parse path is driven by hand instead. */
static PnNode *
make_node (Capture *cap)
{
    PnNode *node = PN_NODE (pn_bridge_quote_new ());

    g_object_set (node, "url", "", NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

/* Feed one reply straight to the parse path, then let the emission land. */
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

static void
clear (Capture *cap, PnNode *node)
{
    g_clear_object (&cap->last);
    g_object_unref (node);
    /* The node's dispose joins its worker; drain whatever it left. */
    drain_main ();
}

/* ------------------------------------------------------------------ */
/*  Provider tables                                                    */
/* ------------------------------------------------------------------ */

static void
test_ticker_lookup (void)
{
    /* Pure table lookups — no node required. */
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                           PN_CURRENCY_PLS), ==, "PLS");
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                           PN_CURRENCY_BNB), ==, "BNB");
    /* Polygon rebranded MATIC to POL, and PulseLN follows the new name. */
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                           PN_CURRENCY_MATIC), ==, "POL");

    /* ChangeNOW's legacy tickers are lowercase and mostly the obvious
     * identity, but three of ours only exist chain-qualified. */
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_CHANGENOW,
                                           PN_CURRENCY_PLS), ==, "pls");
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_CHANGENOW,
                                           PN_CURRENCY_BNB), ==, "bnbbsc");
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_CHANGENOW,
                                           PN_CURRENCY_DOT), ==, "dotbsc");
    PN_CHECK_CMPSTR (pn_bridge_get_ticker (PN_BRIDGE_CHANGENOW,
                                           PN_CURRENCY_USDT), ==, "usdterc20");

    /* Not listed: PulseLN trades no Monero, and USD is a fiat pivot for
     * the FX Converter rather than a bridgeable asset anywhere. */
    PN_CHECK (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                    PN_CURRENCY_XMR) == NULL);
    PN_CHECK (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                    PN_CURRENCY_USD) == NULL);
    PN_CHECK (pn_bridge_get_ticker (PN_BRIDGE_CHANGENOW,
                                    PN_CURRENCY_USD) == NULL);

    /* An out-of-range currency is listed nowhere, which is exactly what
     * a NULL already means to every caller. */
    PN_CHECK (pn_bridge_get_ticker (PN_BRIDGE_PULSELN,
                                    (PnCurrency) 99999) == NULL);
}

static void
test_bridge_table (void)
{
    PN_CHECK_CMPSTR (pn_bridge_get_display_name (PN_BRIDGE_PULSELN),
                     ==, "PulseLN");
    PN_CHECK_CMPSTR (pn_bridge_get_display_name (PN_BRIDGE_CHANGENOW),
                     ==, "ChangeNOW");

    PN_CHECK_CMPSTR (pn_bridge_get_default_url (PN_BRIDGE_PULSELN),
                     ==, "https://api.pulseln.com/c/prices");
    PN_CHECK_CMPSTR (pn_bridge_get_default_url (PN_BRIDGE_CHANGENOW),
                     ==, "https://api.changenow.io/v1/exchange-amount");

    /* Out of range falls back to the first row rather than reading past
     * the table, so callers can use the result unconditionally. */
    PN_CHECK_CMPSTR (pn_bridge_get_display_name ((PnBridge) 99999),
                     ==, "PulseLN");
}

/* ------------------------------------------------------------------ */
/*  Configuration gate                                                 */
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

    g_object_set (node, "url", "https://example.invalid/quote", NULL);
    PN_CHECK (configured (node));            /* PLS -> BNB, 1e6, PulseLN */

    /* Quoting an asset against itself is not a swap. */
    g_object_set (node, "to", PN_CURRENCY_PLS, NULL);
    PN_CHECK_FALSE (configured (node));
    g_object_set (node, "to", PN_CURRENCY_BNB, NULL);

    /* A zero amount has no quote to ask for. */
    g_object_set (node, "amount", 0.0, NULL);
    PN_CHECK_FALSE (configured (node));
    g_object_set (node, "amount", 1000000.0, NULL);
    PN_CHECK (configured (node));

    /* PulseLN does not trade XMR, so the pair is unsupported there even
     * though every other setting is fine. */
    g_object_set (node, "to", PN_CURRENCY_XMR, NULL);
    PN_CHECK_FALSE (configured (node));

    /* ChangeNOW does trade it, so switching provider fixes the same pair.
     * (Changing `bridge` re-points the URL, so set it back afterwards.) */
    g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
    PN_CHECK (configured (node));

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  PulseLN back-end                                                   */
/* ------------------------------------------------------------------ */

static void
test_pulseln_quote (void)
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

    /* One tick, one message. */
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "deprecated"));

    /* `value` is the payout, the canonical number of the contract. */
    PN_CHECK_NEAR (pn_test_num (cap.last, "value"),  PULSELN_QUOTE, 1e-9);
    PN_CHECK_NEAR (pn_test_num (cap.last, "rate"),   PULSELN_RATE,  1e-17);
    PN_CHECK_NEAR (pn_test_num (cap.last, "amount"), PULSELN_AMOUNT, 1e-6);

    PN_CHECK_CMPSTR (pn_test_str (cap.last, "from"),   ==, "PLS");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "to"),     ==, "BNB");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "bridge"), ==, "PulseLN");

    /* PulseLN publishes its cut and its per-asset limits, so both ride
     * along on the message. */
    PN_CHECK_NEAR (pn_test_num (cap.last, "fee"),        0.014,        1e-9);
    PN_CHECK_NEAR (pn_test_num (cap.last, "min_amount"), 76195.975113, 1e-6);
    PN_CHECK_NEAR (pn_test_num (cap.last, "max_amount"),
                   226683025.960030, 1e-4);

    /* v1 ChangeNOW has no forecast to offer and neither does PulseLN. */
    PN_CHECK_FALSE (pn_test_has (cap.last, "speed_forecast"));

    /* The same numbers are readable back off the node for the dialog. */
    PN_CHECK_NEAR (pn_test_num (cap.last, "value"), PULSELN_QUOTE, 1e-9);
    g_object_get (node, "status", &status, NULL);
    PN_CHECK_CMPSTR (status, ==, "OK");
    g_free (status);

    clear (&cap, node);
}

/* A closed payout pool stops the swap, not the pricing.  PulseLN's own
 * page gates only its swap *button* on PS[to] and still shows the rate
 * ("Swaps To X Temporarily Offline"), so the node prices it too: a
 * monitor that blanked its history whenever a pool drained would be
 * useless exactly when it mattered.  The closure rides on `swappable`,
 * `warning` and the status line instead. */
static void
test_pulseln_pool_offline (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    g_object_set (node, "amount", PULSELN_AMOUNT, NULL);
    feed (node, TRUE, 200, PULSELN_BODY_OFFLINE);

    PN_CHECK_CMPINT (cap.count, ==, 1);

    /* The fetch worked and the rate is real, so this is not a failure. */
    PN_CHECK       (pn_test_bool (cap.last, "success"));
    PN_CHECK_FALSE (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK_NEAR  (pn_test_num (cap.last, "value"), PULSELN_QUOTE, 1e-9);

    /* But the route is shut, and that is stated rather than implied. */
    PN_CHECK_FALSE (pn_test_bool (cap.last, "swappable"));
    PN_CHECK (strstr (pn_test_str (cap.last, "warning"), "offline") != NULL);

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "OK"));      /* not "Update failed" */
    PN_CHECK (strstr (status, "offline") != NULL);
    g_free (status);

    clear (&cap, node);
}

/* The healthy case says so too, so `swappable` is a real signal rather
 * than a field that only ever appears when something is wrong. */
static void
test_open_route_is_swappable (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "amount", PULSELN_AMOUNT, NULL);
    feed (node, TRUE, 200, PULSELN_BODY);

    PN_CHECK       (pn_test_bool (cap.last, "swappable"));
    PN_CHECK_FALSE (pn_test_has (cap.last, "warning"));

    clear (&cap, node);
}

/* The currency pair has to appear in the reply at all. */
static void
test_pulseln_missing_pair (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    /* SOL is a PulseLN asset, but this trimmed body only carries PLS and
     * BNB — the same shape a provider-side change would produce. */
    g_object_set (node, "to", PN_CURRENCY_SOL, NULL);
    feed (node, TRUE, 200, PULSELN_BODY);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "Update failed:"));
    g_free (status);

    clear (&cap, node);
}

/* An amount outside the provider's band is priced the same way but will
 * not be executed, so it lands on `swappable` too.  The limits float —
 * PulseLN derives them from a USD band divided by the live price — so a
 * node can drift out of range with nothing reconfigured, which is
 * precisely why this has to be reported rather than assumed. */
static void
test_amount_outside_limits (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    /* The fixture's band is 76,195.98 .. 226,683,025.96 PLS. */
    g_object_set (node, "amount", 1000000000.0, NULL);   /* over the top */
    feed (node, TRUE, 200, PULSELN_BODY);

    PN_CHECK       (pn_test_bool (cap.last, "success"));      /* still priced */
    PN_CHECK       (pn_test_num  (cap.last, "value") > 0.0);
    PN_CHECK_FALSE (pn_test_bool (cap.last, "swappable"));
    PN_CHECK (strstr (pn_test_str (cap.last, "warning"), "maximum") != NULL);

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "OK"));
    g_free (status);

    clear (&cap, node);

    /* ... and the same at the bottom of the band. */
    node = make_node (&cap);
    g_object_set (node, "amount", 100.0, NULL);
    feed (node, TRUE, 200, PULSELN_BODY);

    PN_CHECK_FALSE (pn_test_bool (cap.last, "swappable"));
    PN_CHECK (strstr (pn_test_str (cap.last, "warning"), "minimum") != NULL);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  ChangeNOW back-end                                                 */
/* ------------------------------------------------------------------ */

static void
test_changenow_quote (void)
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
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "deprecated"));

    PN_CHECK_NEAR (pn_test_num (cap.last, "value"), 0.0158293, 1e-12);
    PN_CHECK_NEAR (pn_test_num (cap.last, "rate"),
                   0.0158293 / 1000000.0, 1e-17);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "bridge"), ==, "ChangeNOW");

    /* The provider's own speed estimate is passed through. */
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "speed_forecast"), ==, "10-60");

    /* v1 folds the provider's cut into the estimate rather than
     * publishing it, so `fee` is left off rather than claimed to be 0. */
    PN_CHECK_FALSE (pn_test_has (cap.last, "fee"));

    clear (&cap, node);
}

/* ChangeNOW answers an over-cap amount with HTTP 400 and a terse error
 * code; the node has to turn that into a sentence rather than surfacing
 * "HTTP 400", which says nothing about what to change. */
static void
test_changenow_max_exceeded (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    g_object_set (node, "bridge", PN_BRIDGE_CHANGENOW, NULL);
    g_object_set (node, "url", "", "amount", 100000000.0, NULL);

    feed (node, TRUE, 400, CHANGENOW_BODY_MAX);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (strstr (status, "ChangeNOW") != NULL);
    /* With no range fetched (the URL is blank) it falls back to naming
     * the raw code, which is still more use than a bare status line. */
    PN_CHECK (strstr (status, "max_amount_exceeded") != NULL);
    g_free (status);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  Transport and parse failures                                       */
/* ------------------------------------------------------------------ */

static void
test_transport_failure (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    PnHttp  *http;
    gchar   *status = NULL;

    http = PN_HTTP (node);
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

    feed (node, TRUE, 200, "<html>gateway timeout</html>");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (g_str_has_prefix (status, "Update failed:"));
    g_free (status);

    clear (&cap, node);
}

static void
test_empty_body (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    feed (node, TRUE, 200, "");

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));

    g_object_get (node, "status", &status, NULL);
    PN_CHECK (strstr (status, "empty reply") != NULL);
    g_free (status);

    clear (&cap, node);
}

/* ------------------------------------------------------------------ */
/*  Cache invalidation and the period floor                            */
/* ------------------------------------------------------------------ */

/* A node with no recorded success stamps its quote `deprecated` and
 * reports "Never updated" — the user-visible signals that the cached
 * number is not to be trusted. */
static void
test_deprecated_without_last_update (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);
    gchar   *status = NULL;

    g_object_get (node, "status", &status, NULL);
    PN_CHECK_CMPSTR (status, ==, "Never updated");
    g_free (status);

    clear (&cap, node);
}

/* Every setting that changes *what* is being quoted invalidates the
 * cache: the stored number answers a different question now. */
static void
test_settings_change_invalidates (void)
{
    const gchar *const knobs[] = { "bridge", "from", "to", "amount" };
    guint              i;

    for (i = 0; i < G_N_ELEMENTS (knobs); i++)
    {
        Capture  cap;
        PnNode  *node   = make_node (&cap);
        gchar   *last   = NULL;
        gchar   *status = NULL;

        /* Stand in for a fetch that landed a moment ago. */
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
        else if (g_strcmp0 (knobs[i], "to") == 0)
            g_object_set (node, "to", PN_CURRENCY_ETH, NULL);
        else
            g_object_set (node, "amount", 5000.0, NULL);

        g_object_get (node, "last-update", &last, "status", &status, NULL);
        PN_CHECK_CMPSTR (last,   ==, "");               /* timestamp dropped */
        PN_CHECK_CMPSTR (status, ==, "Never updated");  /* status reset      */
        g_free (last);
        g_free (status);

        /* The limits and the fee described the old provider or pair. */
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

/* The 60-second floor protects an undocumented endpoint from a spinner
 * dialled to 1 or a hand-edited worksheet. */
static void
test_period_floor (void)
{
    Capture  cap;
    PnNode  *node   = make_node (&cap);
    guint    period = 0;

    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 300);       /* the default */

    g_object_set (node, "period", 1u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 60);        /* clamped back up */

    /* A legitimate longer period is left alone. */
    g_object_set (node, "period", 900u, NULL);
    g_object_get (node, "period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 900);

    clear (&cap, node);
}

/* Unlike the FX Converter — which holds its rate privately and only
 * speaks when a message arrives — this node is a source, so a tick that
 * skips the fetch because the cache is still fresh must *still* emit.
 * Staying silent would leave a reopened worksheet's displays blank. */
static void
test_fresh_cache_still_emits (void)
{
    Capture       cap;
    PnNode       *node = make_node (&cap);
    GDateTime    *now  = g_date_time_new_now_utc ();
    gchar        *iso  = g_date_time_format_iso8601 (now);

    g_object_set (node,
                  "quote",       2.5,
                  "rate",        2.5e-08,
                  "amount",      100000000.0,
                  "last-update", iso,        /* fetched just now */
                  "status",      "OK",
                  NULL);

    PN_AUTO_TRIGGER_GET_CLASS (node)->trigger (PN_AUTO_TRIGGER (node));
    drain_main ();

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK        (pn_test_bool (cap.last, "success"));
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 2.5, 1e-9);

    g_free (iso);
    g_date_time_unref (now);
    clear (&cap, node);
}

/* Regression: a worksheet that carries a good cached quote must load
 * clean, not red.
 *
 * pn-flow.c applies a node's saved properties one at a time, in file
 * order, so `amount` lands well before the `last-update` further down
 * the bag.  Reacting to each setting the instant it arrives meant the
 * node was judged "no successful fetch yet" half way through the load
 * and painted with the error marker, and nothing repainted it once the
 * timestamp finally arrived — a node with a perfectly good quote sat red
 * until the next real fetch.  The reaction is deferred to an idle
 * instead, so this asserts on the state *after* the main loop turns. */
static void
test_load_order_leaves_no_error (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    /* Exactly the order pn-flow writes: the settings, then the cached
     * fetch state, then the timestamp. */
    g_object_set (node, "url", "https://api.pulseln.com/c/prices", NULL);
    g_object_set (node, "bridge",      PN_BRIDGE_PULSELN, NULL);
    g_object_set (node, "from",        PN_CURRENCY_PLS, NULL);
    g_object_set (node, "to",          PN_CURRENCY_BNB, NULL);
    g_object_set (node, "amount",      100000000.0, NULL);
    g_object_set (node, "quote",       1.767541, NULL);
    g_object_set (node, "rate",        1.767541e-08, NULL);
    g_object_set (node, "fee",         0.014, NULL);
    g_object_set (node, "min-amount",  77605.215026, NULL);
    g_object_set (node, "max-amount",  77605215.026373, NULL);

    {
        GDateTime *now = g_date_time_new_now_local ();
        gchar     *iso = g_date_time_format_iso8601 (now);

        g_object_set (node, "last-update", iso, "status", "OK", NULL);
        g_free (iso);
        g_date_time_unref (now);
    }

    /* Mid-load the node is allowed to look deprecated; what matters is
     * where it settles once the batch is complete. */
    drain_main ();

    PN_CHECK_FALSE (pn_node_get_has_error (node));

    /* And the restored state is intact — the load must not have eaten
     * the timestamp or the status it just applied. */
    {
        gchar *last = NULL, *status = NULL;

        g_object_get (node, "last-update", &last, "status", &status, NULL);
        PN_CHECK (last != NULL && *last != '\0');
        PN_CHECK_CMPSTR (status, ==, "OK");
        g_free (last);
        g_free (status);
    }

    clear (&cap, node);
}

/* The other half of the same rule: a node whose cache really is empty
 * still has to paint the error marker, or the signal means nothing. */
static void
test_empty_cache_still_marks_error (void)
{
    Capture  cap;
    PnNode  *node = make_node (&cap);

    g_object_set (node, "url", "https://api.pulseln.com/c/prices", NULL);
    g_object_set (node, "amount", 5000000.0, NULL);
    drain_main ();

    PN_CHECK (pn_node_get_has_error (node));

    clear (&cap, node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-bridge-quote");
    pn_test_add ("ticker_lookup",         test_ticker_lookup);
    pn_test_add ("bridge_table",          test_bridge_table);
    pn_test_add ("is_configured",         test_is_configured);
    pn_test_add ("pulseln_quote",         test_pulseln_quote);
    pn_test_add ("pulseln_pool_offline",  test_pulseln_pool_offline);
    pn_test_add ("open_route_swappable",  test_open_route_is_swappable);
    pn_test_add ("amount_outside_limits", test_amount_outside_limits);
    pn_test_add ("pulseln_missing_pair",  test_pulseln_missing_pair);
    pn_test_add ("changenow_quote",       test_changenow_quote);
    pn_test_add ("changenow_max",         test_changenow_max_exceeded);
    pn_test_add ("transport_failure",     test_transport_failure);
    pn_test_add ("unparseable_body",      test_unparseable_body);
    pn_test_add ("empty_body",            test_empty_body);
    pn_test_add ("deprecated_no_update",  test_deprecated_without_last_update);
    pn_test_add ("settings_invalidate",   test_settings_change_invalidates);
    pn_test_add ("bridge_moves_url",      test_bridge_change_moves_url);
    pn_test_add ("period_floor",          test_period_floor);
    pn_test_add ("fresh_cache_emits",     test_fresh_cache_still_emits);
    pn_test_add ("load_order_no_error",   test_load_order_leaves_no_error);
    pn_test_add ("empty_cache_error",     test_empty_cache_still_marks_error);
    return pn_test_run ();
}
