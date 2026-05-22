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

/* Unit tests for the shared ${...} substitution engine.  Drives
 * pn_subst_expand() directly with the built-in json / strv resolvers,
 * exercising both output modes, all three miss policies, resolver
 * chaining, and the JSON string-context escaping that PnRewrite relies
 * on.  Headless: no nodes, no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-subst.h"

#include <json-glib/json-glib.h>

/* Build a small root:
 *   { "topic": "sensors",
 *     "data": { "value": 42, "ratio": 1.5, "label": "cpu",
 *               "flag": true, "obj": { "a": 1 },
 *               "raw": "a\"b\\c\nd" } }                              */
static JsonObject *
make_root (void)
{
    JsonObject *root = json_object_new ();
    JsonObject *data = json_object_new ();
    JsonObject *obj  = json_object_new ();

    json_object_set_int_member    (data, "value", 42);
    json_object_set_double_member (data, "ratio", 1.5);
    json_object_set_string_member (data, "label", "cpu");
    json_object_set_boolean_member(data, "flag",  TRUE);

    json_object_set_int_member    (obj,  "a", 1);
    json_object_set_object_member (data, "obj", obj);

    json_object_set_string_member (data, "raw", "a\"b\\c\nd");
    json_object_set_null_member   (data, "none");

    json_object_set_object_member (root, "data", data);
    json_object_set_string_member (root, "topic", "sensors");

    return root;
}

/* Expand @tmpl with a one-element json resolver over @root. */
static gchar *
expand_json (const gchar *tmpl,
             JsonObject  *root,
             PnSubstMode  mode,
             PnSubstMiss  miss)
{
    PnSubstResolver  r;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;

    pn_subst_resolver_json (&r, root);
    chain[0] = &r;
    chain[1] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = mode;
    ctx.miss      = miss;

    return pn_subst_expand (tmpl, &ctx);
}

/* Expand @tmpl with a one-element strv resolver over @pairs. */
static gchar *
expand_strv (const gchar         *tmpl,
             const gchar * const *pairs,
             PnSubstMode          mode,
             PnSubstMiss          miss)
{
    PnSubstResolver  r;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;

    pn_subst_resolver_strv (&r, pairs);
    chain[0] = &r;
    chain[1] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = mode;
    ctx.miss      = miss;

    return pn_subst_expand (tmpl, &ctx);
}

static void
test_text_scalars (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    out = expand_json ("${data/value}|${data/ratio}|${data/label}|${data/flag}",
                       root, PN_SUBST_TEXT, PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "42|1.5|cpu|true");
    g_free (out);

    /* Envelope member, no surrounding JSON ceremony in text mode. */
    out = expand_json ("topic=${topic}", root, PN_SUBST_TEXT,
                       PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "topic=sensors");
    g_free (out);

    json_object_unref (root);
}

static void
test_text_object_compact_json (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    /* A non-leaf path falls back to compact JSON. */
    out = expand_json ("${data/obj}", root, PN_SUBST_TEXT,
                       PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "{\"a\":1}");
    g_free (out);

    json_object_unref (root);
}

static void
test_text_miss_empty (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    out = expand_json ("[${data/nope}]", root, PN_SUBST_TEXT,
                       PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "[]");
    g_free (out);

    json_object_unref (root);
}

static void
test_json_value_slot (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    /* In a value slot: number is a bare token, string emerges quoted. */
    out = expand_json ("{\"v\": ${data/value}, \"l\": ${data/label}}",
                       root, PN_SUBST_JSON, PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "{\"v\": 42, \"l\": \"cpu\"}");
    g_free (out);

    json_object_unref (root);
}

static void
test_json_string_slot (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    /* Inside a string literal: raw escaped contents, no extra quotes. */
    out = expand_json ("\"x/${topic}\"", root, PN_SUBST_JSON,
                       PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "\"x/sensors\"");
    g_free (out);

    json_object_unref (root);
}

static void
test_json_escaping (void)
{
    JsonObject  *root = make_root ();
    gchar       *out;
    JsonParser  *parser;
    JsonNode    *node;
    JsonObject  *obj;
    gboolean     ok;

    /* A value with ", \, and newline embedded inside a JSON string slot
     * must stay well-formed; prove it by re-parsing. */
    out = expand_json ("{\"s\": \"${data/raw}\"}", root, PN_SUBST_JSON,
                       PN_SUBST_MISS_NULL);

    parser = json_parser_new ();
    ok = json_parser_load_from_data (parser, out, -1, NULL);
    PN_CHECK (ok);

    node = json_parser_get_root (parser);
    PN_CHECK (node != NULL && JSON_NODE_HOLDS_OBJECT (node));
    obj = json_node_get_object (node);
    PN_CHECK_CMPSTR (json_object_get_string_member (obj, "s"),
                     ==, "a\"b\\c\nd");

    g_object_unref (parser);
    g_free (out);
    json_object_unref (root);
}

