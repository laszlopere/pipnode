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

/* Unit tests for PnShellCommand.  The node's trigger spawns /bin/sh -c
 * <command> (or routes it through ssh) on a worker thread, so the tests
 * NEVER call the trigger — they exercise only the headless-safe contract:
 *
 *   - the host-routing helper pn_shell_wrap_argv() builds the exact argv
 *     the trigger would hand g_spawn_sync (local pass-through vs. the
 *     BatchMode ssh wrap), and pn_shell_host_is_local() classifies hosts;
 *   - the shell-command / host properties default correctly and
 *     round-trip;
 *   - the node carries has-error while unconfigured and clears it once a
 *     command is set (the "configuration required" gate);
 *   - pn_node_expand_vars() interpolates the ${nodeclass}/${hostname}
 *     built-ins the trigger applies to the command while leaving an
 *     unrelated ${HOME} verbatim for the login shell.
 *
 * No shell is ever spawned and no host is contacted. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"
#include "pn-shell-command.h"
#include "pn-shell-host.h"

/* ---- host classification (pure helper) --------------------------- */

static void
test_host_is_local (void)
{
    /* Empty / NULL / the loopback spellings are "this machine"; anything
     * else routes through ssh. */
    PN_CHECK (pn_shell_host_is_local (NULL));
    PN_CHECK (pn_shell_host_is_local (""));
    PN_CHECK (pn_shell_host_is_local ("localhost"));
    PN_CHECK (pn_shell_host_is_local ("127.0.0.1"));
    PN_CHECK (pn_shell_host_is_local ("::1"));

    PN_CHECK_FALSE (pn_shell_host_is_local ("box"));
    PN_CHECK_FALSE (pn_shell_host_is_local ("user@box"));
    PN_CHECK_FALSE (pn_shell_host_is_local ("192.168.0.9"));
}

/* ---- argv construction ------------------------------------------- */

static void
test_wrap_argv_local (void)
{
    /* The exact base argv the trigger builds for a local run; the local
     * branch is a verbatim copy (g_spawn_sync goes through no shell). */
    const gchar *base[] = { "/bin/sh", "-c", "echo hi", NULL };
    gchar **out = pn_shell_wrap_argv ("", (const gchar *const *) base);

    PN_CHECK (out != NULL);
    PN_CHECK_CMPINT (g_strv_length (out), ==, 3);
    PN_CHECK_CMPSTR (out[0], ==, "/bin/sh");
    PN_CHECK_CMPSTR (out[1], ==, "-c");
    PN_CHECK_CMPSTR (out[2], ==, "echo hi");
    PN_CHECK (out[3] == NULL);

    g_strfreev (out);

    /* "localhost" is local too — same verbatim copy. */
    out = pn_shell_wrap_argv ("localhost", (const gchar *const *) base);
    PN_CHECK_CMPINT (g_strv_length (out), ==, 3);
    PN_CHECK_CMPSTR (out[0], ==, "/bin/sh");
    g_strfreev (out);
}

static void
test_wrap_argv_remote (void)
{
    /* A remote host prepends the BatchMode ssh preamble (the load-bearing
     * flags that forbid every interactive prompt) followed by the host
     * and then the original argv, untouched. */
    const gchar *base[] = { "echo hi", NULL };
    gchar **out = pn_shell_wrap_argv ("user@box",
                                      (const gchar *const *) base);

    PN_CHECK (out != NULL);
    PN_CHECK_CMPINT (g_strv_length (out), ==, 7);
    PN_CHECK_CMPSTR (out[0], ==, "ssh");
    PN_CHECK_CMPSTR (out[1], ==, "-o");
    PN_CHECK_CMPSTR (out[2], ==, "BatchMode=yes");
    PN_CHECK_CMPSTR (out[3], ==, "-o");
    PN_CHECK_CMPSTR (out[4], ==, "ConnectTimeout=5");
    PN_CHECK_CMPSTR (out[5], ==, "user@box");
    PN_CHECK_CMPSTR (out[6], ==, "echo hi");
    PN_CHECK (out[7] == NULL);

    g_strfreev (out);
}

