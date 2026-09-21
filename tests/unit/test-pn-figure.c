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

/* Unit tests for PnFigure.  So far the line scanner: comment stripping
 * that knows about strings, comma continuation, blank lines and free
 * leading whitespace, plus the source position a joined line maps back
 * to. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-figure.h"

/* Borrowed text of logical line @n, or NULL past the end. */
static const gchar *
line_text (GPtrArray *lines, guint n)
{
    return n < lines->len
           ? ((PnFigureLine *) g_ptr_array_index (lines, n))->text
           : NULL;
}

static gint
line_number (GPtrArray *lines, guint n)
{
    return n < lines->len
           ? ((PnFigureLine *) g_ptr_array_index (lines, n))->line
           : -1;
}

static void
test_statements_and_blanks (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan ("view -60, -25, 60, 35\n"
                                        "\n"
                                        "# the beam\n"
                                        "    color \"#202020\"   \n"
                                        "width 2",
                                        errors);

    PN_CHECK_CMPINT (errors->len, ==, 0);
    PN_CHECK_CMPINT (lines->len, ==, 3);

    /* Blank and comment-only lines vanish; leading and trailing
     * whitespace is trimmed off what is left. */
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "view -60, -25, 60, 35");
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "color \"#202020\"");
    PN_CHECK_CMPSTR (line_text (lines, 2), ==, "width 2");

    /* The source line number survives the lines that vanished. */
    PN_CHECK_CMPINT (line_number (lines, 0), ==, 1);
    PN_CHECK_CMPINT (line_number (lines, 1), ==, 4);
    PN_CHECK_CMPINT (line_number (lines, 2), ==, 5);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_empty_program (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines;

    lines = pn_figure_scan ("", errors);
    PN_CHECK_CMPINT (lines->len, ==, 0);
    PN_CHECK_CMPINT (errors->len, ==, 0);
    g_ptr_array_unref (lines);

    lines = pn_figure_scan (NULL, errors);
    PN_CHECK_CMPINT (lines->len, ==, 0);
    g_ptr_array_unref (lines);

    /* Whitespace and comments only is still a blank figure. */
    lines = pn_figure_scan ("\n   \n# nothing here\n", errors);
    PN_CHECK_CMPINT (lines->len, ==, 0);
    PN_CHECK_CMPINT (errors->len, ==, 0);
    g_ptr_array_unref (lines);

    g_ptr_array_unref (errors);
}

