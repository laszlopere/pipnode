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

#include <math.h>
#include <locale.h>

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

/* ------------------------------------------------------------------ */
/*  The verb table                                                     */
/* ------------------------------------------------------------------ */

/* Scan, split and check, which is the whole front end so far. */
static Split
checked (const gchar *program)
{
    Split result = split (program);

    pn_figure_check_verbs (result.statements, result.errors);
    pn_figure_check_blocks (result.statements, result.errors);
    return result;
}

/* The line error @n was reported on, or -1. */
static gint
error_line (Split *self, guint n)
{
    return n < self->errors->len
           ? ((PnFigureError *) g_ptr_array_index (self->errors, n))->line
           : -1;
}

/* The message of error @n, or NULL. */
static const gchar *
error_text (Split *self, guint n)
{
    return n < self->errors->len
           ? ((PnFigureError *) g_ptr_array_index (self->errors, n))->message
           : NULL;
}

static void
test_every_verb (void)
{
    /* One of everything the language has, at an arity the table
     * accepts.  What the literals MEAN is 80.22.4's business, so
     * "black" and "dot" are just strings here. */
    Split s = checked ("view 0, 0, 100, 100\n"      /*  0 */
                       "color \"black\"\n"          /*  1 */
                       "fill 1, 0, 0\n"             /*  2 */
                       "nofill\n"                   /*  3 */
                       "width 2\n"                  /*  4 */
                       "dash \"dot\"\n"             /*  5 */
                       "dash \"dash\", 2\n"         /*  6 */
                       "font 10\n"                  /*  7 */
                       "align \"left\"\n"           /*  8 */
                       "align \"left\", \"top\"\n"  /*  9 */
                       "move 1, 2\n"                /* 10 */
                       "rmove 1, 2\n"               /* 11 */
                       "lineto 1, 2\n"              /* 12 */
                       "rline 1, 2\n"               /* 13 */
                       "line 1, 2, 3, 4\n"          /* 14 */
                       "point 1, 2\n"               /* 15 */
                       "circle 1, 2, 3\n"           /* 16 */
                       "arc 1, 2, 3, 0, 90\n"       /* 17 */
                       "rect 1, 2, 3, 4\n"          /* 18 */
                       "poly 0,0, 1,0, 1,1\n"       /* 19 */
                       "path 0,0, 1,0, 1,1\n"       /* 20 */
                       "text 50, 50, \"%.1f\", v\n" /* 21 */
                       "color 1, 0, 0, 0.5");       /* 22 */

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 23);

    PN_CHECK_CMPINT (statement (&s, 0)->verb,  ==, PN_FIGURE_VERB_VIEW);
    PN_CHECK_CMPINT (statement (&s, 3)->verb,  ==, PN_FIGURE_VERB_NOFILL);
    PN_CHECK_CMPINT (statement (&s, 12)->verb, ==, PN_FIGURE_VERB_LINETO);
    PN_CHECK_CMPINT (statement (&s, 19)->verb, ==, PN_FIGURE_VERB_POLY);
    PN_CHECK_CMPINT (statement (&s, 21)->verb, ==, PN_FIGURE_VERB_TEXT);
    PN_CHECK_CMPINT (statement (&s, 22)->verb, ==, PN_FIGURE_VERB_COLOR);

    split_free (&s);
}

static void
test_assignments_pass_through (void)
{
    Split s = checked ("dx = 50 * cos(a)\ncircle 0, 0, dx");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 2);
    /* An assignment has no verb and is not looked for in the table --
     * which is what lets a variable be called `line` (80.2 rule 1). */
    PN_CHECK_CMPINT (statement (&s, 0)->verb, ==, PN_FIGURE_VERB_NONE);
    PN_CHECK_CMPINT (statement (&s, 1)->verb, ==, PN_FIGURE_VERB_CIRCLE);

    split_free (&s);
}

static void
test_unknown_verb (void)
{
    Split          s = checked ("width 2\nCircel 1, 2, 3\nline = 4");
    PnFigureError *error;

    /* The bad statement does not travel; the good ones do, including
     * the assignment that a verb name would have shadowed. */
    PN_CHECK_CMPINT (s.statements->len, ==, 2);
    PN_CHECK_CMPSTR (statement (&s, 1)->name, ==, "line");
    PN_CHECK_CMPINT (statement (&s, 1)->kind, ==,
                     PN_FIGURE_STATEMENT_ASSIGNMENT);

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 2);
    PN_CHECK_CMPINT (error->column, ==, 1);
    /* Quoted as typed, not as folded. */
    PN_CHECK_CMPSTR (error->message, ==, "unknown verb \"Circel\"");

    split_free (&s);
}

static void
test_wrong_arity (void)
{
    Split s = checked ("line 1, 2, 3\n"
                       "nofill 1\n"
                       "width\n"
                       "poly 0,0, 1,1\n"
                       "dash \"dot\", 2, 3\n"
                       "text 0, 0");

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 6);

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "line takes 4 arguments, not 3");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "nofill takes 0 arguments, not 1");
    /* Singular when it is one. */
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "width takes 1 argument, not 0");
    PN_CHECK_CMPSTR (error_text (&s, 3), ==,
                     "poly takes at least 6 arguments, not 4");
    PN_CHECK_CMPSTR (error_text (&s, 4), ==,
                     "dash takes 1 or 2 arguments, not 3");
    PN_CHECK_CMPSTR (error_text (&s, 5), ==,
                     "text takes at least 3 arguments, not 2");

    split_free (&s);
}

static void
test_pairs (void)
{
    Split          s = checked ("poly 0,0, 1,1, 2,2, 3");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==, "poly takes x and y in pairs");
    /* At the verb, which is where the count is wrong. */
    PN_CHECK_CMPINT (error->column, ==, 1);

    split_free (&s);
}

static void
test_argument_kinds (void)
{
    Split          s = checked ("width \"2\"\n"
                                "dash 3\n"
                                "align \"left\", 1\n"
                                "text 0, 0, 1");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 4);

    /* Reported AT the argument, so the message does not have to say
     * which one it means. */
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==,
                     "expected an expression, not a string");
    PN_CHECK_CMPINT (error->column, ==, 7);

    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "expected a quoted string");
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "expected a quoted string");

    error = g_ptr_array_index (s.errors, 3);
    PN_CHECK_CMPSTR (error->message, ==, "expected a quoted string");
    PN_CHECK_CMPINT (error->column, ==, 12);

    split_free (&s);
}

static void
test_colour_has_two_spellings (void)
{
    Split s = checked ("color \"#202020\"\n"
                       "fill 1, 0, 0\n"
                       "fill 1, 0, 0, 0.5\n"
                       "color 1, 0");

    /* One quoted literal, or three-to-four expressions, and nothing
     * between (80.5). */
    PN_CHECK_CMPINT (s.statements->len, ==, 3);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "color takes a quoted colour or 3 or 4 numbers, not 2");

    split_free (&s);
}

static void
test_colour_argument_kinds (void)
{
    Split          s = checked ("color c\nfill 1, \"0\", 0");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 2);

    /* The one-argument form points at the other spelling rather than
     * asking for a string and leaving it there. */
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==,
                     "expected a quoted colour or 3 or 4 numbers");
    PN_CHECK_CMPINT (error->column, ==, 7);

    error = g_ptr_array_index (s.errors, 1);
    PN_CHECK_CMPSTR (error->message, ==,
                     "expected an expression, not a string");
    PN_CHECK_CMPINT (error->column, ==, 9);

    split_free (&s);
}

static void
test_check_returns_and_keeps_going (void)
{
    GPtrArray *errors     = pn_figure_errors_new ();
    GPtrArray *lines      = pn_figure_scan ("width 2\nfoo\nline 1,2,3", errors);
    GPtrArray *statements = pn_figure_split (lines, errors);

    PN_CHECK_CMPINT (statements->len, ==, 3);
    PN_CHECK_FALSE (pn_figure_check_verbs (statements, errors));
    /* Both bad statements were found, not just the first. */
    PN_CHECK_CMPINT (errors->len, ==, 2);
    PN_CHECK_CMPINT (statements->len, ==, 1);
    PN_CHECK (pn_figure_check_verbs (statements, errors));

    g_ptr_array_unref (statements);
    g_ptr_array_unref (lines);
    g_ptr_array_unref (errors);
}

/* ------------------------------------------------------------------ */
/*  Literals                                                           */
/* ------------------------------------------------------------------ */

/* The whole front end as it stands: scan, split, check, read literals. */
static Split
literals (const gchar *program)
{
    Split result = checked (program);

    pn_figure_parse_literals (result.statements, result.errors);
    return result;
}

static void
test_colour_literals (void)
{
    Split        s = literals ("color \"#202020\"\n"
                               "fill \"red\"\n"
                               "color \"transparent\"\n"
                               "fill 1, 0, 0");
    PnFigureArg *a;

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK_CMPINT (s.statements->len, ==, 4);

    a = arg (statement (&s, 0), 0);
    PN_CHECK_NEAR (a->color.red,   32 / 255.0, 1e-9);
    PN_CHECK_NEAR (a->color.alpha, 1.0,        1e-9);

    /* A name, through the table 80.5(b) put in pn-color.c. */
    a = arg (statement (&s, 1), 0);
    PN_CHECK_NEAR (a->color.red,   1.0, 1e-9);
    PN_CHECK_NEAR (a->color.green, 0.0, 1e-9);

    a = arg (statement (&s, 2), 0);
    PN_CHECK_NEAR (a->color.alpha, 0.0, 1e-9);

    split_free (&s);
}

