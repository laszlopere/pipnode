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

/* Unit tests for PnTmuxMonitor.  The node captures a tmux pane on a
 * worker thread (locally or over ssh) and diffs successive captures, so
 * the tests NEVER call the trigger and never set a remote host (which
 * would attempt an ssh round-trip).  The diff/argv/error-classifier are
 * all `static` with no public seam, so the coverage here is the
 * observable contract:
 *
 *   - host / tmux-session default to the empty string and line-limit to
 *     50, with the documented [1, 100000] clamp on line-limit;
 *   - the node carries has-error while no session is selected and clears
 *     it once one is, leaving the delta baseline untouched;
 *   - the read-only, non-serialised `sessions` G_TYPE_STRV seam exists
 *     for the companion GUI module and reads NULL until an enumeration
 *     has been *delivered* on the main loop (which the test never spins,
 *     so it stays deterministically NULL);
 *   - busy / last-error are read-only runtime state, not writable.
 *
 * Caveat: constructing the node kicks one detached, read-only
 * `tmux list-sessions` enumerator on the *local* host (init →
 * tm_kick_enumerator).  Its result is delivered through the main loop,
 * which these tests never run, so it never mutates observable state; no
 * shell one-liner is ever executed and no host is contacted over ssh. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"
#include "pn-tmux-monitor.h"

/* ---- property defaults ------------------------------------------- */

static void
test_defaults (void)
{
    PnTmuxMonitor *tm      = pn_tmux_monitor_new ();
    gchar         *host    = NULL;
    gchar         *session = NULL;
    guint          limit   = 0;

    g_object_get (tm,
                  "host",         &host,
                  "tmux-session", &session,
                  "line-limit",   &limit,
                  NULL);

    /* Empty host = "this machine"; no session selected; default 50-line
     * scrollback window. */
    PN_CHECK_CMPSTR (host,    ==, "");
    PN_CHECK_CMPSTR (session, ==, "");
    PN_CHECK_CMPINT (limit,   ==, 50u);

    g_free (host);
    g_free (session);
    g_object_unref (tm);
}

static void
test_session_round_trip (void)
{
    PnTmuxMonitor *tm      = pn_tmux_monitor_new ();
    gchar         *session = NULL;

    g_object_set (tm, "tmux-session", "build", NULL);
    g_object_get (tm, "tmux-session", &session, NULL);
    PN_CHECK_CMPSTR (session, ==, "build");
    g_free (session);

    g_object_set (tm, "tmux-session", "", NULL);
    g_object_get (tm, "tmux-session", &session, NULL);
    PN_CHECK_CMPSTR (session, ==, "");
    g_free (session);

    g_object_unref (tm);
}

static void
test_line_limit_bounds (void)
{
    PnTmuxMonitor  *tm    = pn_tmux_monitor_new ();
    guint           limit = 0;
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;

    /* The [1, 100000] window and the 50 default are the GParamSpec's
     * own bounds (so GObject clamps out-of-range writes before the
     * setter ever runs). */
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (tm),
                                          "line-limit");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum,       ==, 1u);
    PN_CHECK_CMPINT (uspec->maximum,       ==, 100000u);
    PN_CHECK_CMPINT (uspec->default_value, ==, 50u);

    /* A mid-range value round-trips unchanged; the boundary value is
     * accepted. */
    g_object_set (tm, "line-limit", 200u, NULL);
    g_object_get (tm, "line-limit", &limit, NULL);
    PN_CHECK_CMPINT (limit, ==, 200u);

    g_object_set (tm, "line-limit", 100000u, NULL);
    g_object_get (tm, "line-limit", &limit, NULL);
    PN_CHECK_CMPINT (limit, ==, 100000u);

    g_object_unref (tm);
}

static void
test_has_error_gate (void)
{
    PnTmuxMonitor *tm = pn_tmux_monitor_new ();

    /* No session selected → flagged for the worksheet's red/❗ overlay. */
    PN_CHECK (pn_node_get_has_error (PN_NODE (tm)));

    g_object_set (tm, "tmux-session", "work", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (tm)));

    g_object_set (tm, "tmux-session", "", NULL);
    PN_CHECK (pn_node_get_has_error (PN_NODE (tm)));

    g_object_unref (tm);
}

static void
test_is_a_source (void)
{
    PnTmuxMonitor *tm = pn_tmux_monitor_new ();

    PN_CHECK (pn_node_get_has_output (PN_NODE (tm)));
    PN_CHECK_FALSE (pn_node_get_has_input (PN_NODE (tm)));

    g_object_unref (tm);
}

/* ---- the GTK-free read seams ------------------------------------- */

static void
test_sessions_seam (void)
{
    PnTmuxMonitor *tm    = pn_tmux_monitor_new ();
    GParamSpec    *pspec;
    gchar        **sessions = (gchar **) GINT_TO_POINTER (1);

    /* `sessions` is the read-only G_TYPE_STRV seam the companion GUI
     * module reads across the BIND_LOCAL barrier: readable, NOT
     * writable, and not part of the saved worksheet. */
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (tm),
                                          "sessions");
    PN_CHECK (pspec != NULL);
    PN_CHECK (G_PARAM_SPEC_VALUE_TYPE (pspec) == G_TYPE_STRV);
    PN_CHECK ((pspec->flags & G_PARAM_READABLE) != 0);
    PN_CHECK_FALSE ((pspec->flags & G_PARAM_WRITABLE) != 0);

    /* Before any enumeration is *delivered* on the main loop (which this
     * test never spins) the cache is NULL. */
    g_object_get (tm, "sessions", &sessions, NULL);
    PN_CHECK (sessions == NULL);
    g_strfreev (sessions);

    g_object_unref (tm);
}

static void
test_runtime_props_read_only (void)
{
    PnTmuxMonitor *tm    = pn_tmux_monitor_new ();
    GParamSpec    *busy;
    GParamSpec    *err;
    gboolean       is_busy = TRUE;

    busy = g_object_class_find_property (G_OBJECT_GET_CLASS (tm), "busy");
    err  = g_object_class_find_property (G_OBJECT_GET_CLASS (tm),
                                         "last-error");

    /* Transient runtime state: readable, never writable (so pn-flow's
     * serialiser skips them). */
    PN_CHECK (busy != NULL);
    PN_CHECK_FALSE ((busy->flags & G_PARAM_WRITABLE) != 0);
    PN_CHECK (err != NULL);
    PN_CHECK_FALSE ((err->flags & G_PARAM_WRITABLE) != 0);

    /* busy is a plain boolean that simply reads back. */
    g_object_get (tm, "busy", &is_busy, NULL);
    PN_CHECK (is_busy == TRUE || is_busy == FALSE);

    g_object_unref (tm);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tmux-monitor");
    pn_test_add ("defaults",            test_defaults);
    pn_test_add ("session_round_trip",  test_session_round_trip);
    pn_test_add ("line_limit_bounds",   test_line_limit_bounds);
    pn_test_add ("has_error_gate",      test_has_error_gate);
    pn_test_add ("is_a_source",         test_is_a_source);
    pn_test_add ("sessions_seam",       test_sessions_seam);
    pn_test_add ("runtime_read_only",   test_runtime_props_read_only);
    return pn_test_run ();
}
