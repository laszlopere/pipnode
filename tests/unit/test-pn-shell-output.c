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

/* Unit tests for pn_shell_output_apply(), the shared output-shaping helper
 * used by the Shell Command / Shell Script trigger.  No shell is spawned;
 * the helper is fed captured stdout/stderr strings directly and the
 * resulting message data bag is inspected. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pntest.h"
#include "pn-message.h"
#include "pn-shell-command.h"
#include "pn-shell-output.h"

/* A throwaway source node so pn_message_new() has a valid source. */
static PnNode *
source_node (void)
{
    return PN_NODE (pn_shell_command_new ());
}

static gboolean
member_double (PnMessage *msg, const gchar *name, gdouble *out)
{
    JsonNode *node = pn_message_get_member (msg, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;
    *out = json_node_get_double (node);
    return TRUE;
}

/* PnMessage has no typed getters; read scalars off the JSON members. */
static const gchar *
member_string (PnMessage *msg, const gchar *name)
{
    JsonNode *node = pn_message_get_member (msg, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    return json_node_get_string (node);
}

static gboolean
member_boolean (PnMessage *msg, const gchar *name)
{
    JsonNode *node = pn_message_get_member (msg, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;
    return json_node_get_boolean (node);
}

/* ---- Text mode: the historical contract ------------------------- */

static void
test_text_mode (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);

    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_TEXT, TRUE,
                           "hello\n", "warn\n");

    /* output = stdout+stderr verbatim; success = exit status; no value. */
    PN_CHECK_CMPSTR (member_string (msg, "output"), ==,
                     "hello\nwarn\n");
    PN_CHECK (member_boolean (msg, "success"));
    PN_CHECK (pn_message_get_member (msg, "value") == NULL);

    g_object_unref (msg);
    g_object_unref (node);
}

/* ---- JSON mode: object merges into the data bag ----------------- */

static void
test_json_object_merges (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);
    gdouble    value;

    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_JSON, TRUE,
                           "{ \"value\": 1700000000, \"label\": \"hi\" }\n",
                           NULL);

    /* Every object member lands on the bag, value included. */
    PN_CHECK (member_double (msg, "value", &value));
    PN_CHECK_CMPINT ((gint64) value, ==, 1700000000);
    PN_CHECK_CMPSTR (member_string (msg, "label"), ==, "hi");
    /* Baseline success preserved; output baseline is the trimmed stdout. */
    PN_CHECK (member_boolean (msg, "success"));

    g_object_unref (msg);
    g_object_unref (node);
}

/* An object may override the baseline success/output members itself. */
static void
test_json_object_overrides (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);

    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_JSON, TRUE,
                           "{ \"success\": false, \"output\": \"nope\" }",
                           NULL);

    PN_CHECK_FALSE (member_boolean (msg, "success"));
    PN_CHECK_CMPSTR (member_string (msg, "output"), ==, "nope");

    g_object_unref (msg);
    g_object_unref (node);
}

/* ---- JSON mode: a bare scalar lands in data.value --------------- */

static void
test_json_bare_number (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);
    gdouble    value;

    /* `date +%s` style output: a bare integer with a trailing newline. */
    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_JSON, TRUE,
                           "1700000000\n", NULL);

    PN_CHECK (member_double (msg, "value", &value));
    PN_CHECK_CMPINT ((gint64) value, ==, 1700000000);
    PN_CHECK (member_boolean (msg, "success"));
    /* output baseline is the trimmed stdout (no trailing newline). */
    PN_CHECK_CMPSTR (member_string (msg, "output"), ==, "1700000000");

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_json_bare_boolean (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);
    gdouble    value;

    /* A bare boolean is coerced to the bag's 1.0/0.0 convention. */
    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_JSON, TRUE, "true", NULL);

    PN_CHECK (member_double (msg, "value", &value));
    PN_CHECK_NEAR (value, 1.0, 1e-9);

    g_object_unref (msg);
    g_object_unref (node);
}

/* ---- JSON mode: invalid input is reported as a failure ---------- */

static void
test_json_parse_failure (void)
{
    PnNode    *node = source_node ();
    PnMessage *msg  = pn_message_new (node, NULL);

    /* Not JSON: surface it as a failure carrying the raw text. */
    pn_shell_output_apply (msg, PN_SHELL_OUTPUT_JSON, TRUE,
                           "not json at all\n", NULL);

    PN_CHECK_FALSE (member_boolean (msg, "success"));
    PN_CHECK_CMPSTR (member_string (msg, "output"), ==,
                     "not json at all\n");
    PN_CHECK (pn_message_get_member (msg, "value") == NULL);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-shell-output");
    pn_test_add ("text_mode",            test_text_mode);
    pn_test_add ("json_object_merges",   test_json_object_merges);
    pn_test_add ("json_object_overrides", test_json_object_overrides);
    pn_test_add ("json_bare_number",     test_json_bare_number);
    pn_test_add ("json_bare_boolean",    test_json_bare_boolean);
    pn_test_add ("json_parse_failure",   test_json_parse_failure);
    return pn_test_run ();
}