static void
test_bad_colour_literal (void)
{
    Split          s = literals ("color \"banana\"\nfill \"#12345\"\nwidth 2");
    PnFigureError *error;

    /* Caught while reading the program, so there is no such thing as a
     * runtime colour error (80.5h). */
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPINT (s.errors->len, ==, 2);

    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==, "unknown colour \"banana\"");
    PN_CHECK_CMPINT (error->line, ==, 1);
    PN_CHECK_CMPINT (error->column, ==, 7);

    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "unknown colour \"#12345\"");

    split_free (&s);
}

static void
test_dash_styles (void)
{
    Split s = literals ("dash \"solid\"\n"
                        "dash \"dot\", 2\n"
                        "dash \"dash\"\n"
                        "dash \"dashdot\"");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (arg (statement (&s, 0), 0)->word, ==,
                     PN_FIGURE_DASH_SOLID);
    PN_CHECK_CMPINT (arg (statement (&s, 1), 0)->word, ==,
                     PN_FIGURE_DASH_DOT);
    PN_CHECK_CMPINT (arg (statement (&s, 2), 0)->word, ==,
                     PN_FIGURE_DASH_DASH);
    PN_CHECK_CMPINT (arg (statement (&s, 3), 0)->word, ==,
                     PN_FIGURE_DASH_DASHDOT);

    split_free (&s);
}

static void
test_bad_dash_style (void)
{
    Split          s = literals ("dash \"wiggly\"");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    /* The message names the whole vocabulary, since it is short. */
    PN_CHECK_CMPSTR (error->message, ==,
                     "expected \"solid\", \"dot\", \"dash\" or \"dashdot\"");
    PN_CHECK_CMPINT (error->column, ==, 6);

    split_free (&s);
}

static void
test_alignment_words (void)
{
    Split s = literals ("align \"left\"\n"
                        "align \"centre\", \"baseline\"\n"
                        "align \"center\", \"top\"\n"
                        "align \"right\", \"bottom\"");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (arg (statement (&s, 0), 0)->word, ==,
                     PN_FIGURE_HALIGN_LEFT);
    PN_CHECK_CMPINT (arg (statement (&s, 1), 0)->word, ==,
                     PN_FIGURE_HALIGN_CENTRE);
    PN_CHECK_CMPINT (arg (statement (&s, 1), 1)->word, ==,
                     PN_FIGURE_VALIGN_BASELINE);
    /* Both spellings of centre, for the same reason pn-color.c takes
     * both grey and gray. */
    PN_CHECK_CMPINT (arg (statement (&s, 2), 0)->word, ==,
                     PN_FIGURE_HALIGN_CENTRE);
    PN_CHECK_CMPINT (arg (statement (&s, 3), 1)->word, ==,
                     PN_FIGURE_VALIGN_BOTTOM);

    split_free (&s);
}

static void
test_alignment_words_are_per_axis (void)
{
    /* "middle" is a real word in the wrong place, and the message says
     * which words belong there rather than that this one is unknown. */
    Split s = literals ("align \"middle\"\nalign \"left\", \"right\"");

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "expected \"left\", \"centre\" or \"right\"");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==,
                     "expected \"top\", \"middle\", \"baseline\" or \"bottom\"");

    split_free (&s);
}

static void
test_format_conversions (void)
{
    Split s = literals ("text 0, 0, \"A\"\n"
                        "text 0, 0, \"%.1f deg\", a * 57.2958\n"
                        "text 0, 0, \"100%%\"\n"
                        "text 0, 0, \"%-8.3e and %+G\", u, v\n"
                        "text 0, 0, \"%f\", a");

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK_CMPINT (s.statements->len, ==, 5);

    /* The conversion count lands on the format argument. */
    PN_CHECK_CMPINT (arg (statement (&s, 0), 2)->word, ==, 0);
    PN_CHECK_CMPINT (arg (statement (&s, 1), 2)->word, ==, 1);
    /* "%%" is a literal per cent, not a conversion. */
    PN_CHECK_CMPINT (arg (statement (&s, 2), 2)->word, ==, 0);
    PN_CHECK_CMPINT (arg (statement (&s, 3), 2)->word, ==, 2);

    split_free (&s);
}

static void
test_format_rejects_unsafe (void)
{
    /* This one is safety, not style: %s would dereference a double and
     * %n would write through one (80.7c). */
    Split s = literals ("text 0, 0, \"%s\", a\n"
                        "text 0, 0, \"%n\", a\n"
                        "text 0, 0, \"%d\", a\n"
                        "text 0, 0, \"%*f\", a\n"
                        "text 0, 0, \"%lf\", a\n"
                        "text 0, 0, \"100%\"");

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 6);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "unsupported conversion \"%s\"");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "unsupported conversion \"%n\"");
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "unsupported conversion \"%d\"");
    /* A "*" width would eat one of the values. */
    PN_CHECK_CMPSTR (error_text (&s, 3), ==, "unsupported conversion \"%*\"");
    /* A length modifier would change the argument's type. */
    PN_CHECK_CMPSTR (error_text (&s, 4), ==, "unsupported conversion \"%l\"");
    /* A trailing per cent is an unfinished conversion. */
    PN_CHECK_CMPSTR (error_text (&s, 5), ==, "unsupported conversion \"%\"");

    split_free (&s);
}

static void
test_format_value_count (void)
{
    Split          s = literals ("text 0, 0, \"%.1f %.1f\", a\n"
                                 "text 0, 0, \"A\", a\n"
                                 "text 0, 0, \"%f\"");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 3);

    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==, "format needs 2 values, not 1");
    /* Reported at the format, which is the thing to go and count. */
    PN_CHECK_CMPINT (error->column, ==, 12);

    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "format needs 0 values, not 1");
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "format needs 1 value, not 0");

    split_free (&s);
}

static void
test_literals_keep_going (void)
{
    Split s = literals ("color \"banana\"\ndash \"wiggly\"\nwidth 2\n"
                        "text 0, 0, \"%.1f\", a");

    /* Every bad literal found in one pass, the good statements kept. */
    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    PN_CHECK_CMPINT (s.statements->len, ==, 2);
    PN_CHECK_CMPSTR (statement (&s, 0)->name, ==, "width");
    PN_CHECK_CMPSTR (statement (&s, 1)->name, ==, "text");

    split_free (&s);
}

/* ------------------------------------------------------------------ */
/*  Expressions                                                        */
/* ------------------------------------------------------------------ */

/* The whole front end, every stage of it. */
static Split
parsed (const gchar *program)
{
    Split result = literals (program);

    pn_figure_parse_expressions (result.statements, result.errors);
    return result;
}

static void
test_constant_folding (void)
{
    Split        s = parsed ("view -60, -25, 60, 35\n"
                             "width 2 * 3\n"
                             "circle 0, 0, 10 / 4");
    PnFigureArg *a;

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK_CMPINT (s.statements->len, ==, 3);

    /* Most of a figure is constants, and none of them survive as a
     * tree to be walked every frame (80.3b). */
    a = arg (statement (&s, 0), 0);
    PN_CHECK (a->folded);
    PN_CHECK (a->ast == NULL);
    PN_CHECK_NEAR (a->value, -60.0, 1e-12);

    a = arg (statement (&s, 1), 0);
    PN_CHECK (a->folded);
    PN_CHECK_NEAR (a->value, 6.0, 1e-12);

    a = arg (statement (&s, 2), 2);
    PN_CHECK (a->folded);
    PN_CHECK_NEAR (a->value, 2.5, 1e-12);

    split_free (&s);
}

static void
test_variable_arguments_keep_their_tree (void)
{
    Split        s = parsed ("line -dx, -dy, dx, dy\ntext 0, 0, \"A\"");
    PnFigureArg *a;

    PN_CHECK_CMPINT (s.errors->len, ==, 0);

    a = arg (statement (&s, 0), 0);
    PN_CHECK_FALSE (a->folded);
    PN_CHECK (a->ast != NULL);

    /* A string is not an expression and never grows a tree. */
    a = arg (statement (&s, 1), 2);
    PN_CHECK_FALSE (a->folded);
    PN_CHECK (a->ast == NULL);
    PN_CHECK_CMPSTR (a->text, ==, "A");

    split_free (&s);
}

static void
test_constants_fold_too (void)
{
    /* pi and e are the language's own, so an expression using them is
     * still constant -- and they are not names the program expects
     * from outside (80.3d). */
    Split      s     = parsed ("circle 0, 0, 2 * pi\nwidth e");
    GPtrArray *names = pn_figure_free_names (s.statements);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK (arg (statement (&s, 0), 2)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 0), 2)->value, 2.0 * G_PI, 1e-12);
    PN_CHECK_NEAR (arg (statement (&s, 1), 0)->value, G_E, 1e-12);
    PN_CHECK_CMPINT (names->len, ==, 0);

    g_ptr_array_unref (names);
    split_free (&s);
}

static void
test_assignment_parses_whole (void)
{
    Split s = parsed ("dx = 50 * cos(a)\nline 0, 0, dx, 0");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    /* The whole line went to the calculator, which returns the ASSIGN
     * node the evaluator already knows how to bind. */
    PN_CHECK (statement (&s, 0)->ast != NULL);
    PN_CHECK_CMPINT (statement (&s, 0)->ast->type, ==, PN_EXPR_NODE_ASSIGN);
    PN_CHECK_CMPSTR (statement (&s, 0)->ast->name, ==, "dx");

    split_free (&s);
}

