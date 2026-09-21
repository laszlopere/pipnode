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

/* ------------------------------------------------------------------ */
/*  The statement splitter                                             */
/* ------------------------------------------------------------------ */

/* Scan and split @program in one go, so a splitter case reads as the
 * program it is about.  The caller owns both halves and frees them
 * with split_free(), since a statement borrows its line. */
typedef struct
{
    GPtrArray *lines;
    GPtrArray *statements;
    GPtrArray *errors;
} Split;

static Split
split (const gchar *program)
{
    Split result;

    result.errors     = pn_figure_errors_new ();
    result.lines      = pn_figure_scan (program, result.errors);
    result.statements = pn_figure_split (result.lines, result.errors);
    return result;
}

static void
split_free (Split *self)
{
    g_ptr_array_unref (self->statements);
    g_ptr_array_unref (self->lines);
    g_ptr_array_unref (self->errors);
}

static PnFigureStatement *
statement (Split *self, guint n)
{
    return n < self->statements->len
           ? g_ptr_array_index (self->statements, n)
           : NULL;
}

static PnFigureArg *
arg (PnFigureStatement *self, guint n)
{
    return self != NULL && n < self->args->len
           ? g_ptr_array_index (self->args, n)
           : NULL;
}

/* Kind, text and offset of one argument in one check each. */
static void
check_arg (PnFigureStatement *self,
           guint              n,
           PnFigureArgKind    kind,
           const gchar       *text,
           gsize              offset)
{
    PnFigureArg *a = arg (self, n);

    PN_CHECK (a != NULL);
    if (a == NULL)
        return;

    PN_CHECK_CMPINT (a->kind, ==, kind);
    PN_CHECK_CMPSTR (a->text, ==, text);
    PN_CHECK_CMPINT (a->offset, ==, offset);
}

static void
test_verb_and_arguments (void)
{
    Split              s = split ("line -dx, -dy, dx, dy");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPINT (st->kind, ==, PN_FIGURE_STATEMENT_VERB);
    PN_CHECK_CMPSTR (st->name, ==, "line");
    PN_CHECK_CMPINT (st->args->len, ==, 4);

    /* Unquoted is an expression, kept as text for the parser, and the
     * offset points at where it really starts. */
    check_arg (st, 0, PN_FIGURE_ARG_EXPRESSION, "-dx", 5);
    check_arg (st, 1, PN_FIGURE_ARG_EXPRESSION, "-dy", 10);
    check_arg (st, 2, PN_FIGURE_ARG_EXPRESSION, "dx",  15);
    check_arg (st, 3, PN_FIGURE_ARG_EXPRESSION, "dy",  19);

    split_free (&s);
}

static void
test_verb_without_arguments (void)
{
    Split s = split ("nofill\nNOFILL\n  Fill \"#808080\"");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 3);

    /* Nothing after the verb is no arguments, not one empty one. */
    PN_CHECK_CMPINT (statement (&s, 0)->args->len, ==, 0);

    /* Verbs are case-insensitive (80.2 rule 2). */
    PN_CHECK_CMPSTR (statement (&s, 1)->name, ==, "nofill");
    PN_CHECK_CMPSTR (statement (&s, 2)->name, ==, "fill");
    check_arg (statement (&s, 2), 0, PN_FIGURE_ARG_STRING, "#808080", 5);

    split_free (&s);
}

static void
test_assignment (void)
{
    Split              s = split ("dx = 50 * cos(a)\nR2=1");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 2);

    PN_CHECK_CMPINT (st->kind, ==, PN_FIGURE_STATEMENT_ASSIGNMENT);
    /* The target keeps its case, and the WHOLE line is what goes to
     * pn_expr_parser_parse(). */
    PN_CHECK_CMPSTR (st->name, ==, "dx");
    PN_CHECK_CMPINT (st->args->len, ==, 0);
    PN_CHECK_CMPSTR (st->source->text, ==, "dx = 50 * cos(a)");

    PN_CHECK_CMPINT (statement (&s, 1)->kind, ==,
                     PN_FIGURE_STATEMENT_ASSIGNMENT);
    PN_CHECK_CMPSTR (statement (&s, 1)->name, ==, "R2");

    split_free (&s);
}

static void
test_equality_is_not_an_assignment (void)
{
    /* One lookahead, and "==" is not it: the line stays a verb, which
     * the verb table will then reject. */
    Split              s = split ("a == 5");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (st->kind, ==, PN_FIGURE_STATEMENT_VERB);
    PN_CHECK_CMPSTR (st->name, ==, "a");
    check_arg (st, 0, PN_FIGURE_ARG_EXPRESSION, "== 5", 2);

    split_free (&s);
}

static void
test_commas_inside_parens (void)
{
    /* Depth 0 only — the day atan2/min/max lands this must already
     * work (80.2 rule 3). */
    Split              s = split ("circle max(1, (2)), 5, 5");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (st->args->len, ==, 3);
    check_arg (st, 0, PN_FIGURE_ARG_EXPRESSION, "max(1, (2))", 7);
    check_arg (st, 1, PN_FIGURE_ARG_EXPRESSION, "5", 20);

    split_free (&s);
}

