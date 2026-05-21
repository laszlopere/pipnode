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

/* Unit tests for the self-contained JMESPath engine (pn_jmespath_search).
 * Each case evaluates an expression against canned JSON input and
 * compares a canonical serialisation of the result.  A successful
 * "no match" yields a JSON-null node; only genuine parse/eval errors
 * return NULL with a GError set.  Pure: no filesystem/network/GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-jmespath.h"

#include <json-glib/json-glib.h>

/* Canonical, compact JSON for a result node.  Scalars are formatted by
 * hand so the helper works regardless of whether the json-glib in use
 * lets the generator serialise a non-container root; arrays and objects
 * go through the generator (compact, since pretty defaults off). */
static gchar *
node_to_json (JsonNode *n)
{
    if (n == NULL || JSON_NODE_HOLDS_NULL (n))
        return g_strdup ("null");

    if (JSON_NODE_HOLDS_VALUE (n))
    {
        GType vt = json_node_get_value_type (n);
        if (vt == G_TYPE_STRING)
            return g_strdup_printf ("\"%s\"", json_node_get_string (n));
        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (n));
        if (vt == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (n));
        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (n) ? "true" : "false");
    }

    {
        JsonGenerator *g = json_generator_new ();
        gchar         *s;
        json_generator_set_root (g, n);
        s = json_generator_to_data (g, NULL);
        g_object_unref (g);
        return s != NULL ? s : g_strdup ("");
    }
}

static JsonNode *
parse_input (const gchar *input_json)
{
    JsonParser *p;
    JsonNode   *copy;

    if (input_json == NULL)
        return NULL;

    p = json_parser_new ();
    g_assert (json_parser_load_from_data (p, input_json, -1, NULL));
    copy = json_node_copy (json_parser_get_root (p));
    g_object_unref (p);
    return copy;
}

/* Run @expr against @input_json and return the canonical result string.
 * pn_jmespath_search does not take ownership of the input. */
static gchar *
search_json (const gchar *expr, const gchar *input_json)
{
    JsonNode *input = parse_input (input_json);
    GError   *err   = NULL;
    JsonNode *res   = pn_jmespath_search (expr, input, &err);
    gchar    *out   = (res != NULL) ? node_to_json (res) : g_strdup ("<error>");

    if (res != NULL)
        json_node_unref (res);
    g_clear_error (&err);
    if (input != NULL)
        json_node_unref (input);
    return out;
}

/* Assert @expr over @input evaluates to the canonical string @want. */
static void
check_eq (const gchar *expr, const gchar *input_json, const gchar *want)
{
    gchar *got = search_json (expr, input_json);
    PN_CHECK_CMPSTR (got, ==, want);
    g_free (got);
}

/* Assert @expr is a hard error: NULL result with the GError set. */
static void
check_error (const gchar *expr, const gchar *input_json)
{
    JsonNode *input = parse_input (input_json);
    GError   *err   = NULL;
    JsonNode *res   = pn_jmespath_search (expr, input, &err);

    PN_CHECK (res == NULL);
    PN_CHECK (err != NULL);

    if (res != NULL)
        json_node_unref (res);
    g_clear_error (&err);
    if (input != NULL)
        json_node_unref (input);
}

static void
test_paths (void)
{
    check_eq ("a",       "{\"a\":1}",                "1");
    check_eq ("a.b",     "{\"a\":{\"b\":\"hi\"}}",   "\"hi\"");
    check_eq ("a[1]",    "{\"a\":[10,20,30]}",       "20");
    check_eq ("a[-1]",   "{\"a\":[10,20,30]}",       "30");
    check_eq ("a[0:2]",  "{\"a\":[1,2,3,4]}",        "[1,2]");
}

static void
test_projections (void)
{
    check_eq ("a[*].b",  "{\"a\":[{\"b\":1},{\"b\":2}]}", "[1,2]");
    check_eq ("a[]",     "{\"a\":[[1,2],[3]]}",           "[1,2,3]");
    check_eq ("[a, b]",  "{\"a\":1,\"b\":2}",             "[1,2]");
    check_eq ("a[*].b | [0]",
              "{\"a\":[{\"b\":5},{\"b\":6}]}",            "5");
}

static void
test_filter_and_compare (void)
{
    check_eq ("people[?age >= `18`].name",
              "{\"people\":[{\"name\":\"A\",\"age\":20},"
              "{\"name\":\"B\",\"age\":10}]}",
              "[\"A\"]");
    check_eq ("a == `1`", "{\"a\":1}", "true");
    check_eq ("a != `1`", "{\"a\":2}", "true");
}

static void
test_functions (void)
{
    check_eq ("length(a)",    "{\"a\":[1,2,3]}",        "3");
    check_eq ("sort(a)",      "{\"a\":[3,1,2]}",        "[1,2,3]");
    check_eq ("join('-', a)", "{\"a\":[\"x\",\"y\",\"z\"]}", "\"x-y-z\"");
    check_eq ("to_number(a)", "{\"a\":\"42\"}",         "42");
    check_eq ("max_by(a, &age).name",
              "{\"a\":[{\"name\":\"A\",\"age\":5},"
              "{\"name\":\"B\",\"age\":9}]}",
              "\"B\"");
}

static void
test_no_match_is_null (void)
{
    /* No match is a JSON-null result, never an error. */
    check_eq ("z",        "{\"a\":1}",      "null");
    check_eq ("a.b.c",    "{\"a\":{}}",     "null");
    /* Empty / NULL expression yields null too. */
    check_eq ("",         "{\"a\":1}",      "null");
    check_eq (NULL,       "{\"a\":1}",      "null");
}

static void
test_errors (void)
{
    check_error ("a |",          "{\"a\":1}");   /* parse: dangling pipe   */
    check_error ("a b",          "{\"a\":1}");   /* parse: trailing tokens */
    check_error ("length(`5`)",  "{\"a\":1}");   /* type: number to length */
    check_error ("bogus(a)",     "{\"a\":1}");   /* unknown function       */
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-jmespath");
    pn_test_add ("paths",            test_paths);
    pn_test_add ("projections",      test_projections);
    pn_test_add ("filter_compare",   test_filter_and_compare);
    pn_test_add ("functions",        test_functions);
    pn_test_add ("no_match_is_null", test_no_match_is_null);
    pn_test_add ("errors",           test_errors);
    return pn_test_run ();
}
