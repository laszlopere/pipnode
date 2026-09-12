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

/* Unit tests for pn_help_text_from_html(): the shared reduction of a
 * bundled help page to plain text.  Two modes — verbatim for the help
 * browser's no-WebKit fallback, and normalised for the palette's
 * full-text search. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-help-text.h"

/* Convenience: run the search-mode extraction and compare. */
static void
check_search (const gchar *html, const gchar *expected)
{
    gchar *got = pn_help_text_from_html (html, TRUE);

    PN_CHECK_CMPSTR (got, ==, expected);
    g_free (got);
}

static void
test_strips_tags (void)
{
    check_search ("<p>Rotary <em>knob</em> source.</p>",
                  "Rotary knob source.");

    /* Attributes, and a tag spanning what would otherwise be a word
     * boundary, must not leak into the text. */
    check_search ("<a href=\"help-index.html\">index</a>", "index");
    check_search ("<code>min</code>/<code>max</code>", "min/max");
}

static void
test_entities_become_spaces (void)
{
    /* The pages write non-breaking spaces and dashes as entities; a
     * needle typed as plain prose has to match across them. */
    check_search ("Wheel&nbsp;up turns the knob", "Wheel up turns the knob");
    check_search ("a&mdash;b", "a b");
    check_search ("&#160;x&#x2014;y", "x y");
}

static void
test_bare_ampersand_survives (void)
{
    /* Not an entity: no ';' before the bound, so the '&' is literal
     * and must not swallow the words after it. */
    check_search ("Sinks & Sources", "Sinks & Sources");
    check_search ("a &verylongnotanentity; b", "a &verylongnotanentity; b");
    check_search ("a &; b", "a &; b");
}

static void
test_whitespace_collapses (void)
{
    /* Source lines wrap mid-sentence; the search text must read as one
     * line so "spin the mouse" matches text broken across two lines. */
    check_search ("spin the\n   mouse\twheel", "spin the mouse wheel");

    /* Nothing leads or trails the result. */
    check_search ("\n  <p>\n  Hello\n  </p>\n  ", "Hello");
}

static void
test_degenerate_input (void)
{
    check_search ("", "");
    check_search ("<p>", "");

    /* An unterminated tag eats the rest of the document rather than
     * spilling markup into the text. */
    check_search ("visible <span class=\"x", "visible");
}

static void
test_verbatim_mode_keeps_layout (void)
{
    /* What the no-WebKit help viewer renders: tags gone, but the
     * paragraph breaks and the entities exactly as the page wrote
     * them. */
    gchar *got = pn_help_text_from_html ("<p>one</p>\n<p>two&nbsp;three</p>\n",
                                         FALSE);

    PN_CHECK_CMPSTR (got, ==, "one\ntwo&nbsp;three\n");
    g_free (got);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-help-text");
    pn_test_add ("strips_tags",        test_strips_tags);
    pn_test_add ("entities_to_spaces", test_entities_become_spaces);
    pn_test_add ("bare_ampersand",     test_bare_ampersand_survives);
    pn_test_add ("whitespace_folds",   test_whitespace_collapses);
    pn_test_add ("degenerate_input",   test_degenerate_input);
    pn_test_add ("verbatim_mode",      test_verbatim_mode_keeps_layout);
    return pn_test_run ();
}
