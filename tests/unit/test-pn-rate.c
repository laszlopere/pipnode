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

/* Unit tests for PnRate.  The live rate comes from a network fetch on a
 * worker thread, which the test never exercises: clearing the "url"
 * property leaves the node un-configured so no curl is ever spawned,
 * while the synchronous receive path still applies the last-known rate.
 *
 * Two surfaces are covered headlessly:
 *   - pn_currency_get_icon_name(), a pure public lookup;
 *   - receive(), which multiplies data.value by the current rate and
 *     stamps the message with "rate" and the target "currency". */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-rate.h"

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

/* Build a rate node that will never touch the network: an empty URL
 * makes it report itself un-configured, so the auto-trigger worker's
 * periodic fetch is a no-op. */
static PnNode *
make_node (Capture *cap)
{
    PnNode *node = PN_NODE (pn_rate_new ());

    g_object_set (node, "url", "", NULL);

    cap->count = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_emit), cap);
    return node;
}

static void
test_icon_name_lookup (void)
{
    /* Pure table lookup — no node required. */
    PN_CHECK_CMPSTR (pn_currency_get_icon_name (PN_CURRENCY_BTC), ==, "btc");
    PN_CHECK_CMPSTR (pn_currency_get_icon_name (PN_CURRENCY_ETH), ==, "eth");
    PN_CHECK_CMPSTR (pn_currency_get_icon_name (PN_CURRENCY_USD), ==, "usd");
    PN_CHECK_CMPSTR (pn_currency_get_icon_name (PN_CURRENCY_XRP), ==, "xrp");

    /* An out-of-range enum value falls back to the USD row rather than
     * dereferencing past the table — the documented safety net that
     * lets every caller use the result unconditionally. */
    PN_CHECK_CMPSTR (pn_currency_get_icon_name ((PnCurrency) 99999),
                     ==, "usd");
}

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
     * stored, so the conversion is deterministic. */
    g_object_set (node, "to", PN_CURRENCY_BTC, "rate", 3.0, NULL);

    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 6.0, 1e-9);   /* 2 × 3 */
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"),  3.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "BTC");

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
    g_object_set (node, "to", PN_CURRENCY_BTC, "rate", 3.0, NULL);

    pn_message_set_int (msg, "value", 4);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 12.0, 1e-9);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"),  3.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "BTC");

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
    g_object_set (node, "to", PN_CURRENCY_ETH, "rate", 5.0, NULL);

    pn_message_set_string (msg, "value", "hello");
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "value"), ==, "hello");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"), 5.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "ETH");

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
    g_object_set (node, "to", PN_CURRENCY_ETH, "rate", 5.0, NULL);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_FALSE  (pn_test_has (cap.last, "value"));
    PN_CHECK_NEAR   (pn_test_num (cap.last, "rate"), 5.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "currency"), ==, "ETH");

    g_clear_object (&cap.last);
    g_object_unref (msg);
    g_object_unref (node);
}

/* A node that has never recorded a successful fetch (empty last-update)
 * stamps every conversion as `deprecated`, and reports "Never updated"
 * as its status — the user-visible signals that the rate is not to be
 * trusted yet. */
static void
test_deprecated_without_last_update (void)
{
    Capture    cap;
    PnNode    *node = make_node (&cap);
    PnMessage *msg  = pn_message_new (NULL, NULL);
    gchar     *status = NULL;

    g_object_set (node, "to", PN_CURRENCY_BTC, "rate", 3.0, NULL);

    pn_message_set_double (msg, "value", 2.0);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK       (pn_test_bool (cap.last, "deprecated"));
    PN_CHECK_NEAR  (pn_test_num (cap.last, "value"), 6.0, 1e-9);  /* still converts */

    g_object_get (node, "status", &status, NULL);
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

    g_object_set (node, "to", PN_CURRENCY_BTC, "rate", 3.0,
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
    gchar   *status = NULL;

    g_object_set (node, "from", PN_CURRENCY_ETH, "to", PN_CURRENCY_USD,
                  "rate", 3000.0, "last-update", "2026-06-07T08:00:00+00",
                  "status", "OK", NULL);

    /* Pick a different source currency. */
    g_object_set (node, "from", PN_CURRENCY_BTC, NULL);

    g_object_get (node, "last-update", &last, "status", &status, NULL);
    PN_CHECK_CMPSTR (last, ==, "");                 /* timestamp dropped */
    PN_CHECK_CMPSTR (status, ==, "Never updated");  /* status reset */
    g_free (last);
    g_free (status);

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-rate");
    pn_test_add ("icon_name_lookup",       test_icon_name_lookup);
    pn_test_add ("default_passthrough",    test_default_rate_passes_value_through);
    pn_test_add ("applies_rate",           test_applies_configured_rate);
    pn_test_add ("converts_integer",       test_converts_integer_value);
    pn_test_add ("non_numeric_value",      test_leaves_non_numeric_value_alone);
    pn_test_add ("stamps_without_value",   test_stamps_even_without_value);
    pn_test_add ("deprecated_no_update",   test_deprecated_without_last_update);
    pn_test_add ("not_deprecated_update",  test_not_deprecated_with_last_update);
    pn_test_add ("pair_change_deprecates", test_pair_change_deprecates);
    return pn_test_run ();
}