static void
test_free_names (void)
{
    Split      s = parsed ("dx = 50 * cos(a)\n"
                           "dy = 50 * sin(a)\n"
                           "line -dx, -dy, dx, dy\n"
                           "text 0, 0, \"%.1f\", a * 57.2958");
    GPtrArray *names = pn_figure_free_names (s.statements);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    /* Sorted, no repeats, and the assigned names are in it because the
     * program also reads them. */
    PN_CHECK_CMPINT (names->len, ==, 3);
    PN_CHECK_CMPSTR (g_ptr_array_index (names, 0), ==, "a");
    PN_CHECK_CMPSTR (g_ptr_array_index (names, 1), ==, "dx");
    PN_CHECK_CMPSTR (g_ptr_array_index (names, 2), ==, "dy");

    g_ptr_array_unref (names);
    split_free (&s);
}

static void
test_expression_parse_error (void)
{
    Split          s = parsed ("width 2\ncircle 0, 0, (1 + 2");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);

    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->line, ==, 2);
    /* The argument starts at column 14 and the parser stopped at
     * position 7 within it, so the column is the two put together and
     * the message keeps neither. */
    PN_CHECK_CMPINT (error->column, ==, 20);
    PN_CHECK_CMPSTR (error->message, ==, "expected ')'");

    split_free (&s);
}

static void
test_expression_error_without_a_position (void)
{
    /* Not every message carries one; the argument's own place stands
     * in, which is never wrong, only less precise. */
    Split          s = parsed ("circle 0, 0, 1 +");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPINT (error->column, ==, 14);
    PN_CHECK_CMPSTR (error->message, ==, "unexpected end of expression");

    split_free (&s);
}

static void
test_unknown_function_is_a_parse_error (void)
{
    /* A constant argument is evaluated now, so the one way it can fail
     * -- a function that does not exist -- is caught now too. */
    Split s = parsed ("circle 0, 0, wibble(2)");

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "unknown function 'wibble'");

    split_free (&s);
}

static void
test_atan2_is_a_function (void)
{
    /* The gap #81 existed to close, now closed.  The two commas in
     * `circle 0, 0, atan2(1, 2)` mean different things and the splitter
     * has always known it: the first two are the verb's separators, the
     * third is at paren depth 1 and so belongs to the call, which is why
     * the splitter keeps `atan2(1, 2)` whole and hands it to the
     * calculator entire.  What used to happen next was a lexer meeting a
     * comma it had no token for (80.3e); now it is an ordinary
     * two-argument call, folded because both arguments are constants. */
    Split s = parsed ("circle 0, 0, atan2(1, 2)");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK (arg (statement (&s, 0), 2)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 0), 2)->value, atan2 (1.0, 2.0), 1e-12);

    split_free (&s);
}

/* TODO #83.1 predicted this file would need NOTHING: the figure's two
 * AST walkers recurse into .left and .right generically, so a call's
 * arguments 2..N riding on a chain of ARG nodes are walked for free.
 * Re-confirmed rather than assumed, and on the case that would catch a
 * walker that stops at argument two — a free name in the LAST argument
 * of a three-argument call, plus a constant fold through the chain. */
static void
test_a_three_argument_call_walks (void)
{
    Split      s = parsed ("circle 0, 0, clamp(r, 1, 10)\n"
                           "line 0, 0, clamp(5, 0, 10), log(8, 2)");
    GPtrArray *names = pn_figure_free_names (s.statements);

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 2);

    /* `r` is the only free name, and it is found although it sits in
     * argument ONE of a call whose other arguments chain behind it. */
    PN_CHECK_CMPINT (names->len, ==, 1);
    PN_CHECK_CMPSTR (g_ptr_array_index (names, 0), ==, "r");

    /* The all-constant calls fold, which walks the chain a second way —
     * three arguments, and a ranged arity taking its two. */
    PN_CHECK (arg (statement (&s, 1), 2)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 1), 2)->value, 5.0, 1e-12);
    PN_CHECK (arg (statement (&s, 1), 3)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 1), 3)->value, 3.0, 1e-12);

    g_ptr_array_unref (names);
    split_free (&s);
}

static void
test_non_finite_constant_is_not_an_error (void)
{
    /* A value, not a bug (80.10b): the resolver skips the statement
     * and the node does not go red. */
    Split s = parsed ("circle 0, 0, 1 / 0");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK (arg (statement (&s, 0), 2)->folded);
    PN_CHECK (isinf (arg (statement (&s, 0), 2)->value));

    split_free (&s);
}

/* ------------------------------------------------------------------ */
/*  Reporting                                                          */
/* ------------------------------------------------------------------ */

static void
test_report_nothing_wrong (void)
{
    Split  s    = parsed ("view 0, 0, 100, 100\ncircle 50, 50, 40");
    gchar *text = pn_figure_errors_to_string (s.errors);

    PN_CHECK (text == NULL);
    PN_CHECK (pn_figure_errors_to_string (NULL) == NULL);

    g_free (text);
    split_free (&s);
}

static void
test_report_one_error (void)
{
    Split  s    = parsed ("width 2\ncircle 0, 0");
    gchar *text = pn_figure_errors_to_string (s.errors);

    PN_CHECK_CMPSTR (text, ==,
                     "line 2, column 1: circle takes 3 arguments, not 2");

    g_free (text);
    split_free (&s);
}

static void
test_report_counts_and_takes_the_earliest (void)
{
    /* The unterminated string on line 3 is found by the FIRST stage
     * and the bad verb on line 1 by the third, so list order is not
     * program order -- and a person reads their program top to
     * bottom. */
    Split  s    = parsed ("circle 0, 0\n"
                          "width 2\n"
                          "text 0, 0, \"A\n"
                          "dash \"wiggly\"");
    gchar *text = pn_figure_errors_to_string (s.errors);

    PN_CHECK_CMPINT (s.errors->len, ==, 3);
    PN_CHECK_CMPSTR (text, ==,
                     "line 1, column 1: circle takes 3 arguments, not 2\n"
                     "3 errors, first on line 1");

    g_free (text);
    split_free (&s);
}

/* A statement that fails a stage is removed before the next one sees
 * it, and each stage gives up on a statement at its first bad
 * argument -- so no program produces two errors on one line, and the
 * column tie-break is tested against the formatter directly. */
static void
add_error (GPtrArray *errors, gint line, gint column, const gchar *message)
{
    PnFigureError *error = g_new0 (PnFigureError, 1);

    error->line    = line;
    error->column  = column;
    error->message = g_strdup (message);
    g_ptr_array_add (errors, error);
}

static void
test_report_earliest_on_a_line (void)
{
    GPtrArray *errors = pn_figure_errors_new ();
    gchar     *text;

    add_error (errors, 2, 9, "second");
    add_error (errors, 5, 1, "later");
    add_error (errors, 2, 3, "first");

    text = pn_figure_errors_to_string (errors);
    PN_CHECK_CMPSTR (text, ==, "line 2, column 3: first\n"
                               "3 errors, first on line 2");

    g_free (text);
    g_ptr_array_unref (errors);
}

static void
test_the_specimen_parses (void)
{
    /* The reference specimen from the head of TODO #80, which the
     * language was chosen against. */
    Split s = parsed ("view -60, -25, 60, 35\n"
                      "color \"#202020\"\n"
                      "width 2\n"
                      "\n"
                      "# the beam, tilted by the input angle\n"
                      "dx = 50 * cos(a)\n"
                      "dy = 50 * sin(a)\n"
                      "line -dx, -dy, dx, dy\n"
                      "\n"
                      "# fulcrum\n"
                      "fill \"#808080\"\n"
                      "poly 0,-2, -8,-14, 8,-14\n"
                      "nofill\n"
                      "\n"
                      "# weight hanging off the left end\n"
                      "rect -dx-6, -dy-16, 12, 10\n"
                      "\n"
                      "text -dx, -dy+6, \"A\"\n"
                      "text  dx,  dy+6, \"B\"\n"
                      "text 0, 22, \"%.1f deg\", a * 57.2958");
    GPtrArray *names;

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK (pn_figure_errors_to_string (s.errors) == NULL);
    PN_CHECK_CMPINT (s.statements->len, ==, 13);

    /* One input, two working variables. */
    names = pn_figure_free_names (s.statements);
    PN_CHECK_CMPINT (names->len, ==, 3);
    PN_CHECK_CMPSTR (g_ptr_array_index (names, 0), ==, "a");

    g_ptr_array_unref (names);
    split_free (&s);
}

/* ------------------------------------------------------------------ */
/*  The back end                                                       */
/* ------------------------------------------------------------------ */

/* A program plus what it reads, so a case can resolve the SAME parse
 * more than once -- which is how the promises that survive a repaint
 * are tested at all (80.5g, 80.8e). */
typedef struct
{
    Split      parse;
    GPtrArray *names;
} Figure;

static Figure
figure (const gchar *program)
{
    Figure self;

    self.parse = parsed (program);
    self.names = pn_figure_free_names (self.parse.statements);
    return self;
}

static void
figure_free (Figure *self)
{
    g_ptr_array_unref (self->names);
    split_free (&self->parse);
}