static void
test_hash_inside_a_string (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan (
        "color \"#202020\"  # a grey, not a comment colour\n"
        "text 0, 0, \"a \\\" # b\"\n"
        "text 0, 0, \"back\\\\\" # trailing comment\n",
        errors);

    PN_CHECK_CMPINT (errors->len, ==, 0);
    PN_CHECK_CMPINT (lines->len, ==, 3);

    /* A "#" inside quotes is a character. */
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "color \"#202020\"");
    /* An escaped quote does not close the string, so the "#" after it
     * is still inside. */
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "text 0, 0, \"a \\\" # b\"");
    /* An escaped backslash does, so the comment after it is one. */
    PN_CHECK_CMPSTR (line_text (lines, 2), ==, "text 0, 0, \"back\\\\\"");

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_continuation (void)
{
    GPtrArray    *errors = pn_figure_errors_new ();
    GPtrArray    *lines  = pn_figure_scan ("poly 0,-2,\n"
                                           "    -8,-14, 8,-14\n"
                                           "nofill",
                                           errors);
    PnFigureLine *joined;
    gint          line = 0, column = 0;

    PN_CHECK_CMPINT (errors->len, ==, 0);
    PN_CHECK_CMPINT (lines->len, ==, 2);

    /* Pieces join straight after the comma, with no separator. */
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "poly 0,-2,-8,-14, 8,-14");
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "nofill");
    PN_CHECK_CMPINT (line_number (lines, 0), ==, 1);
    PN_CHECK_CMPINT (line_number (lines, 1), ==, 3);

    /* An offset in the head maps to the line it was typed on ... */
    joined = g_ptr_array_index (lines, 0);
    pn_figure_line_locate (joined, 5, &line, &column);
    PN_CHECK_CMPINT (line, ==, 1);
    PN_CHECK_CMPINT (column, ==, 6);

    /* ... and one in the tail to the NEXT line, at the column the
     * indented continuation really starts at. */
    pn_figure_line_locate (joined, 10, &line, &column);
    PN_CHECK_CMPINT (line, ==, 2);
    PN_CHECK_CMPINT (column, ==, 5);

    pn_figure_line_locate (joined, 12, &line, &column);
    PN_CHECK_CMPINT (line, ==, 2);
    PN_CHECK_CMPINT (column, ==, 7);

    /* Past the end clamps rather than walking off. */
    pn_figure_line_locate (joined, 9999, &line, &column);
    PN_CHECK_CMPINT (line, ==, 2);
    PN_CHECK_CMPINT (column, ==, 18);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_continuation_over_comments (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan ("poly 0,-2,   # the tip\n"
                                        "  # and the two feet\n"
                                        "  -8,-14, 8,-14\n",
                                        errors);

    PN_CHECK_CMPINT (errors->len, ==, 0);
    PN_CHECK_CMPINT (lines->len, ==, 1);
    /* The comment tail goes before the trailing comma is looked for,
     * and a comment-only line keeps the continuation alive. */
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "poly 0,-2,-8,-14, 8,-14");

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_blank_line_ends_a_continuation (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan ("poly 0,-2,\n"
                                        "\n"
                                        "circle 5, 5, 2\n",
                                        errors);

    /* The dangling comma stays in the text for the statement splitter
     * to complain about, instead of swallowing the next statement. */
    PN_CHECK_CMPINT (lines->len, ==, 2);
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "poly 0,-2,");
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "circle 5, 5, 2");
    PN_CHECK_CMPINT (errors->len, ==, 0);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_trailing_comma_at_end_of_program (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan ("circle 5, 5, 2\npoly 0,-2,", errors);

    PN_CHECK_CMPINT (lines->len, ==, 2);
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "poly 0,-2,");
    PN_CHECK_CMPINT (errors->len, ==, 0);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_unterminated_string (void)
{
    GPtrArray     *errors = pn_figure_errors_new ();
    GPtrArray     *lines  = pn_figure_scan ("width 2\n"
                                            "text 0, 0, \"A\n"
                                            "width 3",
                                            errors);
    PnFigureError *error;

    /* The broken line is dropped, so no later stage re-reports it. */
    PN_CHECK_CMPINT (lines->len, ==, 2);
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "width 2");
    PN_CHECK_CMPSTR (line_text (lines, 1), ==, "width 3");

    PN_CHECK_CMPINT (errors->len, ==, 1);
    error = g_ptr_array_index (errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 2);
    /* The column of the quote that was never closed. */
    PN_CHECK_CMPINT (error->column, ==, 12);
    PN_CHECK_CMPSTR (error->message, ==, "unterminated string");

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_unterminated_string_in_a_continuation (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    GPtrArray *lines  = pn_figure_scan ("text 0,\n"
                                        "     0, \"A\n"
                                        "width 3",
                                        errors);

    /* The head is already buffered when the tail breaks: the whole
     * logical line goes, not just the tail. */
    PN_CHECK_CMPINT (lines->len, ==, 1);
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "width 3");
    PN_CHECK_CMPINT (errors->len, ==, 1);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_columns_count_characters (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    /* "°" is two bytes, so a byte count would report column 20. */
    GPtrArray *lines  = pn_figure_scan ("text 0, 0, \"45\xc2\xb0\", \"x\n",
                                        errors);
    PnFigureError *error;

    PN_CHECK_CMPINT (lines->len, ==, 0);
    PN_CHECK_CMPINT (errors->len, ==, 1);
    error = g_ptr_array_index (errors, 0);
    PN_CHECK_CMPINT (error->column, ==, 19);

    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

static void
test_scan_without_a_collector (void)
{
    /* A NULL collector is allowed: the scan still runs, quietly. */
    GPtrArray *lines = pn_figure_scan ("text 0, 0, \"A\nwidth 3", NULL);

    PN_CHECK_CMPINT (lines->len, ==, 1);
    PN_CHECK_CMPSTR (line_text (lines, 0), ==, "width 3");

    g_ptr_array_unref (lines);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-figure");
    pn_test_add ("scan_statements",     test_statements_and_blanks);
    pn_test_add ("scan_empty",          test_empty_program);
    pn_test_add ("scan_hash_in_string", test_hash_inside_a_string);
    pn_test_add ("scan_continuation",   test_continuation);
    pn_test_add ("scan_cont_comments",  test_continuation_over_comments);
    pn_test_add ("scan_cont_blank_ends", test_blank_line_ends_a_continuation);
    pn_test_add ("scan_cont_at_eof",    test_trailing_comma_at_end_of_program);
    pn_test_add ("scan_unterminated",   test_unterminated_string);
    pn_test_add ("scan_unterminated_cont",
                 test_unterminated_string_in_a_continuation);
    pn_test_add ("scan_column_chars",   test_columns_count_characters);
    pn_test_add ("scan_no_collector",   test_scan_without_a_collector);
    return pn_test_run ();
}
