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

/* Unit tests for PnHttp's output parser.  The node spawns curl, which is
 * not reproducible headless, so the testable contract is the pure helper
 * the trigger feeds curl's stdout through: pn_http_split_body_and_status
 * recovers the response body and the HTTP status code that curl's
 * --write-out appends after a sentinel.  Canned stdout blobs stand in
 * for the spawn.  This pins the logic half the headless/core split
 * (TODO #23) keeps loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-http.h"

/* Mirror of the private PN_HTTP_STATUS_SENTINEL in pn-http.c: curl's
 * --write-out emits this immediately before "%{http_code}", so the
 * parser splits the stream here.  The leading newline is part of it. */
#define SENTINEL "\n--PN-HTTP-STATUS--"

static void
test_null_input (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    pn_http_split_body_and_status (NULL, &body, &status);
    PN_CHECK_CMPSTR (body, ==, "");
    PN_CHECK_CMPINT (status, ==, 0);

    g_free (body);
}

static void
test_no_sentinel_is_all_body (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    /* A spawn that produced no status line (the sentinel never arrived)
     * hands the whole stream back as the body, status unknown (0). */
    pn_http_split_body_and_status ("plain response", &body, &status);
    PN_CHECK_CMPSTR (body, ==, "plain response");
    PN_CHECK_CMPINT (status, ==, 0);

    g_free (body);
}

static void
test_body_and_status (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    pn_http_split_body_and_status ("the body" SENTINEL "200", &body, &status);
    PN_CHECK_CMPSTR (body, ==, "the body");
    PN_CHECK_CMPINT (status, ==, 200);

    g_free (body);
}

static void
test_empty_body_with_status (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    /* A 404 with no payload: the sentinel sits at the very start, so the
     * body is empty but the status still parses. */
    pn_http_split_body_and_status (SENTINEL "404", &body, &status);
    PN_CHECK_CMPSTR (body, ==, "");
    PN_CHECK_CMPINT (status, ==, 404);

    g_free (body);
}

static void
test_splits_on_last_sentinel (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    /* If the body itself echoes the sentinel text, the split must happen
     * on the *last* occurrence so the real status is recovered and the
     * echoed copy stays part of the body. */
    pn_http_split_body_and_status ("a" SENTINEL "b" SENTINEL "500",
                                   &body, &status);
    PN_CHECK_CMPSTR (body, ==, "a" SENTINEL "b");
    PN_CHECK_CMPINT (status, ==, 500);

    g_free (body);
}

static void
test_non_numeric_status (void)
{
    gchar *body   = NULL;
    gint   status = -1;

    /* A garbled status field parses as 0 rather than crashing; the body
     * is still recovered. */
    pn_http_split_body_and_status ("body" SENTINEL "oops", &body, &status);
    PN_CHECK_CMPSTR (body, ==, "body");
    PN_CHECK_CMPINT (status, ==, 0);

    g_free (body);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-http");
    pn_test_add ("null_input",            test_null_input);
    pn_test_add ("no_sentinel_all_body",  test_no_sentinel_is_all_body);
    pn_test_add ("body_and_status",       test_body_and_status);
    pn_test_add ("empty_body_with_status", test_empty_body_with_status);
    pn_test_add ("splits_on_last_sentinel", test_splits_on_last_sentinel);
    pn_test_add ("non_numeric_status",    test_non_numeric_status);
    return pn_test_run ();
}