/* ---- properties -------------------------------------------------- */

static void
test_host_default_round_trip (void)
{
    PnShellCommand *cmd  = pn_shell_command_new ();
    gchar          *host = NULL;

    /* Default host is the empty string — "run on the local machine",
     * never coerced to "localhost". */
    g_object_get (cmd, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    g_object_set (cmd, "host", "user@box", NULL);
    g_object_get (cmd, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "user@box");
    g_free (host);

    /* Clearing the field stays cleared (empty != localhost). */
    g_object_set (cmd, "host", "", NULL);
    g_object_get (cmd, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    g_object_unref (cmd);
}

static void
test_command_round_trip (void)
{
    PnShellCommand *cmd     = pn_shell_command_new ();
    gchar          *command = NULL;

    /* A fresh node has no command configured. */
    g_object_get (cmd, "shell-command", &command, NULL);
    PN_CHECK (command == NULL);
    g_free (command);

    g_object_set (cmd, "shell-command", "uptime | head -1", NULL);
    g_object_get (cmd, "shell-command", &command, NULL);
    PN_CHECK_CMPSTR (command, ==, "uptime | head -1");
    g_free (command);

    g_object_unref (cmd);
}

static void
test_has_error_gate (void)
{
    PnShellCommand *cmd = pn_shell_command_new ();

    /* Unconfigured (empty command) → the node flags itself for the
     * worksheet's central red/❗ overlay. */
    PN_CHECK (pn_node_get_has_error (PN_NODE (cmd)));

    g_object_set (cmd, "shell-command", "echo hi", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (cmd)));

    /* Clearing it back to empty re-arms the error. */
    g_object_set (cmd, "shell-command", "", NULL);
    PN_CHECK (pn_node_get_has_error (PN_NODE (cmd)));

    g_object_unref (cmd);
}

static void
test_is_a_source (void)
{
    /* Contract metadata: an output-only source that takes no input. */
    PnShellCommand *cmd = pn_shell_command_new ();

    PN_CHECK (pn_node_get_has_output (PN_NODE (cmd)));
    PN_CHECK_FALSE (pn_node_get_has_input (PN_NODE (cmd)));

    g_object_unref (cmd);
}

static void
test_expand_vars (void)
{
    /* The trigger runs the command through pn_node_expand_vars before
     * spawning it: the ${nodeclass}/${hostname} built-ins resolve while
     * an unrelated ${HOME} is left for the login shell to expand. */
    PnShellCommand *cmd = pn_shell_command_new ();
    gchar          *out;

    out = pn_node_expand_vars (PN_NODE (cmd), "echo ${nodeclass}");
    PN_CHECK_CMPSTR (out, ==, "echo Shell Command");
    g_free (out);

    out = pn_node_expand_vars (PN_NODE (cmd), "ls ${HOME}");
    PN_CHECK_CMPSTR (out, ==, "ls ${HOME}");
    g_free (out);

    out = pn_node_expand_vars (PN_NODE (cmd), "ping ${hostname}");
    {
        gchar *expect = g_strconcat ("ping ", g_get_host_name (), NULL);
        PN_CHECK_CMPSTR (out, ==, expect);
        g_free (expect);
    }
    g_free (out);

    g_object_unref (cmd);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-shell-command");
    pn_test_add ("host_is_local",       test_host_is_local);
    pn_test_add ("wrap_argv_local",     test_wrap_argv_local);
    pn_test_add ("wrap_argv_remote",    test_wrap_argv_remote);
    pn_test_add ("host_round_trip",     test_host_default_round_trip);
    pn_test_add ("command_round_trip",  test_command_round_trip);
    pn_test_add ("has_error_gate",      test_has_error_gate);
    pn_test_add ("is_a_source",         test_is_a_source);
    pn_test_add ("expand_vars",         test_expand_vars);
    return pn_test_run ();
}
