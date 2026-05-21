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

/* ------------------------------------------------------------------ */
/*  pntest                                                             */
/*                                                                     */
/*  Tiny unit-test harness built on GLib.  The default output is the   */
/*  test unit name followed by one line per test case with its check   */
/*  tally:                                                             */
/*                                                                     */
/*      pn-expression                                                  */
/*        eval_arithmetic         4 checks, 0 failed                   */
/*        eval_siblings           3 checks, 0 failed                   */
/*        total: 7 checks, 0 failed                                    */
/*                                                                     */
/*  Pass --verbose to additionally list every check (PASS/FAIL, with   */
/*  the source location of failures) under its case.  These tests      */
/*  construct nodes and feed them messages in process: they never      */
/*  touch the filesystem, the network, or open a GUI (gtk_init() is    */
/*  never called), so they are safe to run on a headless box.          */
/* ------------------------------------------------------------------ */

#ifndef PNTEST_H
#define PNTEST_H

#include <glib.h>
#include <math.h>

#include "pn-message.h"

G_BEGIN_DECLS

typedef void (*PnTestFunc) (void);

/* Width of the test-case name column.  Shared by every test binary so
 * the per-case lines line up across units, not just within one.  Bump
 * it if a test-case name ever grows wider than this. */
#define PN_TEST_NAME_WIDTH 28

/* Wraps g_test_init(); @suite is the name printed in the summary. */
void pn_test_init (int *argc, char ***argv, const gchar *suite);

/* Register a named test function (run in registration order). */
void pn_test_add (const gchar *name, PnTestFunc func);

/* Run every registered test, print the summary, and return non-zero
 * when at least one test failed — usable directly as main()'s result
 * so the automake test driver sees the right exit status. */
int pn_test_run (void);

/* Records one check.  Prefer the PN_CHECK* macros below. */
void pn_test_check_ (gboolean ok, const gchar *expr,
                     const gchar *file, int line);

#define PN_CHECK(expr) \
    pn_test_check_ ((expr) ? TRUE : FALSE, #expr, __FILE__, __LINE__)

#define PN_CHECK_FALSE(expr) \
    pn_test_check_ ((expr) ? FALSE : TRUE, "!(" #expr ")", __FILE__, __LINE__)

#define PN_CHECK_CMPINT(a, op, b) \
    pn_test_check_ ((a) op (b), #a " " #op " " #b, __FILE__, __LINE__)

#define PN_CHECK_CMPSTR(a, op, b) \
    pn_test_check_ (g_strcmp0 ((a), (b)) op 0, \
                    #a " " #op " " #b, __FILE__, __LINE__)

#define PN_CHECK_NEAR(a, b, eps) \
    pn_test_check_ (fabs ((double) (a) - (double) (b)) <= (eps), \
                    #a " ~= " #b, __FILE__, __LINE__)

/* ---- read-only accessors for a message's data bag ----
 *
 * Borrowed lookups that never warn on a missing member, so a test can
 * assert on absence as easily as on a value. */

static inline JsonNode *
pn_test_member (PnMessage *m, const gchar *key)
{
    return m != NULL ? pn_message_get_member (m, key) : NULL;
}

static inline gboolean
pn_test_has (PnMessage *m, const gchar *key)
{
    return pn_test_member (m, key) != NULL;
}

static inline gdouble
pn_test_num (PnMessage *m, const gchar *key)
{
    JsonNode *n = pn_test_member (m, key);
    return (n != NULL && JSON_NODE_HOLDS_VALUE (n))
           ? json_node_get_double (n) : G_MAXDOUBLE;
}

static inline const gchar *
pn_test_str (PnMessage *m, const gchar *key)
{
    JsonNode *n = pn_test_member (m, key);
    return (n != NULL && JSON_NODE_HOLDS_VALUE (n))
           ? json_node_get_string (n) : NULL;
}

static inline gboolean
pn_test_bool (PnMessage *m, const gchar *key)
{
    JsonNode *n = pn_test_member (m, key);
    return (n != NULL && JSON_NODE_HOLDS_VALUE (n))
           ? json_node_get_boolean (n) : FALSE;
}

/* Counts emitted messages; wire it to a node's "message" signal with
 * a guint counter as the user-data:
 *
 *     guint emits = 0;
 *     g_signal_connect (node, "message",
 *                       G_CALLBACK (pn_test_count_emits), &emits);
 */
static inline void
pn_test_count_emits (PnNode *node, PnMessage *message, gpointer counter)
{
    (void) node;
    (void) message;
    (*(guint *) counter)++;
}

G_END_DECLS

#endif /* PNTEST_H */