static gchar *
figure_dump (Figure           *self,
             PnFigureSnapshot *snapshot,
             gdouble           w,
             gdouble           h,
             gboolean          stretch,
             gchar           **out_error)
{
    GPtrArray *ops  = pn_figure_resolve (self->parse.statements, self->names,
                                         snapshot, 0, 0, w, h, stretch,
                                         out_error);
    gchar     *text = pn_figure_display_to_string (ops);

    g_ptr_array_unref (ops);
    return text;
}

/* One program, one frame, in the 100x100 rectangle where the scale is 1
 * and every expected number can be checked in the head: X = u and
 * Y = 100 - v (80.12c). */
static gchar *
dump100 (const gchar *program)
{
    Figure  f    = figure (program);
    gchar  *text = figure_dump (&f, NULL, 100, 100, FALSE, NULL);

    figure_free (&f);
    return text;
}

/* How many operations a dump holds, for the cases where the interesting
 * number is a COUNT -- a thousand identical points is not a string
 * anybody should write out. */
static gint
count_lines (const gchar *text)
{
    gint n = 0;

    for (; text != NULL && *text != '\0'; text++)
        if (*text == '\n')
            n++;

    return n;
}

/* ... of which this many are the head every dump begins with. */
static gint
head_lines (const gchar *head)
{
    return count_lines (head);
}

/* Every frame starts by putting the whole pen state into the display
 * list, so the painter holds no defaults of its own and 80.5(g)'s reset
 * is something a test can see.  Every expected dump begins with it. */
#define HEAD_100 \
    "# view 0 0 100 100 scale 1.00 rect 0.00 0.00 100.00 100.00\n" \
    "color rgb(0,0,0)\n" \
    "nofill\n" \
    "width 1.00\n" \
    "dash solid\n" \
    "font 5.00\n" \
    "align centre middle\n"

/* The same in the node's own 280x173 client area, where the default
 * window is letterboxed: s = 1.73, and 173 of the 280 pixels are used
 * with the other 107 split between the two bars. */
#define HEAD_280 \
    "# view 0 0 100 100 scale 1.73 rect 53.50 0.00 173.00 173.00\n" \
    "color rgb(0,0,0)\n" \
    "nofill\n" \
    "width 1.73\n" \
    "dash solid\n" \
    "font 8.65\n" \
    "align centre middle\n"

static void
test_an_empty_program_is_a_blank_figure (void)
{
    gchar *text = dump100 ("");

    /* Not an error, not nothing: the window is still established, which
     * is what makes an empty figure read as a deliberate area. */
    PN_CHECK_CMPSTR (text, ==, HEAD_100);
    g_free (text);
}

static void
test_the_state_verbs (void)
{
    gchar *text = dump100 ("color \"#202020\"\n"
                           "fill \"grey\"\n"
                           "nofill\n"
                           "width 2\n"
                           "dash \"dot\"\n"
                           "font 10\n"
                           "align \"left\", \"top\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(32,32,32)\n"
                     "fill rgb(128,128,128)\n"
                     "nofill\n"
                     "width 2.00\n"
                     "dash 0.50 1.50\n"
                     "font 10.00\n"
                     "align left top\n");
    g_free (text);
}

static void
test_the_geometry_verbs (void)
{
    gchar *text = dump100 ("move 10, 10\n"
                           "rmove 5, 0\n"
                           "lineto 30, 40\n"
                           "rline 10, 10\n"
                           "line 0, 0, 50, 50\n"
                           "point 20, 20\n"
                           "circle 50, 50, 10\n"
                           "arc 50, 50, 10, 0, 90\n"
                           "rect 10, 20, 30, 40\n"
                           "poly 0,0, 10,0, 10,10\n"
                           "path 0,0, 10,0, 10,10\n"
                           "text 5, 5, \"%.1f\", 2.5");

    /* `move`, `lineto`, `rline` and `line` all resolve against the pen,
     * which starts each frame at the user origin and follows every
     * segment's far end (80.6a); the shapes leave it alone. */
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "move 10.00 90.00\n"
                     "move 15.00 90.00\n"
                     "line 15.00 90.00 30.00 60.00\n"
                     "line 30.00 60.00 40.00 50.00\n"
                     "line 0.00 100.00 50.00 50.00\n"
                     "point 20.00 80.00 1.00\n"
                     "circle 50.00 50.00 10.00\n"
                     "arc 50.00 50.00 10.00 0.00 -90.00 negative\n"
                     "rect 10.00 40.00 30.00 40.00\n"
                     "poly 0.00 100.00 10.00 100.00 10.00 90.00\n"
                     "path 0.00 100.00 10.00 100.00 10.00 90.00\n"
                     "text 5.00 95.00 centre middle \"2.5\"\n");
    g_free (text);
}

static void
test_y_points_up (void)
{
    /* The first thing everyone gets wrong (80.4), so it gets a case of
     * its own: user y = 0 is the BOTTOM of the client area. */
    gchar *text = dump100 ("line 0, 0, 0, 100");

    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 0.00 0.00\n");
    g_free (text);
}

static void
test_rect_takes_its_lower_left_corner (void)
{
    /* Which is a DIFFERENT device corner, and a negative extent takes
     * the other one again (80.6f). */
    gchar *a = dump100 ("rect 10, 20, 30, 40");
    gchar *b = dump100 ("rect 40, 60, -30, -40");

    PN_CHECK_CMPSTR (a, ==, HEAD_100 "rect 10.00 40.00 30.00 40.00\n");
    PN_CHECK_CMPSTR (b, ==, a);
    g_free (a);
    g_free (b);
}

static void
test_an_arc_sweeps_the_way_it_was_written (void)
{
    /* 80.6(d)'s own worked example, and the one the y flip is laid for:
       `0, 90` is a counter-clockwise quarter and `90, 0` the clockwise
       quarter BACK, not the three quarters the long way round.  Our
       flip makes a counter-clockwise user sweep a clockwise device one,
       so the two differ by which cairo call the painter makes -- and
       the device angles are the same pair either way. */
    gchar *text = dump100 ("arc 50, 50, 10, 0, 90\n"
                           "arc 50, 50, 20, 90, 0\n"
                           "arc 50, 50, 30, 0, 360");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "arc 50.00 50.00 10.00 0.00 -90.00 negative\n"
                     "arc 50.00 50.00 20.00 -90.00 0.00 positive\n"
                     "arc 50.00 50.00 30.00 0.00 -360.00 negative\n");
    g_free (text);
}

static void
test_a_reversed_axis_turns_an_arc_around (void)
{
    /* With x increasing leftwards the whole picture is mirrored, so the
       sweep a program wrote counter-clockwise comes out the other way
       -- and the painter is told so rather than working it out. */
    Figure  f    = figure ("view 100, 0, 0, 100\n"
                           "arc 50, 50, 10, 0, 90\n"
                           "arc 50, 50, 20, 90, 0");
    gchar  *text = figure_dump (&f, NULL, 100, 100, FALSE, NULL);

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 100 0 0 100 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00\n"
                     "arc 50.00 50.00 10.00 180.00 270.00 positive\n"
                     "arc 50.00 50.00 20.00 270.00 180.00 negative\n");

    g_free (text);
    figure_free (&f);
}

static void
test_the_window_is_letterboxed (void)
{
    Figure  f    = figure ("view -60, -25, 60, 35\nline -60, -25, 60, 35");
    gchar  *text = figure_dump (&f, NULL, 280, 173, FALSE, NULL);

    /* 120 by 60 user units into 280 by 173 device ones: the width runs
     * out first, so s = 2.3333, the drawing is 280 by 140, and the 33
     * pixels left over are split into two bars of 16.5.
     *
     * A new scale also re-emits the state that DEPENDS on it: the pen
     * is in user units, so `width 1` is 1.73 device pixels before the
     * view and 2.33 after it. */
    PN_CHECK_CMPSTR (text, ==, HEAD_280
                     "# view -60 -25 60 35 scale 2.33"
                     " rect 0.00 16.50 280.00 140.00\n"
                     "width 2.33\n"
                     "dash solid\n"
                     "font 11.67\n"
                     "line 0.00 156.50 280.00 16.50\n");
    g_free (text);
    figure_free (&f);
}

static void
test_reversed_bounds_flip_an_axis (void)
{
    /* Legal, and it costs nothing: x now increases leftwards (80.4d).
     * The scale is unchanged, so nothing is re-emitted. */
    gchar *text = dump100 ("view 100, 0, 0, 100\n"
                           "line 0, 0, 100, 0\n"
                           "arc 50, 50, 10, 0, 90");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 100 0 0 100 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00\n"
                     "line 100.00 100.00 0.00 100.00\n"
                     /* One flip cancels the other: the sweep goes the
                      * positive way round after all (80.6d). */
                     "arc 50.00 50.00 10.00 180.00 270.00 positive\n");
    g_free (text);
}

static void
test_a_degenerate_view_is_skipped (void)
{
    /* A window with no extent is a VALUE, not a program error -- a knob
     * winding through zero makes one -- so the statement is skipped and
     * the previous window stands (80.4d, 80.10b). */
    gchar *text = dump100 ("view -50, -50, 50, 50\n"
                           "view 0, 0, 0, 100\n"
                           "line -50, -50, 50, 50");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view -50 -50 50 50 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00\n"
                     "# skip 2 degenerate\n"
                     "line 0.00 100.00 100.00 0.00\n");
    g_free (text);
}