static void
test_json_miss_null (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    out = expand_json ("{\"v\": ${data/nope}}", root, PN_SUBST_JSON,
                       PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "{\"v\": null}");
    g_free (out);

    json_object_unref (root);
}

static void
test_json_miss_null_in_string (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    /* A missing key in a value slot becomes the JSON token `null`, but a
     * missing key INSIDE a string literal must not inject the word "null"
     * into the text — there it collapses to empty, leaving valid JSON. */
    out = expand_json ("{\"v\": ${data/nope}, \"s\": \"hello ${data/nope}\"}",
                       root, PN_SUBST_JSON, PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "{\"v\": null, \"s\": \"hello \"}");
    g_free (out);

    json_object_unref (root);
}

static void
test_json_null_value_in_string (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    /* A present-but-null value reads the same as a missing key: the JSON
     * token `null` in a value slot, empty inside a string literal. */
    out = expand_json ("{\"v\": ${data/none}, \"s\": \"x=${data/none}\"}",
                       root, PN_SUBST_JSON, PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "{\"v\": null, \"s\": \"x=\"}");
    g_free (out);

    json_object_unref (root);
}

static void
test_strv_exact_and_verbatim (void)
{
    const gchar * const pairs[] = {
        "nodeclass", "CPU",
        "nodename",  "cpu0",
        "hostname",  "host1",
        NULL
    };
    gchar *out;

    /* Known keys substitute; an unknown key is left verbatim. */
    out = expand_strv ("${nodeclass}/${nodename}@${hostname}",
                       pairs, PN_SUBST_TEXT, PN_SUBST_MISS_VERBATIM);
    PN_CHECK_CMPSTR (out, ==, "CPU/cpu0@host1");
    g_free (out);

    out = expand_strv ("echo ${nodename} ${HOME}",
                       pairs, PN_SUBST_TEXT, PN_SUBST_MISS_VERBATIM);
    PN_CHECK_CMPSTR (out, ==, "echo cpu0 ${HOME}");
    g_free (out);

    /* Whole-key match only: a near-miss key does not resolve. */
    out = expand_strv ("${nodeclassx}", pairs, PN_SUBST_TEXT,
                       PN_SUBST_MISS_VERBATIM);
    PN_CHECK_CMPSTR (out, ==, "${nodeclassx}");
    g_free (out);
}

static void
test_chain_precedence (void)
{
    JsonObject          *root = make_root ();
    const gchar * const  pairs[] = { "nodename", "cpu0", NULL };
    PnSubstResolver      vars, msg;
    PnSubstResolver     *chain[3];
    PnSubstContext       ctx;
    gchar               *out;

    pn_subst_resolver_strv (&vars, pairs);
    pn_subst_resolver_json (&msg, root);
    chain[0] = &vars;     /* node vars first */
    chain[1] = &msg;      /* then message fields */
    chain[2] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_TEXT;
    ctx.miss      = PN_SUBST_MISS_EMPTY;

    /* Bare key served by strv, path key falls through to json. */
    out = pn_subst_expand ("${nodename}:${data/value}", &ctx);
    PN_CHECK_CMPSTR (out, ==, "cpu0:42");
    g_free (out);

    json_object_unref (root);
}

static void
test_unterminated_brace (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    out = expand_json ("a${b", root, PN_SUBST_TEXT, PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "a${b");
    g_free (out);

    out = expand_json ("a${b", root, PN_SUBST_JSON, PN_SUBST_MISS_NULL);
    PN_CHECK_CMPSTR (out, ==, "a${b");
    g_free (out);

    json_object_unref (root);
}

static void
test_null_and_empty_template (void)
{
    JsonObject *root = make_root ();
    gchar      *out;

    out = expand_json (NULL, root, PN_SUBST_TEXT, PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "");
    g_free (out);

    out = expand_json ("", root, PN_SUBST_TEXT, PN_SUBST_MISS_EMPTY);
    PN_CHECK_CMPSTR (out, ==, "");
    g_free (out);

    json_object_unref (root);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-subst");
    pn_test_add ("text_scalars",         test_text_scalars);
    pn_test_add ("text_object_json",     test_text_object_compact_json);
    pn_test_add ("text_miss_empty",      test_text_miss_empty);
    pn_test_add ("json_value_slot",      test_json_value_slot);
    pn_test_add ("json_string_slot",     test_json_string_slot);
    pn_test_add ("json_escaping",        test_json_escaping);
    pn_test_add ("json_miss_null",       test_json_miss_null);
    pn_test_add ("json_miss_null_str",   test_json_miss_null_in_string);
    pn_test_add ("json_null_value_str",  test_json_null_value_in_string);
    pn_test_add ("strv_exact_verbatim",  test_strv_exact_and_verbatim);
    pn_test_add ("chain_precedence",     test_chain_precedence);
    pn_test_add ("unterminated_brace",   test_unterminated_brace);
    pn_test_add ("null_empty_template",  test_null_and_empty_template);
    return pn_test_run ();
}
