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

/* Unit tests for PnShellScript, the multi-line sibling of PnShellCommand.
 * Like that test, these NEVER call the trigger (it would spawn /bin/sh on
 * a worker thread); they exercise only the headless-safe contract:
 *
 *   - the shell-script / host properties default correctly and
 *     round-trip, including a genuinely multi-line body kept verbatim;
 *   - the node carries has-error while unconfigured and clears it once a
 *     script is set (the "configuration required" gate);
 *   - the node is an output-only source;
 *   - pn_node_expand_vars() interpolates the ${nodeclass}/${hostname}
 *     built-ins the trigger applies to the script while leaving an
 *     unrelated ${HOME} verbatim for the login shell;
 *   - the declarative settings schema gives the body a full-width
 *     PN_EDITOR_CODE editor highlighted as `sh`.
 *
 * No shell is ever spawned and no host is contacted. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-node.h"
#include "pn-settings-schema.h"
#include "pn-shell-output.h"
#include "pn-shell-script.h"

/* ---- properties -------------------------------------------------- */

static void
test_host_default_round_trip (void)
{
    PnShellScript *s    = pn_shell_script_new ();
    gchar         *host = NULL;

    /* Default host is the empty string — "run on the local machine",
     * never coerced to "localhost". */
    g_object_get (s, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    g_object_set (s, "host", "user@box", NULL);
    g_object_get (s, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "user@box");
    g_free (host);

    /* Clearing the field stays cleared (empty != localhost). */
    g_object_set (s, "host", "", NULL);
    g_object_get (s, "host", &host, NULL);
    PN_CHECK_CMPSTR (host, ==, "");
    g_free (host);

    g_object_unref (s);
}

static void
test_script_round_trip (void)
{
    PnShellScript *s      = pn_shell_script_new ();
    const gchar   *body   =
        "#!/bin/sh\n"
        "for f in *.log; do\n"
        "  echo \"$f\"\n"
        "done\n";
    gchar         *script = NULL;

    /* A fresh node has no script configured. */
    g_object_get (s, "shell-script", &script, NULL);
    PN_CHECK (script == NULL);
    g_free (script);

    /* A multi-line body round-trips byte-for-byte (newlines kept). */
    g_object_set (s, "shell-script", body, NULL);
    g_object_get (s, "shell-script", &script, NULL);
    PN_CHECK_CMPSTR (script, ==, body);
    g_free (script);

    g_object_unref (s);
}

static void
test_has_error_gate (void)
{
    PnShellScript *s = pn_shell_script_new ();

    /* Unconfigured (empty script) → the node flags itself for the
     * worksheet's central red/❗ overlay. */
    PN_CHECK (pn_node_get_has_error (PN_NODE (s)));

    g_object_set (s, "shell-script", "echo hi\n", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (s)));

    /* Clearing it back to empty re-arms the error. */
    g_object_set (s, "shell-script", "", NULL);
    PN_CHECK (pn_node_get_has_error (PN_NODE (s)));

    g_object_unref (s);
}

static void
test_is_a_source (void)
{
    /* Contract metadata: an output-only source that takes no input. */
    PnShellScript *s = pn_shell_script_new ();

    PN_CHECK (pn_node_get_has_output (PN_NODE (s)));
    PN_CHECK_FALSE (pn_node_get_has_input (PN_NODE (s)));

    g_object_unref (s);
}

static void
test_expand_vars (void)
{
    /* The trigger runs the script through pn_node_expand_vars before
     * spawning it: the ${nodeclass}/${hostname} built-ins resolve while
     * an unrelated ${HOME} is left for the login shell to expand. */
    PnShellScript *s = pn_shell_script_new ();
    gchar         *out;

    out = pn_node_expand_vars (PN_NODE (s), "echo ${nodeclass}");
    PN_CHECK_CMPSTR (out, ==, "echo Shell Script");
    g_free (out);

    out = pn_node_expand_vars (PN_NODE (s), "ls ${HOME}");
    PN_CHECK_CMPSTR (out, ==, "ls ${HOME}");
    g_free (out);

    out = pn_node_expand_vars (PN_NODE (s), "ping ${hostname}");
    {
        gchar *expect = g_strconcat ("ping ", g_get_host_name (), NULL);
        PN_CHECK_CMPSTR (out, ==, expect);
        g_free (expect);
    }
    g_free (out);

    g_object_unref (s);
}

static void
test_output_format_round_trip (void)
{
    PnShellScript      *s   = pn_shell_script_new ();
    PnShellOutputFormat fmt;

    /* Defaults to Text (the historical behaviour). */
    g_object_get (s, "output-format", &fmt, NULL);
    PN_CHECK_CMPINT (fmt, ==, PN_SHELL_OUTPUT_TEXT);

    g_object_set (s, "output-format", PN_SHELL_OUTPUT_JSON, NULL);
    g_object_get (s, "output-format", &fmt, NULL);
    PN_CHECK_CMPINT (fmt, ==, PN_SHELL_OUTPUT_JSON);

    g_object_unref (s);
}

/* ---- settings schema (GTK-free description) ---------------------- */

static void
test_schema_code_editor (void)
{
    /* The class declares two tabs: a "Settings" tab (output mode +
     * host + SSH login) first, then a "Script" tab whose sole row edits
     * the body as a full-width `sh`-highlighted code editor — its own
     * last page.  The schema is GTK-free data, so this needs no GUI. */
    PnShellScript    *s      = pn_shell_script_new ();
    PnNodeClass      *klass  = PN_NODE_GET_CLASS (s);
    PnSettingsSchema *schema = pn_node_class_get_settings_schema (klass);

    PN_CHECK (schema != NULL);
    PN_CHECK (pn_settings_schema_has_tabs (schema));
    PN_CHECK_CMPINT (pn_settings_schema_get_n_tabs (schema), ==, 2);

    /* Tab 0 — "Settings": output mode + where/how to connect. */
    PN_CHECK_CMPSTR (pn_settings_schema_get_tab_title (schema, 0),
                     ==, "Settings");
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (schema, 0, 0),
                     ==, "output-format");
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (schema, 0, 1),
                     ==, "host");
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (schema, 0, 2),
                     ==, "auth-profile");

    /* Tab 1 — "Script": the code editor alone, full width, `sh`-lit. */
    PN_CHECK_CMPSTR (pn_settings_schema_get_tab_title (schema, 1),
                     ==, "Script");
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (schema, 1, 0),
                     ==, "shell-script");
    PN_CHECK_CMPINT (pn_settings_schema_row_kind (schema, 1, 0),
                     ==, PN_EDITOR_CODE);
    PN_CHECK_CMPSTR (pn_settings_schema_row_code_language (schema, 1, 0),
                     ==, "sh");
    PN_CHECK ((pn_settings_schema_row_get_flags (schema, 1, 0)
               & PN_ROW_FLAG_FULL_WIDTH) != 0);

    g_object_unref (s);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-shell-script");
    pn_test_add ("host_round_trip",   test_host_default_round_trip);
    pn_test_add ("script_round_trip", test_script_round_trip);
    pn_test_add ("has_error_gate",    test_has_error_gate);
    pn_test_add ("is_a_source",       test_is_a_source);
    pn_test_add ("expand_vars",       test_expand_vars);
    pn_test_add ("output_format_round_trip", test_output_format_round_trip);
    pn_test_add ("schema_code_editor", test_schema_code_editor);
    return pn_test_run ();
}