static void
test_the_pen_resets_every_frame (void)
{
    Figure  f = figure ("color \"red\"\n"
                        "width 4\n"
                        "fill \"blue\"\n"
                        "align \"left\"\n"
                        "move 10, 10\n"
                        "rline 20, 0\n"
                        "text 0, 0, \"x\"");
    gchar  *first  = figure_dump (&f, NULL, 100, 100, FALSE, NULL);
    gchar  *second = figure_dump (&f, NULL, 100, 100, FALSE, NULL);

    /* Twice the same program, twice the same picture.  Persisting the
     * pen -- or the pen POSITION, which `rline` and `text` both read --
     * would make frame N depend on frame N-1 (80.5g). */
    PN_CHECK_CMPSTR (second, ==, first);
    PN_CHECK_CMPSTR (first, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "width 4.00\n"
                     "fill rgb(0,0,255)\n"
                     "align left middle\n"
                     "move 10.00 90.00\n"
                     "line 10.00 90.00 30.00 90.00\n"
                     "text 0.00 100.00 left middle \"x\"\n");
    g_free (first);
    g_free (second);
    figure_free (&f);
}

static void
test_an_unwired_figure_still_draws (void)
{
    /* `a` is bound by nothing at all, and an unbound name FAILS an
     * evaluation in PnVarStore rather than reading as zero -- so
     * without the zero-fill of 80.2 rule 12 this program would draw
     * nothing and say something unhelpful. */
    gchar *text = dump100 ("dx = 50 * cos(a)\nline 0, 0, dx, 0");

    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 50.00 100.00\n");
    g_free (text);
}

static void
test_the_constants_are_bound (void)
{
    /* `pi` folds at parse time in an argument, but an assignment never
     * folds (80.22.5c), so the store has to carry the constants too or
     * `r` here would fail to evaluate. */
    gchar *text = dump100 ("r = 10 * pi / pi\ncircle 50, 50, r");

    PN_CHECK_CMPSTR (text, ==, HEAD_100 "circle 50.00 50.00 10.00\n");
    g_free (text);
}

static void
test_the_snapshot_survives_a_repaint (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, a, 0");
    gchar            *first;
    gchar            *second;

    pn_figure_snapshot_set (snapshot, "a", 30.0);

    first  = figure_dump (&f, snapshot, 100, 100, FALSE, NULL);
    second = figure_dump (&f, snapshot, 100, 100, FALSE, NULL);

    /* A figure repaints long after the message that last changed it, so
     * the latched inputs have to outlive the message (80.8e).  The
     * second frame is the repaint. */
    PN_CHECK_CMPSTR (first, ==, HEAD_100 "line 0.00 100.00 30.00 100.00\n");
    PN_CHECK_CMPSTR (second, ==, first);

    g_free (first);
    g_free (second);
    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_an_input_beats_the_zero_fill (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, a, 0");
    gchar            *text;

    pn_figure_snapshot_set (snapshot, "a", 0.0);
    text = figure_dump (&f, snapshot, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 0.00 100.00\n");
    g_free (text);

    /* And an assignment beats the input, because it runs later (80.3c). */
    figure_free (&f);
    f    = figure ("a = 70\nline 0, 0, a, 0");
    text = figure_dump (&f, snapshot, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 70.00 100.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_stretch_fills_the_rectangle (void)
{
    Figure  f    = figure ("view 0, 0, 100, 50\nline 0, 0, 100, 50");
    gchar  *text = figure_dump (&f, NULL, 100, 100, TRUE, NULL);

    /* Aspect thrown away: 50 user units up the page become 100 device
     * ones, and the two per-axis scales part company.  They are what a
     * circle is drawn inside so it becomes the right ellipse (80.6e),
     * so the dump says them. */
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 0 0 100 50 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00 stretch 1.00 2.00\n"
                     "line 0.00 100.00 100.00 0.00\n");
    g_free (text);
    figure_free (&f);
}

static void
test_width_zero_is_a_hairline (void)
{
    /* The PostScript convention and the one escape from "widths scale
     * with the view" (80.5c); everything else is clamped so a fine line
     * cannot vanish, and a dot is never smaller than that either. */
    gchar *text = dump100 ("width 0\npoint 10, 10\nwidth 0.1\npoint 20, 20");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 0.00\n"
                     "point 10.00 90.00 0.75\n"
                     "width 0.75\n"
                     "point 20.00 80.00 0.75\n");
    g_free (text);
}

static void
test_a_non_finite_value_skips_its_statement (void)
{
    gchar *error = NULL;
    Figure f     = figure ("circle 50, 50, 1/0\n"
                           "circle 50, 50, 0/x\n"
                           "line 0, 0, 10, 10");
    gchar *text  = figure_dump (&f, NULL, 100, 100, FALSE, &error);

    /* An infinity folded at parse time and a NaN computed this frame
     * are the same thing: the statement is skipped, the rest of the
     * figure is drawn, and NOTHING is red (80.10b). */
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 non-finite\n"
                     "# skip 2 non-finite\n"
                     "line 0.00 100.00 10.00 90.00\n");
    PN_CHECK_CMPSTR (error, ==, NULL);
    g_free (text);
    figure_free (&f);
}

static void
test_a_zero_radius_skips_its_statement (void)
{
    gchar *text = dump100 ("circle 50, 50, 0\n"
                           "arc 50, 50, -1, 0, 90\n"
                           "font 0\n"
                           "width -1\n"
                           "dash \"dot\", 0\n"
                           "line 0, 0, 10, 10");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n"
                     "# skip 3 degenerate\n"
                     "# skip 4 degenerate\n"
                     "# skip 5 degenerate\n"
                     "line 0.00 100.00 10.00 90.00\n");
    g_free (text);
}

static void
test_a_vector_argument_is_an_error (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, v, 0\ncircle 5, 5, 5");
    gdouble           numbers[3] = { 1.0, 2.0, 3.0 };
    PnVector         *vec = pn_vector_new_copy (numbers, 3);
    gchar            *error = NULL;
    gchar            *text;

    pn_figure_snapshot_set_vector (snapshot, "v", vec);
    g_object_unref (vec);

    text = figure_dump (&f, snapshot, 100, 100, FALSE, &error);

    /* Unlike a NaN this never cures itself: someone wired a vector into
     * a figure that cannot animate yet.  So it is treated as a program
     * error -- nothing drawn, the node red -- with a message that names
     * the cause and the fix (80.10c).  Nothing at all is drawn, not
     * even the circle that came after it (80.10d). */
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (error, ==,
                     "line 1, column 12: vector argument;"
                     " animation is TODO 80.16");

    g_free (error);
    g_free (text);
    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

/* Binds @name to a vector of @len elements 0, 1, 2, ... */
static void
snapshot_ramp (PnFigureSnapshot *snapshot,
               const gchar      *name,
               gsize             len)
{
    gdouble  *numbers = g_new0 (gdouble, MAX (len, 1));
    PnVector *vec;
    gsize     i;

    for (i = 0; i < len; i++)
        numbers[i] = (gdouble) i;

    vec = pn_vector_new_copy (numbers, len);
    pn_figure_snapshot_set_vector (snapshot, name, vec);
    g_object_unref (vec);
    g_free (numbers);
}

static void
test_a_still_figure_is_one_frame (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("circle 50, 50, r");

    /* No vector anywhere is a still: one frame, not zero -- and a
     * scalar input changes nothing. */
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, NULL, 0), ==, 1);
    pn_figure_snapshot_set (snapshot, "r", 5.0);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 0), ==, 1);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_the_shortest_vector_sets_the_frame_count (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, a * b, c");

    /* The arithmetic would make `a * b` three elements long with a
     * pass-through tail; the film stops at two, where every input
     * still has a real value (TODO #82.1). */
    snapshot_ramp (snapshot, "a", 2);
    snapshot_ramp (snapshot, "b", 3);
    pn_figure_snapshot_set (snapshot, "c", 1.0);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 0), ==, 2);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_an_unread_vector_does_not_count (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("circle 50, 50, a");

    /* A wire the program ignores must not cut the film short. */
    snapshot_ramp (snapshot, "a", 10);
    snapshot_ramp (snapshot, "unused", 3);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 0), ==, 10);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_an_explicit_count_joins_the_minimum (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("circle 50, 50, a");

    /* With no vector input `frames` IS the film -- a wheel that turns
     * with nothing wired to it (80.17e).  With one, it is just another
     * length in the minimum: it can shorten the film, never stretch it
     * past the data. */
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, NULL, 40), ==, 40);
    snapshot_ramp (snapshot, "a", 10);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 4), ==, 4);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 40), ==, 10);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_an_empty_vector_leaves_no_frame (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, a, b");

    /* Zero, not one: there is no element 0 to draw, whatever `frames`
     * asks for. */
    snapshot_ramp (snapshot, "a", 5);
    snapshot_ramp (snapshot, "b", 0);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 0), ==, 0);
    PN_CHECK_CMPINT (pn_figure_frame_count (f.names, snapshot, 8), ==, 0);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_the_text_format_is_filled_in (void)
{
    gchar *text = dump100 ("text 50, 50, \"%.1f%% of %g\\nline two\", 12.34, 8");

    /* The conversions were checked at parse time, so the only work left
     * is the filling in -- through g_ascii_formatd(), because a label
     * must not read "12,3" on a machine with a comma separator. */
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "text 50.00 50.00 centre middle"
                     " \"12.3% of 8\\nline two\"\n");
    g_free (text);
}