static void
test_commas_inside_strings (void)
{
    Split              s = split ("text 0, 0, \"a, b\", 1");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (st->args->len, ==, 4);
    check_arg (st, 2, PN_FIGURE_ARG_STRING, "a, b", 11);
    check_arg (st, 3, PN_FIGURE_ARG_EXPRESSION, "1", 19);

    split_free (&s);
}

static void
test_string_escapes (void)
{
    Split              s = split ("text 0,0,\"a\\\"b\\\\c\\nd\"");
    PnFigureStatement *st = statement (&s, 0);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    /* \" and \\ are the grammar's; \n is 80.7(d)'s line split. */
    check_arg (st, 2, PN_FIGURE_ARG_STRING, "a\"b\\c\nd", 9);

    split_free (&s);
}

static void
test_unknown_escape (void)
{
    Split          s = split ("width 2\ntext 0,0,\"a\\qb\"");
    PnFigureError *error;

    /* The bad line is dropped; the good one still made it. */
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPSTR (statement (&s, 0)->name, ==, "width");

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 2);
    PN_CHECK_CMPINT (error->column, ==, 12);
    PN_CHECK_CMPSTR (error->message, ==, "unknown escape \"\\q\"");

    split_free (&s);
}

static void
test_text_after_a_string (void)
{
    /* Quoted means literal, so a string is the whole argument or it is
     * a mistake (80.2 rule 7). */
    Split          s = split ("text 0, 0, \"A\" + 1");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 1);
    /* At the offending text, not at the space before it. */
    PN_CHECK_CMPINT (error->column, ==, 16);
    PN_CHECK_CMPSTR (error->message, ==, "unexpected text after a string");

    split_free (&s);
}

static void
test_empty_argument (void)
{
    Split          s = split ("line 1,,2");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 1);
    PN_CHECK_CMPINT (error->column, ==, 8);
    PN_CHECK_CMPSTR (error->message, ==, "empty argument");

    split_free (&s);
}

static void
test_dangling_comma_is_reported_here (void)
{
    /* 80.22.1 leaves the comma a blank line orphaned in the text; this
     * is where it finally gets complained about. */
    Split          s = split ("poly 0,-2,\n\ncircle 5, 5, 2");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.lines->len, ==, 2);
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPSTR (statement (&s, 0)->name, ==, "circle");

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 1);
    PN_CHECK_CMPINT (error->column, ==, 11);
    PN_CHECK_CMPSTR (error->message, ==, "empty argument");

    split_free (&s);
}

static void
test_not_a_statement (void)
{
    Split          s = split ("5 + 3\n\"A\"\nwidth 2");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPSTR (statement (&s, 0)->name, ==, "width");

    /* Every error, not just the first (80.2 rule 9). */
    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 1);
    PN_CHECK_CMPINT (error->column, ==, 1);
    PN_CHECK_CMPSTR (error->message, ==, "expected a verb or an assignment");
    error = g_ptr_array_index (s.errors, 1);
    PN_CHECK_CMPINT (error->line, ==, 2);

    split_free (&s);
}

static void
test_arguments_across_a_continuation (void)
{
    Split              s = split ("poly 0,-2,\n  -8,-14, 8,-14");
    PnFigureStatement *st = statement (&s, 0);
    gint               line = 0, column = 0;

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (st->args->len, ==, 6);
    check_arg (st, 2, PN_FIGURE_ARG_EXPRESSION, "-8", 10);

    /* An argument's offset is a position in the JOINED text, which
     * only the line it came from can turn back into a place a person
     * can look at. */
    pn_figure_line_locate (st->source, arg (st, 2)->offset, &line, &column);
    PN_CHECK_CMPINT (line, ==, 2);
    PN_CHECK_CMPINT (column, ==, 3);

    pn_figure_line_locate (st->source, arg (st, 1)->offset, &line, &column);
    PN_CHECK_CMPINT (line, ==, 1);
    PN_CHECK_CMPINT (column, ==, 8);

    split_free (&s);
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
    pn_test_add ("split_verb_args",     test_verb_and_arguments);
    pn_test_add ("split_no_args",       test_verb_without_arguments);
    pn_test_add ("split_assignment",    test_assignment);
    pn_test_add ("split_equality",      test_equality_is_not_an_assignment);
    pn_test_add ("split_parens",        test_commas_inside_parens);
    pn_test_add ("split_string_commas", test_commas_inside_strings);
    pn_test_add ("split_escapes",       test_string_escapes);
    pn_test_add ("split_bad_escape",    test_unknown_escape);
    pn_test_add ("split_string_junk",   test_text_after_a_string);
    pn_test_add ("split_empty_arg",     test_empty_argument);
    pn_test_add ("split_dangling_comma", test_dangling_comma_is_reported_here);
    pn_test_add ("split_not_a_statement", test_not_a_statement);
    pn_test_add ("split_continuation",  test_arguments_across_a_continuation);
    return pn_test_run ();
}
