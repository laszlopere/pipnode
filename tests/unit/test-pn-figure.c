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

/* ------------------------------------------------------------------ */
/*  The verb table                                                     */
/* ------------------------------------------------------------------ */

/* Scan, split and check, which is the whole front end so far. */
static Split
checked (const gchar *program)
{
    Split result = split (program);

    pn_figure_check_verbs (result.statements, result.errors);
    return result;
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
test_atan2_is_not_a_function_yet (void)
{
    /* The gap #81 exists to close.  Our splitter keeps `atan2(1, 2)`
     * whole, because its comma is at paren depth 1; the calculator's
     * lexer then meets a comma it has no token for.  Loud, and pointed
     * straight at the comma, which is the thing that cannot be there
     * (80.3e). */
    Split          s = parsed ("circle 0, 0, atan2(1, 2)");
    PnFigureError *error;

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    error = g_ptr_array_index (s.errors, 0);
    PN_CHECK_CMPSTR (error->message, ==, "unexpected character ','");
    PN_CHECK_CMPINT (error->column, ==, 21);

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
    pn_test_add ("expr_no_atan2",       test_atan2_is_not_a_function_yet);
    pn_test_add ("expr_non_finite",     test_non_finite_constant_is_not_an_error);
    pn_test_add ("report_none",         test_report_nothing_wrong);
    pn_test_add ("report_one",          test_report_one_error);
    pn_test_add ("report_count",        test_report_counts_and_takes_the_earliest);
    pn_test_add ("report_leftmost",     test_report_earliest_on_a_line);
    pn_test_add ("report_specimen",     test_the_specimen_parses);
    return pn_test_run ();
}