static void
test_the_dump_is_locale_independent (void)
{
    gchar *saved = g_strdup (setlocale (LC_NUMERIC, NULL));
    gchar *text;

    if (setlocale (LC_NUMERIC, "de_DE.UTF-8") == NULL
        && setlocale (LC_NUMERIC, "fr_FR.UTF-8") == NULL
        && setlocale (LC_NUMERIC, "de_DE") == NULL)
    {
        /* No comma locale installed here, and the case must not fail
         * because of the machine it runs on (80.12d). */
        PN_CHECK (TRUE);
        g_free (saved);
        return;
    }

    text = dump100 ("line 0, 0, 12.5, 0\ntext 0, 0, \"%.1f\", 1.5");
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 12.50 100.00\n"
                     "text 0.00 100.00 centre middle \"1.5\"\n");
    g_free (text);

    setlocale (LC_NUMERIC, saved != NULL ? saved : "C");
    g_free (saved);
}

static void
test_the_specimen_draws (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f;
    gchar            *error = NULL;
    gchar            *text;

    /* The reference specimen from the head of TODO #80, in the node's
     * own client area, with its one input at rest. */
    f = figure ("view -60, -25, 60, 35\n"
                "color \"#202020\"\n"
                "width 2\n"
                "\n"
                "# the beam, tilted by the input angle\n"
                "dx = 50 * cos(a)\n"
                "dy = 50 * sin(a)\n"
                "line -dx, -dy, dx, dy\n"
                "\n"
                "# fulcrum\n"
                "fill \"#808080\"\n"
                "poly 0,-2, -8,-14, 8,-14\n"
                "nofill\n"
                "\n"
                "# weight hanging off the left end\n"
                "rect -dx-6, -dy-16, 12, 10\n"
                "\n"
                "text -dx, -dy+6, \"A\"\n"
                "text  dx,  dy+6, \"B\"\n"
                "text 0, 22, \"%.1f deg\", a * 57.2958\n");

    PN_CHECK_CMPINT (f.parse.errors->len, ==, 0);

    pn_figure_snapshot_set (snapshot, "a", 0.0);
    text = figure_dump (&f, snapshot, 280, 173, FALSE, &error);

    PN_CHECK_CMPSTR (error, ==, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_280
                     "# view -60 -25 60 35 scale 2.33"
                     " rect 0.00 16.50 280.00 140.00\n"
                     "width 2.33\n"
                     "dash solid\n"
                     "font 11.67\n"
                     "color rgb(32,32,32)\n"
                     "width 4.67\n"
                     "line 23.33 98.17 256.67 98.17\n"
                     "fill rgb(128,128,128)\n"
                     "poly 140.00 102.83 121.33 130.83 158.67 130.83\n"
                     "nofill\n"
                     "rect 9.33 112.17 28.00 23.33\n"
                     "text 23.33 84.17 centre middle \"A\"\n"
                     "text 256.67 84.17 centre middle \"B\"\n"
                     "text 140.00 46.83 centre middle \"0.0 deg\"\n");

    g_free (text);
    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_frame_needs_room (void)
{
    Figure  f    = figure ("line 0, 0, 10, 10");
    gchar  *text = figure_dump (&f, NULL, 0, 0, FALSE, NULL);

    /* A client area with no room in it maps nothing, and is not an
     * error either -- a card mid-animation is briefly this shape. */
    PN_CHECK_CMPSTR (text, ==, "");
    g_free (text);
    figure_free (&f);
}


/* ------------------------------------------------------------------ */
/*  The node                                                           */
/*                                                                     */
/*  What the front and back ends could not be asked on their own: the  */
/*  inputs becoming variables, the latch that keeps them between       */
/*  messages, and the `error` property that is this node's only        */
/*  channel to the person who typed the program (80.10f, 80.12e).      */
/* ------------------------------------------------------------------ */

/* A node carrying @program, with the ports already sized. */
static PnNode *
node (const gchar *program, gint inputs)
{
    PnNode *self = g_object_new (PN_TYPE_FIGURE, NULL);

    if (inputs > 1)
        g_object_set (self, "inputs", inputs, NULL);
    if (program != NULL)
        g_object_set (self, "program", program, NULL);
    return self;
}

/* One frame in the 100x100 rectangle, exactly as dump100() does it for
 * the back end. */
static gchar *
node_dump (PnNode *self)
{
    return pn_figure_dump (PN_FIGURE (self), 0, 0, 100, 100);
}

/* Deliver a value on @input, the way the worksheet delivers one. */
static void
send (PnNode *self, gint input, const gchar *key, gdouble value)
{
    PnMessage *message = pn_message_new (NULL, NULL);

    pn_message_set_double (message, key, value);
    pn_node_receive_message_on_input (self, message, input);
    g_object_unref (message);
}

/* ------------------------------------------------------------------ */
/*  The repeat block (TODO #86)                                        */
/* ------------------------------------------------------------------ */

static void
test_a_block_runs_its_body_n_times (void)
{
    /* Three passes, and `i` counting 0, 1, 2 — which is the whole of
     * 86.4: the index is an ordinary binding, set before each pass. */
    gchar *text = dump100 ("repeat 3\n"
                           "    circle 20 + 30 * i, 50, 5\n"
                           "end");

    /* Every operation carries the line it came from, so the loop is
     * assertable by the line numbers repeating (#86.7). */
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "circle 20.00 50.00 5.00\n"
                     "circle 50.00 50.00 5.00\n"
                     "circle 80.00 50.00 5.00\n");
    g_free (text);
}

static void
test_a_block_leaves_no_operation_of_its_own (void)
{
    /* `repeat` and `end` are control flow, not ink, exactly as an
     * assignment is (#86.7) — a one-pass block draws what the same
     * statements draw without it. */
    gchar *with    = dump100 ("repeat 1\nline 0, 0, 10, 10\nend");
    gchar *without = dump100 ("line 0, 0, 10, 10");

    PN_CHECK_CMPSTR (with, ==, without);
    g_free (with);
    g_free (without);
}

static void
test_the_index_beats_the_zero_fill (void)
{
    /* `i` is collected as a free name and zero-filled like everything
     * else before the frame runs; the loop rebinds it every pass, and
     * a binding wins (#86.4).  Outside the block the last value stands
     * — the block is shorthand, not a scope. */
    gchar *text = dump100 ("point i, 10\n"
                           "repeat 2\n"
                           "    point 20 + i, 50\n"
                           "end\n"
                           "point 90, i");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 0.00 90.00 1.00\n"
                     "point 20.00 50.00 1.00\n"
                     "point 21.00 50.00 1.00\n"
                     "point 90.00 99.00 1.00\n");
    g_free (text);
}

static void
test_pen_state_and_assignments_carry_across_passes (void)
{
    /* 86.6: the block is shorthand for writing the statements out, so
     * a `width` set inside it survives into the next pass and out the
     * far side, and an assignment accumulates exactly as the copied
     * lines would. */
    gchar *text = dump100 ("w = 1\n"
                           "repeat 2\n"
                           "    w = w + 1\n"
                           "    width w\n"
                           "    line 0, 0, 10, 10\n"
                           "end\n"
                           "line 0, 0, 20, 20");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 2.00\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "width 3.00\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "line 0.00 100.00 20.00 80.00\n");
    g_free (text);
}

static void
test_a_count_is_a_value_not_a_program (void)
{
    /* 86.5, which is 80.10(b) seen from the block: a count that is
     * zero, negative, NaN or infinite draws nothing and says why,
     * rather than reddening a node because a knob passed through
     * zero.  The figure after it still draws. */
    gchar *zero     = dump100 ("repeat 0\nline 0, 0, 10, 10\nend\n"
                               "point 50, 50");
    gchar *negative = dump100 ("repeat -3\nline 0, 0, 10, 10\nend");
    gchar *nan      = dump100 ("repeat 0 / 0\nline 0, 0, 10, 10\nend");
    gchar *huge     = dump100 ("repeat 1000000\nline 0, 0, 10, 10\nend");

    PN_CHECK_CMPSTR (zero, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "point 50.00 50.00 1.00\n");
    PN_CHECK_CMPSTR (negative, ==, HEAD_100 "# skip 1 degenerate\n");
    PN_CHECK_CMPSTR (nan, ==, HEAD_100 "# skip 1 non-finite\n");

    /* And the cap refuses out loud rather than clamping, because a
     * silently shortened loop draws a lie (#86.5). */
    PN_CHECK_CMPSTR (huge, ==, HEAD_100 "# skip 1 too-many\n");

    g_free (zero);
    g_free (negative);
    g_free (nan);
    g_free (huge);
}

static void
test_the_cap_is_the_last_count_that_runs (void)
{
    /* The boundary itself, so the cap cannot drift by one: the limit
     * runs, one more does not.  Counted by what the body drew. */
    gchar *at    = dump100 ("repeat 1000\npoint 0, 0\nend");
    gchar *over  = dump100 ("repeat 1001\npoint 0, 0\nend");
    gchar *fract = dump100 ("repeat 3.9\npoint 0, 0\nend");

    PN_CHECK_CMPINT (count_lines (at), ==, 1000 + head_lines (HEAD_100));
    PN_CHECK_CMPSTR (over, ==, HEAD_100 "# skip 1 too-many\n");

    /* A count truncates toward zero rather than rounding (#86.5). */
    PN_CHECK_CMPINT (count_lines (fract), ==, 3 + head_lines (HEAD_100));

    g_free (at);
    g_free (over);
    g_free (fract);
}

