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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-flow.h"
#include "pn-node-factory.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>

#include <signal.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  pipnode-run                                                        */
/*                                                                     */
/*  Headless companion to the pipnode GUI.  Loads a saved worksheet   */
/*  through #PnFlow (which carries no GTK runtime dependency — see    */
/*  pn-flow.h), runs the message-dispatch GMainLoop, and forwards    */
/*  debug-message / status-message text from PnDebug nodes to        */
/*  stdout / stderr respectively so a shell pipeline can capture the */
/*  flow's output the way it would any other Unix tool.               */
/*                                                                     */
/*  Save paths are deliberately never wired up: the runner is         */
/*  read-only on disk by construction, so a scheduled execution       */
/*  cannot accidentally rewrite the source-of-truth worksheet file.   */
/* ------------------------------------------------------------------ */

static void
on_status_message (
        PnFlow      *flow,
        const gchar *text,
        gpointer     user_data)
{
    (void) flow;
    (void) user_data;

    /* Status bar text — progress / state info — goes to stderr so
     * the stdout stream stays a clean "data only" channel that a
     * caller can pipe into the next tool without filtering. */
    g_printerr ("%s\n", text != NULL ? text : "");
}

static void
on_debug_message (
        PnFlow      *flow,
        const gchar *text,
        gpointer     user_data)
{
    (void) flow;
    (void) user_data;

    g_print ("%s\n", text != NULL ? text : "");
    /* libc would otherwise block-buffer stdout when it is connected
     * to a pipe, so a long-running flow's lines would only land at
     * 4 KB boundaries.  An explicit flush per message keeps a
     * `pipnode-run flow.json | head` interaction live. */
    fflush (stdout);
}

static gboolean
on_quit_signal (gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static gboolean
on_timeout (gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

int
main (
        int    argc,
        char **argv)
{
    GOptionContext *ctx;
    gint            opt_timeout   = 0;
    gboolean        opt_version   = FALSE;
    gchar         **opt_remaining = NULL;
    GError         *error         = NULL;
    PnFlow         *flow;
    GMainLoop      *loop;
    const gchar    *path;
    int             status        = 0;

    static GOptionEntry entries[] = {
        { "timeout", 't', 0, G_OPTION_ARG_INT, NULL,
          "Exit gracefully after N seconds (default: run until "
          "interrupted)", "N" },
        { "version", 'V', 0, G_OPTION_ARG_NONE, NULL,
          "Print version and exit", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL,
          NULL, "WORKSHEET" },
        { NULL }
    };
    entries[0].arg_data = &opt_timeout;
    entries[1].arg_data = &opt_version;
    entries[2].arg_data = &opt_remaining;

    /* %NULL parameter_string keeps "Usage:" from printing the
     * positional name twice — the G_OPTION_REMAINING entry below
     * already contributes the WORKSHEET label. */
    ctx = g_option_context_new (NULL);
    g_option_context_set_summary (ctx,
        "Run a saved pipnode worksheet without the GUI.  The file "
        "is loaded read-only — the runner has no save path wired "
        "up, so a scheduled execution cannot rewrite it.  Debug "
        "node output reaches stdout and status messages reach "
        "stderr.");
    g_option_context_add_main_entries (ctx, entries, NULL);

    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("pipnode-run: %s\n", error->message);
        g_error_free (error);
        g_option_context_free (ctx);
        g_strfreev (opt_remaining);
        return 1;
    }
    g_option_context_free (ctx);

    if (opt_version)
    {
#ifdef GIT_VERSION
        g_print ("pipnode-run %s (%s)\n", PACKAGE_VERSION, GIT_VERSION);
#else
        g_print ("pipnode-run %s\n", PACKAGE_VERSION);
#endif
        g_strfreev (opt_remaining);
        return 0;
    }

    if (opt_remaining == NULL || opt_remaining[0] == NULL)
    {
        g_printerr ("pipnode-run: a WORKSHEET path is required "
                    "(see --help)\n");
        g_strfreev (opt_remaining);
        return 2;
    }

    if (opt_remaining[1] != NULL)
    {
        g_printerr ("pipnode-run: only one WORKSHEET path is "
                    "accepted; got \"%s\" and \"%s\"\n",
                    opt_remaining[0], opt_remaining[1]);
        g_strfreev (opt_remaining);
        return 2;
    }

    if (opt_timeout < 0)
    {
        g_printerr ("pipnode-run: --timeout must be non-negative "
                    "(got %d)\n", opt_timeout);
        g_strfreev (opt_remaining);
        return 2;
    }

    path = opt_remaining[0];

    /* Auto-discover plugins before constructing the flow so the
     * load path can resolve plugin-shipped node types from the
     * worksheet JSON.  Same search order as the GUI binary —
     * PIPNODE_PLUGIN_PATH, ~/.local/share/pipnode/plugins, and the
     * baked-in pkglibdir. */
    pn_node_factory_load_plugins_default (pn_node_factory_get_default ());

    flow = pn_flow_new ();

    /* Connect the message forwarders before the load so that any
     * status text emitted while wiring up the flow (a hypothetical
     * future "loaded N nodes" line, for example) is not lost. */
    g_signal_connect (flow, "status-message",
                      G_CALLBACK (on_status_message), NULL);
    g_signal_connect (flow, "debug-message",
                      G_CALLBACK (on_debug_message),  NULL);

    if (!pn_flow_load_from_file (flow, path, &error))
    {
        g_printerr ("pipnode-run: failed to load %s: %s\n",
                    path,
                    error != NULL ? error->message : "unknown error");
        g_clear_error (&error);
        g_object_unref (flow);
        g_strfreev (opt_remaining);
        return 1;
    }

    loop = g_main_loop_new (NULL, FALSE);

    /* SIGINT / SIGTERM cause a graceful unwind: the loop drops back
     * to main(), the flow is finalised, file descriptors are
     * closed, and the process exits 0.  Without this Ctrl-C would
     * still terminate the process but skip the pipnode-side
     * cleanup, which matters once a node holds e.g. an open SQLite
     * handle. */
    g_unix_signal_add (SIGINT,  on_quit_signal, loop);
    g_unix_signal_add (SIGTERM, on_quit_signal, loop);

    if (opt_timeout > 0)
        g_timeout_add_seconds (opt_timeout, on_timeout, loop);

    g_main_loop_run (loop);

    g_main_loop_unref (loop);
    g_object_unref     (flow);
    g_strfreev         (opt_remaining);

    return status;
}