static void
test_a_grid_is_one_loop (void)
{
    /* The arithmetic 85.12 stands on, and the reason nesting is not
     * missed (#86.2): floor and the modulo it makes turn one index
     * into a row and a column. */
    gchar *text = dump100 ("cols = 3\n"
                           "repeat 6\n"
                           "    cx = i - cols * floor(i / cols)\n"
                           "    cy = floor(i / cols)\n"
                           "    point 20 + 30 * cx, 30 + 30 * cy\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 20.00 70.00 1.00\n"
                     "point 50.00 70.00 1.00\n"
                     "point 80.00 70.00 1.00\n"
                     "point 20.00 40.00 1.00\n"
                     "point 50.00 40.00 1.00\n"
                     "point 80.00 40.00 1.00\n");
    g_free (text);
}

static void
test_a_constant_inside_a_block_still_folds (void)
{
    /* An argument that does not read `i` is the same every pass, so it
     * folds exactly as it would outside the block, and one that does
     * cannot (#86.4). */
    Split s = parsed ("repeat 4\n"
                      "    circle 10 * i, 50, 2 + 3\n"
                      "end");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK (!arg (statement (&s, 1), 0)->folded);   /* 10 * i */
    PN_CHECK (arg (statement (&s, 1), 2)->folded);    /* 2 + 3  */
    PN_CHECK_NEAR (arg (statement (&s, 1), 2)->value, 5.0, 1e-12);

    /* The count folds too, being a constant like any other. */
    PN_CHECK (arg (statement (&s, 0), 0)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 0), 0)->value, 4.0, 1e-12);

    split_free (&s);
}

static void
test_an_unclosed_block_is_a_parse_error (void)
{
    Split s = checked ("repeat 3\nline 0, 0, 10, 10");

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "repeat without an end");
    PN_CHECK_CMPINT (error_line (&s, 0), ==, 1);
    split_free (&s);
}

static void
test_an_end_without_a_repeat_is_a_parse_error (void)
{
    Split s = checked ("line 0, 0, 10, 10\nend");

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "end without a repeat");
    PN_CHECK_CMPINT (error_line (&s, 0), ==, 2);
    split_free (&s);
}

static void
test_blocks_do_not_nest (void)
{
    /* 86.2, and the scan keeps going: a second structural mistake is
     * still reported, so the count in the message is honest. */
    Split s = checked ("repeat 2\n"
                       "    repeat 3\n"
                       "        point 0, 0\n"
                       "    end\n"
                       "end\n"
                       "end");

    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "repeat cannot be nested inside another repeat");
    PN_CHECK_CMPINT (error_line (&s, 0), ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "end without a repeat");
    PN_CHECK_CMPINT (error_line (&s, 1), ==, 6);
    split_free (&s);
}

static void
test_a_structural_error_draws_nothing (void)
{
    /* A program error is a program error (80.10a): the node paints the
     * message, not half a figure. */
    PnFigure *figure = pn_figure_new ();
    gchar    *text;

    g_object_set (figure, "program",
                  "line 0, 0, 10, 10\nrepeat 2\npoint 0, 0", NULL);

    text = pn_figure_dump (figure, 0, 0, 100, 100);
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (pn_figure_get_error (figure), ==,
                     "line 2, column 1: repeat without an end");

    g_free (text);
    g_object_unref (figure);
}

static void
test_the_node_is_a_sink (void)
{
    PnNode *self = node (NULL, 1);

    PN_CHECK        (pn_node_get_has_input  (self));
    PN_CHECK_FALSE  (pn_node_get_has_output (self));
    PN_CHECK_CMPINT (pn_node_get_n_inputs   (self), ==, 1);
    PN_CHECK_CMPSTR (pn_node_get_class_name (self), ==, "Figure");

    g_object_unref (self);
}

static void
test_a_fresh_node_draws (void)
{
    PnNode *self = node (NULL, 1);
    gchar  *text = node_dump (self);

    /* The default program of 80.11(g) draws with every variable
     * zero-filled, so a node dragged in from the palette is a figure
     * and not an empty box (80.8i). */
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");
    PN_CHECK_FALSE  (pn_node_get_has_error (self));
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     /* The program's own `view` re-states the window it
                      * was already given -- the default is the same one
                      * (80.4c), so the numbers repeat. */
                     "# view 0 0 100 100 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00\n"
                     "circle 50.00 50.00 40.00\n"
                     "text 50.00 50.00 centre middle \"0.0\"\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_an_input_becomes_a_variable (void)
{
    PnNode *self = node ("line 0, 0, value1, 0", 1);
    gchar  *text;

    /* Unwired, the name zero-fills and the line is a point. */
    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 0.00 100.00\n");
    g_free (text);

    /* The core does not collate a single-input node, so this is the
     * message's own data.value bound under the input's display name. */
    send (self, 0, "value", 60.0);
    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 60.00 100.00\n");
    g_free (text);

    g_object_unref (self);
}

static void
test_a_renamed_input_renames_the_variable (void)
{
    PnNode *self = node ("line 0, 0, angle, 0", 1);
    gchar  *text;

    pn_node_set_input_name (self, 0, "angle");
    send (self, 0, "value", 25.0);

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 25.00 100.00\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_the_inputs_are_latched (void)
{
    PnNode *self = node ("line 0, 0, value1, value2", 2);
    gchar  *text;

    /* Input 1 alone: value2 is still unbound and zero-fills. */
    send (self, 0, "value", 40.0);
    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 40.00 100.00\n");
    g_free (text);

    /* Input 2 arrives and input 1's value is still remembered, which is
     * the core's collation doing the work for us (80.8a). */
    send (self, 1, "value", 30.0);
    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 40.00 70.00\n");
    g_free (text);

    g_object_unref (self);
}

static void
test_a_sibling_member_takes_the_input_number (void)
{
    PnNode *self = node ("line 0, 0, temp1, 0", 1);
    gchar  *text;

    /* A numeric member that is not the headline value binds with the
     * arriving input's 1-based number suffixed, exactly as Calculator 2
     * binds it (80.8b). */
    send (self, 0, "temp", 75.0);

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 75.00 100.00\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_a_string_member_is_not_bound (void)
{
    PnNode    *self    = node ("line 0, 0, label1, 0", 1);
    PnMessage *message = pn_message_new (NULL, NULL);
    gchar     *text;

    /* A figure draws numbers: a string never becomes a variable, so the
     * name zero-fills instead (80.8f, and 80.7c from the other side). */
    pn_message_set_string (message, "label", "23");
    pn_node_receive_message_on_input (self, message, 0);
    g_object_unref (message);

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 0.00 100.00\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_the_node_snapshot_survives_a_repaint (void)
{
    PnNode *self = node ("circle 50, 50, value1", 1);
    gchar  *first;
    gchar  *second;

    /* One message, two frames: the bindings outlive the message that
     * set them, which is the whole reason the node keeps a snapshot
     * rather than rebuilding from the bag like Calculator 2 (80.8e). */
    send (self, 0, "value", 20.0);
    first  = node_dump (self);
    second = node_dump (self);

    PN_CHECK_CMPSTR (first, ==, HEAD_100 "circle 50.00 50.00 20.00\n");
    PN_CHECK_CMPSTR (second, ==, first);

    g_free (second);
    g_free (first);
    g_object_unref (self);
}

static void
test_a_program_error_reaches_the_property (void)
{
    PnNode *self = node ("width 2\ncircle 0, 0", 1);
    gchar  *text = node_dump (self);

    /* Class (a): the program is broken until someone edits it, so the
     * card shows the message, nothing at all is drawn, and the node
     * paints red (80.10a). */
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==,
                     "line 2, column 1: circle takes 3 arguments, not 2");
    PN_CHECK        (pn_node_get_has_error (self));
    PN_CHECK_CMPSTR (text, ==, "");

    g_free (text);
    g_object_unref (self);
}

static void
test_the_error_clears_on_a_good_program (void)
{
    PnNode *self = node ("circle 0, 0", 1);
    gchar  *text;

    PN_CHECK (pn_node_get_has_error (self));

    /* The moment a valid program is set the state goes: it is transient
     * and never serialised (80.10h). */
    g_object_set (self, "program", "circle 50, 50, 10", NULL);

    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");
    PN_CHECK_FALSE  (pn_node_get_has_error (self));

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "circle 50.00 50.00 10.00\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_a_value_problem_does_not_redden_the_node (void)
{
    PnNode *self = node ("circle 50, 50, 10 / value1\nrect 0, 0, 10, 10", 1);
    gchar  *text = node_dump (self);

    /* Class (b): a knob winding through zero produces this and the next
     * message cures it, so the statement is skipped, the rest of the
     * figure draws, and the node stays its own colour -- a figure that
     * flashes red teaches the user to ignore the red (80.10b). */
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");
    PN_CHECK_FALSE  (pn_node_get_has_error (self));
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "# skip 1 non-finite\n"
                     "rect 0.00 90.00 10.00 10.00\n");

    g_free (text);
    g_object_unref (self);
}

static void
test_a_vector_input_reddens_the_node (void)
{
    PnNode     *self    = node ("line 0, 0, value1, 0", 1);
    PnMessage  *message = pn_message_new (NULL, NULL);
    gdouble     numbers[3] = { 1.0, 2.0, 3.0 };
    PnVector   *vec     = pn_vector_new_copy (numbers, 3);
    gchar      *text;

    /* Class (c): someone wired a vector source into a figure that
     * cannot animate yet, and no amount of winding a knob will cure it
     * -- so it is reported like a program error (80.10c). */
    pn_message_set_vector (message, "value", vec);
    pn_node_receive_message_on_input (self, message, 0);
    g_object_unref (vec);
    g_object_unref (message);

    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==,
                     "line 1, column 12: vector argument;"
                     " animation is TODO 80.16");
    PN_CHECK (pn_node_get_has_error (self));

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, "");

    g_free (text);
    g_object_unref (self);
}

static void
test_the_error_property_reads_back (void)
{
    PnNode *self = node ("circle 0, 0", 1);
    gchar  *text = NULL;

    /* Read-only, so a headless test and the D-Bus automation can assert
     * the exact text without a screenshot -- and pn-flow.c will not save
     * it into the worksheet (80.10f). */
    g_object_get (self, "error", &text, NULL);
    PN_CHECK_CMPSTR (text, ==,
                     "line 1, column 1: circle takes 3 arguments, not 2");

    g_free (text);
    g_object_unref (self);
}

static void
test_the_client_area_is_the_body (void)
{
    PnNode  *self = node (NULL, 1);
    double   x = -1, y = -1, w = -1, h = -1;
    double   width = 0, height = 0;

    pn_node_get_size (self, &width, &height);
    PN_CHECK_NEAR (width,  PN_FIGURE_WIDTH, 1e-9);
    PN_CHECK_NEAR (height, PN_FIGURE_TOTAL_HEIGHT, 1e-9);

    /* No override: PnNode's geometric default already reports the
     * rectangle under the header, which is what the worksheet hands the
     * painter (80.24.4). */
    PN_CHECK      (pn_node_get_client_area (self, &x, &y, &w, &h));
    PN_CHECK_NEAR (w, PN_FIGURE_WIDTH, 1e-9);
    PN_CHECK_NEAR (h, PN_FIGURE_CLIENT_HEIGHT, 1e-9);

    g_object_unref (self);
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
    pn_test_add ("verb_every_one",      test_every_verb);
    pn_test_add ("verb_assignments",    test_assignments_pass_through);
    pn_test_add ("verb_unknown",        test_unknown_verb);
    pn_test_add ("verb_arity",          test_wrong_arity);
    pn_test_add ("verb_pairs",          test_pairs);
    pn_test_add ("verb_arg_kinds",      test_argument_kinds);
    pn_test_add ("verb_colour_forms",   test_colour_has_two_spellings);
    pn_test_add ("verb_colour_kinds",   test_colour_argument_kinds);
    pn_test_add ("verb_keeps_going",    test_check_returns_and_keeps_going);
    pn_test_add ("lit_colours",         test_colour_literals);
    pn_test_add ("lit_bad_colour",      test_bad_colour_literal);
    pn_test_add ("lit_dash",            test_dash_styles);
    pn_test_add ("lit_bad_dash",        test_bad_dash_style);
    pn_test_add ("lit_align",           test_alignment_words);
    pn_test_add ("lit_align_axes",      test_alignment_words_are_per_axis);
    pn_test_add ("lit_format",          test_format_conversions);
    pn_test_add ("lit_format_unsafe",   test_format_rejects_unsafe);
    pn_test_add ("lit_format_count",    test_format_value_count);
    pn_test_add ("lit_keeps_going",     test_literals_keep_going);
    pn_test_add ("expr_folding",        test_constant_folding);
    pn_test_add ("expr_variables",      test_variable_arguments_keep_their_tree);
    pn_test_add ("expr_constants",      test_constants_fold_too);
    pn_test_add ("expr_assignment",     test_assignment_parses_whole);
    pn_test_add ("expr_free_names",     test_free_names);
    pn_test_add ("expr_parse_error",    test_expression_parse_error);
    pn_test_add ("expr_no_position",    test_expression_error_without_a_position);
    pn_test_add ("expr_bad_function",   test_unknown_function_is_a_parse_error);
    pn_test_add ("expr_atan2",          test_atan2_is_a_function);
    pn_test_add ("expr_three_args",     test_a_three_argument_call_walks);
    pn_test_add ("expr_non_finite",     test_non_finite_constant_is_not_an_error);
    pn_test_add ("report_none",         test_report_nothing_wrong);
    pn_test_add ("report_one",          test_report_one_error);
    pn_test_add ("report_count",        test_report_counts_and_takes_the_earliest);
    pn_test_add ("report_leftmost",     test_report_earliest_on_a_line);
    pn_test_add ("report_specimen",     test_the_specimen_parses);
    pn_test_add ("back_empty",          test_an_empty_program_is_a_blank_figure);
    pn_test_add ("back_state_verbs",    test_the_state_verbs);
    pn_test_add ("back_geometry",       test_the_geometry_verbs);
    pn_test_add ("back_y_up",           test_y_points_up);
    pn_test_add ("back_rect_corner",    test_rect_takes_its_lower_left_corner);
    pn_test_add ("back_arc_sweep",      test_an_arc_sweeps_the_way_it_was_written);
    pn_test_add ("back_arc_reversed",   test_a_reversed_axis_turns_an_arc_around);
    pn_test_add ("back_letterbox",      test_the_window_is_letterboxed);
    pn_test_add ("back_reversed",       test_reversed_bounds_flip_an_axis);
    pn_test_add ("back_view_degenerate", test_a_degenerate_view_is_skipped);
    pn_test_add ("back_pen_resets",     test_the_pen_resets_every_frame);
    pn_test_add ("back_zero_fill",      test_an_unwired_figure_still_draws);
    pn_test_add ("back_constants",      test_the_constants_are_bound);
    pn_test_add ("back_snapshot",       test_the_snapshot_survives_a_repaint);
    pn_test_add ("back_binding_order",  test_an_input_beats_the_zero_fill);
    pn_test_add ("back_stretch",        test_stretch_fills_the_rectangle);
    pn_test_add ("back_hairline",       test_width_zero_is_a_hairline);
    pn_test_add ("back_skip_non_finite", test_a_non_finite_value_skips_its_statement);
    pn_test_add ("back_skip_degenerate", test_a_zero_radius_skips_its_statement);
    pn_test_add ("back_vector",         test_a_vector_argument_is_an_error);
    pn_test_add ("film_still",          test_a_still_figure_is_one_frame);
    pn_test_add ("film_shortest",       test_the_shortest_vector_sets_the_frame_count);
    pn_test_add ("film_unread",         test_an_unread_vector_does_not_count);
    pn_test_add ("film_explicit",       test_an_explicit_count_joins_the_minimum);
    pn_test_add ("film_empty",          test_an_empty_vector_leaves_no_frame);
    pn_test_add ("back_text_format",    test_the_text_format_is_filled_in);
    pn_test_add ("back_locale",         test_the_dump_is_locale_independent);
    pn_test_add ("back_specimen",       test_the_specimen_draws);
    pn_test_add ("back_no_room",        test_a_frame_needs_room);
    pn_test_add ("block_runs_n_times",  test_a_block_runs_its_body_n_times);
    pn_test_add ("block_no_op",         test_a_block_leaves_no_operation_of_its_own);
    pn_test_add ("block_index_binds",   test_the_index_beats_the_zero_fill);
    pn_test_add ("block_state_carries", test_pen_state_and_assignments_carry_across_passes);
    pn_test_add ("block_count_value",   test_a_count_is_a_value_not_a_program);
    pn_test_add ("block_cap",           test_the_cap_is_the_last_count_that_runs);
    pn_test_add ("block_grid",          test_a_grid_is_one_loop);
    pn_test_add ("block_folding",       test_a_constant_inside_a_block_still_folds);
    pn_test_add ("block_unclosed",      test_an_unclosed_block_is_a_parse_error);
    pn_test_add ("block_stray_end",     test_an_end_without_a_repeat_is_a_parse_error);
    pn_test_add ("block_no_nesting",    test_blocks_do_not_nest);
    pn_test_add ("block_error_draws_nothing", test_a_structural_error_draws_nothing);
    pn_test_add ("node_is_a_sink",      test_the_node_is_a_sink);
    pn_test_add ("node_fresh_draws",    test_a_fresh_node_draws);
    pn_test_add ("node_input_variable", test_an_input_becomes_a_variable);
    pn_test_add ("node_renamed_input",  test_a_renamed_input_renames_the_variable);
    pn_test_add ("node_latching",       test_the_inputs_are_latched);
    pn_test_add ("node_sibling_suffix", test_a_sibling_member_takes_the_input_number);
    pn_test_add ("node_no_strings",     test_a_string_member_is_not_bound);
    pn_test_add ("node_snapshot",       test_the_node_snapshot_survives_a_repaint);
    pn_test_add ("node_program_error",  test_a_program_error_reaches_the_property);
    pn_test_add ("node_error_clears",   test_the_error_clears_on_a_good_program);
    pn_test_add ("node_value_problem",  test_a_value_problem_does_not_redden_the_node);
    pn_test_add ("node_vector_input",   test_a_vector_input_reddens_the_node);
    pn_test_add ("node_error_property", test_the_error_property_reads_back);
    pn_test_add ("node_client_area",    test_the_client_area_is_the_body);
    return pn_test_run ();
}
