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

/* Unit tests for PnFigure: the front end (the line scanner, the
 * statement splitter, the verb table, literals and expressions), the
 * resolver and the display list it dumps, the blocks (repeat, if, with,
 * origin, def), the drawing verbs, animation, and the node itself. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-figure.h"
#include "pn-flow.h"

#include <math.h>
#include <locale.h>
#include <string.h>

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
    /* Depth 0 only: the commas of a call such as max(1, 2) belong to
     * the call, not to the verb (80.2 rule 3). */
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
    /* One of every pen and drawing verb, at an arity the table
     * accepts; the block keywords have tests of their own.  What the
     * literals MEAN is 80.22.4's business, so "black" and "dot" are just
     * strings here. */
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
                       "color 1, 0, 0, 0.5\n"      /* 22 */
                       "arrowhead 3, 2\n"          /* 23 */
                       "arrow 1, 2, 3, 4\n"        /* 24 */
                       "head 1, 2, 3, 4\n"         /* 25 */
                       "hatch 1, 2, 3, 4, 1\n"     /* 26 */
                       "hatch 1, 2, 3, 4, 1, -1\n" /* 27 */
                       "dimension 1, 2, 3, 4\n"    /* 28 */
                       "dimension 1, 2, 3, 4, 1\n" /* 29 */
                       "angle 30\n"                /* 30 */
                       "curve 0,0, 1,1, 2,2, 3,3\n" /* 31 */
                       "anglemark 0, 0, 5, 0, 90\n" /* 32 */
                       "anglemark 0, 0, 5, 0, 90, \"%.0f\", a\n" /* 33 */
                       "axes 0, 0, 10, 10\n"        /* 34 */
                       "axes 0, 0, 10, 10, \"x\", \"y\""); /* 35 */

    PN_CHECK_CMPSTR (error_text (&s, 0), ==, NULL);
    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (s.statements->len, ==, 36);

    PN_CHECK_CMPINT (statement (&s, 0)->verb,  ==, PN_FIGURE_VERB_VIEW);
    PN_CHECK_CMPINT (statement (&s, 3)->verb,  ==, PN_FIGURE_VERB_NOFILL);
    PN_CHECK_CMPINT (statement (&s, 12)->verb, ==, PN_FIGURE_VERB_LINETO);
    PN_CHECK_CMPINT (statement (&s, 19)->verb, ==, PN_FIGURE_VERB_POLY);
    PN_CHECK_CMPINT (statement (&s, 21)->verb, ==, PN_FIGURE_VERB_TEXT);
    PN_CHECK_CMPINT (statement (&s, 22)->verb, ==, PN_FIGURE_VERB_COLOR);
    PN_CHECK_CMPINT (statement (&s, 23)->verb, ==, PN_FIGURE_VERB_ARROWHEAD);
    PN_CHECK_CMPINT (statement (&s, 24)->verb, ==, PN_FIGURE_VERB_ARROW);
    PN_CHECK_CMPINT (statement (&s, 25)->verb, ==, PN_FIGURE_VERB_HEAD);
    PN_CHECK_CMPINT (statement (&s, 27)->verb, ==, PN_FIGURE_VERB_HATCH);
    PN_CHECK_CMPINT (statement (&s, 29)->verb, ==, PN_FIGURE_VERB_DIMENSION);
    PN_CHECK_CMPINT (statement (&s, 30)->verb, ==, PN_FIGURE_VERB_ANGLE);
    PN_CHECK_CMPINT (statement (&s, 31)->verb, ==, PN_FIGURE_VERB_CURVE);
    PN_CHECK_CMPINT (statement (&s, 33)->verb, ==, PN_FIGURE_VERB_ANGLEMARK);
    PN_CHECK_CMPINT (statement (&s, 35)->verb, ==, PN_FIGURE_VERB_AXES);

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
                        "text 0, 0, \"%.1f deg\", degrees(a)\n"
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
                           "text 0, 0, \"%.1f\", degrees(a)");
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
figure_dump_film (Figure             *self,
                  PnFigureSnapshot   *snapshot,
                  const PnFigureFilm *film,
                  gdouble             w,
                  gdouble             h,
                  gboolean            stretch,
                  gchar             **out_error)
{
    GPtrArray *ops  = pn_figure_resolve (self->parse.statements, self->names,
                                         snapshot, film, 0, 0, w, h, stretch,
                                         out_error);
    gchar     *text = pn_figure_display_to_string (ops);

    g_ptr_array_unref (ops);
    return text;
}

/* Frame @index of a film whose length comes from the data, looping. */
static gchar *
figure_dump_frame (Figure           *self,
                   PnFigureSnapshot *snapshot,
                   guint             index,
                   gdouble           w,
                   gdouble           h,
                   gboolean          stretch,
                   gchar           **out_error)
{
    PnFigureFilm film = { index, 0, PN_FIGURE_PLAY_LOOP };

    return figure_dump_film (self, snapshot, &film, w, h, stretch, out_error);
}

/* Frame 0, which for a figure with no vector in it is the only one. */
static gchar *
figure_dump (Figure           *self,
             PnFigureSnapshot *snapshot,
             gdouble           w,
             gdouble           h,
             gboolean          stretch,
             gchar           **out_error)
{
    return figure_dump_frame (self, snapshot, 0, w, h, stretch, out_error);
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

/* Binds @name to the @n numbers in @numbers. */
static void
snapshot_vector (PnFigureSnapshot *snapshot,
                 const gchar      *name,
                 const gdouble    *numbers,
                 gsize             n)
{
    PnVector *vec = pn_vector_new_copy (numbers, n);

    pn_figure_snapshot_set_vector (snapshot, name, vec);
    g_object_unref (vec);
}

static void
test_a_vector_argument_is_a_film (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, v, 0\ncircle 5, 5, 5");
    const gdouble     numbers[3] = { 10.0, 20.0, 30.0 };
    gchar            *error = NULL;
    gchar            *text;

    snapshot_vector (snapshot, "v", numbers, 3);

    /* What used to be 80.10(c)'s error is the frame indexer (80.16d):
     * frame i takes element i of the vector, and the scalar circle is
     * the same in every frame. */
    text = figure_dump_frame (&f, snapshot, 0, 100, 100, FALSE, &error);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 10.00 100.00\n"
                     "circle 5.00 95.00 5.00\n");
    PN_CHECK_CMPSTR (error, ==, NULL);
    g_free (text);

    text = figure_dump_frame (&f, snapshot, 2, 100, 100, FALSE, &error);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 30.00 100.00\n"
                     "circle 5.00 95.00 5.00\n");
    PN_CHECK_CMPSTR (error, ==, NULL);
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_vector_survives_assignment_and_arithmetic (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("x = 2 * v + 1\nline 0, 0, x, 0");
    const gdouble     numbers[2] = { 10.0, 20.0 };
    gchar            *text;

    snapshot_vector (snapshot, "v", numbers, 2);

    /* Evaluated once, indexed where it lands (80.16a): the assignment
     * binds a whole vector, and only the argument picks an element. */
    text = figure_dump_frame (&f, snapshot, 1, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 41.00 100.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_vector_pen_state_animates (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("width w\nline 0, 0, 10, 0");
    const gdouble     numbers[2] = { 2.0, 4.0 };
    gchar            *text;

    snapshot_vector (snapshot, "w", numbers, 2);

    /* 80.16(c): pen state goes through the same indexing, no special
     * case. */
    text = figure_dump_frame (&f, snapshot, 1, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 4.00\n"
                     "line 0.00 100.00 10.00 100.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_vector_repeat_count_animates (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("repeat n\n"
                                         "line i * 10, 0, i * 10, 10\n"
                                         "end");
    const gdouble     numbers[2] = { 1.0, 2.0 };
    gchar            *text;

    snapshot_vector (snapshot, "n", numbers, 2);

    /* Inside or around a block a vector is still a frame, never an
     * iteration (80.18b): frame 1 runs the block twice because the
     * count's element 1 is 2. */
    text = figure_dump_frame (&f, snapshot, 0, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 0.00 90.00\n");
    g_free (text);

    text = figure_dump_frame (&f, snapshot, 1, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 0.00 90.00\n"
                     "line 10.00 100.00 10.00 90.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_frame_past_the_end_is_an_error (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, v, 0\ncircle 5, 5, 5");
    const gdouble     numbers[3] = { 10.0, 20.0, 30.0 };
    gchar            *error = NULL;
    gchar            *text;

    snapshot_vector (snapshot, "v", numbers, 3);

    /* A caller that miscounted the film.  Not clamped: a quiet clamp
     * would hide the miscount behind a plausible picture.  Nothing is
     * drawn, not even the circle after it (80.10d). */
    text = figure_dump_frame (&f, snapshot, 3, 100, 100, FALSE, &error);
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (error, ==,
                     "line 1, column 12: frame 3 is past the end of"
                     " a 3-element vector");

    g_free (error);
    g_free (text);
    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_an_empty_vector_argument_is_an_error (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("line 0, 0, v, 0");
    gchar            *error = NULL;
    gchar            *text;

    snapshot_vector (snapshot, "v", NULL, 0);

    /* The film has no frame at all (82.1e); the argument it lands on
     * is where the message can say which input it was. */
    text = figure_dump_frame (&f, snapshot, 0, 100, 100, FALSE, &error);
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (error, ==,
                     "line 1, column 12: empty vector argument;"
                     " nothing to draw");

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

/* Frame @index of an explicit @frames-long film played in @mode, in the
 * 100x100 rectangle. */
static gchar *
dump_film (const gchar      *program,
           PnFigureSnapshot *snapshot,
           guint             index,
           guint             frames,
           PnFigurePlayMode  mode)
{
    Figure        f    = figure (program);
    PnFigureFilm  film = { index, frames, mode };
    gchar        *text = figure_dump_film (&f, snapshot, &film, 100, 100,
                                           FALSE, NULL);

    figure_free (&f);
    return text;
}

static void
test_frame_and_t_are_bound_as_the_film (void)
{
    gchar *text;

    /* 80.17(d): `frame` counts the frames and `t` runs from 0 towards
     * 1; a loop of four steps by 1/4, so frame 1 is t = 0.25 -- and a
     * figure with nothing wired animates at all (80.17e). */
    text = dump_film ("line 0, 0, t * 100, frame", NULL, 1, 4,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 25.00 99.00\n");
    g_free (text);

    /* The loop's last frame stops short of 1: the frame after it is
     * t = 1 = t = 0 again, so sin(2 pi t) never shows its seam twice. */
    text = dump_film ("line 0, 0, t * 100, frame", NULL, 3, 4,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 75.00 97.00\n");
    g_free (text);
}

static void
test_t_ends_on_one_when_the_film_ends (void)
{
    gchar *text;

    /* Once holds its last frame and ping-pong turns on it, so both end
     * exactly at t = 1: the held or turned pose is the true end (82.5). */
    text = dump_film ("line 0, 0, t * 100, 0", NULL, 3, 4,
                      PN_FIGURE_PLAY_ONCE);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 100.00 100.00\n");
    g_free (text);

    text = dump_film ("line 0, 0, t * 100, 0", NULL, 1, 4,
                      PN_FIGURE_PLAY_PING_PONG);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 33.33 100.00\n");
    g_free (text);
}

static void
test_t_follows_a_film_from_the_data (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    gchar            *text;

    /* No `frames`: the film is the three-element input, and `t` is
     * built to its length. */
    snapshot_ramp (snapshot, "v", 3);
    text = dump_film ("line 0, v, t * 90, 0", snapshot, 2, 0,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 98.00 60.00 100.00\n");
    g_free (text);

    pn_figure_snapshot_free (snapshot);
}

static void
test_a_still_reads_t_as_zero (void)
{
    gchar *text;

    /* One frame: `t` and `frame` are plain zeros, not one-element
     * vectors, and a still is still a still. */
    text = dump_film ("line 0, 0, 10 + t, 10 + frame", NULL, 0, 0,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 10.00 90.00\n");
    g_free (text);
}

static void
test_an_input_named_t_wins (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    gchar            *text;

    /* An input is a wire somebody named: it beats the animation
     * variable exactly as it beats the zero-fill (80.3c). */
    pn_figure_snapshot_set (snapshot, "t", 0.5);
    text = dump_film ("line 0, 0, t * 100, frame", snapshot, 2, 4,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 50.00 98.00\n");
    g_free (text);

    pn_figure_snapshot_free (snapshot);
}

static void
test_an_assignment_to_t_still_wins (void)
{
    gchar *text;

    /* An assignment runs after every binding, so it overrides `t` like
     * any other name. */
    text = dump_film ("t = 0.1\nline 0, 0, t * 100, 0", NULL, 2, 4,
                      PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 10.00 100.00\n");
    g_free (text);
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
    return pn_figure_dump (PN_FIGURE (self), 0, 0, 0, 100, 100);
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
    /* The arithmetic 85.12 stands on, and the reason a repeat inside a
     * repeat is not missed (#86.2): i % cols and floor(i / cols) turn
     * one index into a column and a row. */
    gchar *text = dump100 ("cols = 3\n"
                           "repeat 6\n"
                           "    cx = i % cols\n"
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
test_an_end_without_a_block_is_a_parse_error (void)
{
    Split s = checked ("line 0, 0, 10, 10\nend");

    PN_CHECK_CMPINT (s.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "end without a repeat, an if, a with, "
                     "an origin or a def");
    PN_CHECK_CMPINT (error_line (&s, 0), ==, 2);
    split_free (&s);
}

static void
test_a_repeat_does_not_nest_in_a_repeat (void)
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
    PN_CHECK_CMPSTR (error_text (&s, 1), ==,
                     "end without a repeat, an if, a with, "
                     "an origin or a def");
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

    text = pn_figure_dump (figure, 0, 0, 0, 100, 100);
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (pn_figure_get_error (figure), ==,
                     "line 2, column 1: repeat without an end");

    g_free (text);
    g_object_unref (figure);
}

/* ------------------------------------------------------------------ */
/*  The if block (TODO #91.1)                                          */
/* ------------------------------------------------------------------ */

static void
test_an_if_runs_its_body_only_when_true (void)
{
    /* The replacement for 80.10(b)'s division idiom, `point 10 / c, 10`:
     * the statement is simply not reached, and there is no skip marker
     * for it, because nothing went wrong. */
    gchar *text = dump100 ("if 1\n"
                           "    point 10, 10\n"
                           "end\n"
                           "if 0\n"
                           "    point 20, 20\n"
                           "end\n"
                           "if 2 > 1\n"
                           "    point 30, 30\n"
                           "end\n"
                           "point 40, 40");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 10.00 90.00 1.00\n"
                     "point 30.00 70.00 1.00\n"
                     "point 40.00 60.00 1.00\n");
    g_free (text);
}

static void
test_an_if_leaves_no_operation_of_its_own (void)
{
    /* Control flow is not ink (#86.7), for `if` as for `repeat`. */
    gchar *with    = dump100 ("if 1\nline 0, 0, 10, 10\nelse\npoint 0, 0\n"
                              "end");
    gchar *without = dump100 ("line 0, 0, 10, 10");

    PN_CHECK_CMPSTR (with, ==, without);
    g_free (with);
    g_free (without);
}

/* The chain ray-optics needs for its five regimes: the FIRST clause
 * whose condition holds runs, and only that one. */
static gchar *
regime (gint tenths)
{
    gchar *program = g_strdup_printf (
            "r = %d / 10\n"
            "if r < 1\n"
            "    point 10, 10\n"
            "elseif r < 2\n"
            "    point 20, 20\n"
            "elseif r < 3\n"
            "    point 30, 30\n"
            "else\n"
            "    point 40, 40\n"
            "end\n"
            "point 50, 50", tenths);
    gchar *text    = dump100 (program);

    g_free (program);
    return text;
}

static void
test_the_first_true_clause_wins (void)
{
    gchar *a = regime (5);
    gchar *b = regime (15);    /* r < 3 holds as well: b, not c */
    gchar *c = regime (25);
    gchar *d = regime (90);

    PN_CHECK_CMPSTR (a, ==, HEAD_100 "point 10.00 90.00 1.00\n"
                                     "point 50.00 50.00 1.00\n");
    PN_CHECK_CMPSTR (b, ==, HEAD_100 "point 20.00 80.00 1.00\n"
                                     "point 50.00 50.00 1.00\n");
    PN_CHECK_CMPSTR (c, ==, HEAD_100 "point 30.00 70.00 1.00\n"
                                     "point 50.00 50.00 1.00\n");
    PN_CHECK_CMPSTR (d, ==, HEAD_100 "point 40.00 60.00 1.00\n"
                                     "point 50.00 50.00 1.00\n");
    g_free (a);
    g_free (b);
    g_free (c);
    g_free (d);
}

static void
test_an_if_is_not_a_scope (void)
{
    /* Like `repeat` (86.6), the block is not a scope: an assignment or a
     * pen change inside a branch that ran stands after the `end`, and
     * one inside a branch that did not run never happened. */
    gchar *text = dump100 ("x = 1\n"
                           "if 1\n"
                           "    x = 5\n"
                           "    width 3\n"
                           "else\n"
                           "    x = 7\n"
                           "    width 9\n"
                           "end\n"
                           "point x, 0");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 3.00\n"
                     "point 5.00 100.00 3.00\n");
    g_free (text);
}

static void
test_a_condition_is_a_value_not_a_program (void)
{
    /* 80.10(b) once more: a condition that is not a number skips the
     * whole chain and says so at the clause that could not decide --
     * not even the `else` runs, because the test was not false.  The
     * figure after it still draws. */
    gchar *nan  = dump100 ("if 0 / 0\n"
                           "    point 10, 10\n"
                           "else\n"
                           "    point 20, 20\n"
                           "end\n"
                           "point 50, 50");
    gchar *late = dump100 ("if 0\n"
                           "    point 10, 10\n"
                           "elseif 1 / 0\n"
                           "    point 20, 20\n"
                           "else\n"
                           "    point 30, 30\n"
                           "end");

    PN_CHECK_CMPSTR (nan, ==, HEAD_100
                     "# skip 1 non-finite\n"
                     "point 50.00 50.00 1.00\n");
    PN_CHECK_CMPSTR (late, ==, HEAD_100 "# skip 3 non-finite\n");
    g_free (nan);
    g_free (late);
}

static void
test_an_if_nests_inside_anything (void)
{
    /* An `if` inside a `repeat` picks per pass; a `repeat` inside an
     * `if` runs or does not; an `if` inside an `if` closes on its own
     * `end`, and the outer chain's `else` is still the outer one's. */
    gchar *in_loop  = dump100 ("repeat 4\n"
                               "    if i % 2\n"
                               "        point 10 * i, 0\n"
                               "    else\n"
                               "        point 10 * i, 50\n"
                               "    end\n"
                               "end");
    gchar *loop_in  = dump100 ("if 1\n"
                               "    repeat 2\n"
                               "        point i, 0\n"
                               "    end\n"
                               "end\n"
                               "if 0\n"
                               "    repeat 2\n"
                               "        point i, 50\n"
                               "    end\n"
                               "end");
    gchar *if_in_if = dump100 ("if 1\n"
                               "    if 0\n"
                               "        point 10, 10\n"
                               "    else\n"
                               "        point 20, 20\n"
                               "    end\n"
                               "    point 30, 30\n"
                               "else\n"
                               "    point 40, 40\n"
                               "end\n"
                               "point 50, 50");

    PN_CHECK_CMPSTR (in_loop, ==, HEAD_100
                     "point 0.00 50.00 1.00\n"
                     "point 10.00 100.00 1.00\n"
                     "point 20.00 50.00 1.00\n"
                     "point 30.00 100.00 1.00\n");
    PN_CHECK_CMPSTR (loop_in, ==, HEAD_100
                     "point 0.00 100.00 1.00\n"
                     "point 1.00 100.00 1.00\n");
    PN_CHECK_CMPSTR (if_in_if, ==, HEAD_100
                     "point 20.00 80.00 1.00\n"
                     "point 30.00 70.00 1.00\n"
                     "point 50.00 50.00 1.00\n");
    g_free (in_loop);
    g_free (loop_in);
    g_free (if_in_if);
}

static void
test_a_vector_condition_animates (void)
{
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("if c\n"
                                         "    point 10, 10\n"
                                         "else\n"
                                         "    point 20, 20\n"
                                         "end");
    const gdouble     numbers[2] = { 0.0, 1.0 };
    gchar            *text;

    snapshot_vector (snapshot, "c", numbers, 2);

    text = figure_dump_frame (&f, snapshot, 0, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "point 20.00 80.00 1.00\n");
    g_free (text);

    text = figure_dump_frame (&f, snapshot, 1, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "point 10.00 90.00 1.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_comparison_of_a_film_chooses_per_frame (void)
{
    /* A comparison of a vector is elementwise in a figure, unlike in a
     * Calculator: `frame > 1` is 0, 0, 1, 1 and the if follows it --
     * and so does a coordinate made of one. */
    const gchar *program = "if frame > 1\n"
                           "    point 10, 10\n"
                           "else\n"
                           "    point 20, 20\n"
                           "end\n"
                           "line 0, 0, 0, 10 * (frame >= 2)";
    gchar       *text;

    text = dump_film (program, NULL, 1, 4, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 20.00 80.00 1.00\n"
                     "line 0.00 100.00 0.00 100.00\n");
    g_free (text);

    text = dump_film (program, NULL, 2, 4, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 10.00 90.00 1.00\n"
                     "line 0.00 100.00 0.00 90.00\n");
    g_free (text);
}

static void
test_a_condition_folds_like_any_argument (void)
{
    Split s = parsed ("if 2 > 1\n"
                      "    point 0, 0\n"
                      "elseif v\n"
                      "end");

    PN_CHECK_CMPINT (s.errors->len, ==, 0);
    PN_CHECK_CMPINT (statement (&s, 0)->verb, ==, PN_FIGURE_VERB_IF);
    PN_CHECK_CMPINT (statement (&s, 2)->verb, ==, PN_FIGURE_VERB_ELSEIF);
    PN_CHECK (arg (statement (&s, 0), 0)->folded);
    PN_CHECK_NEAR (arg (statement (&s, 0), 0)->value, 1.0, 1e-12);
    PN_CHECK (!arg (statement (&s, 2), 0)->folded);
    split_free (&s);
}

static void
test_the_if_verbs_have_arities (void)
{
    Split none = checked ("if\nend");
    Split more = checked ("if 1\nelse 2\nend");

    PN_CHECK_CMPINT (none.errors->len, >=, 1);
    PN_CHECK_CMPSTR (error_text (&none, 0), ==,
                     "if takes 1 argument, not 0");
    PN_CHECK_CMPINT (more.errors->len, >=, 1);
    PN_CHECK_CMPSTR (error_text (&more, 0), ==,
                     "else takes 0 arguments, not 1");
    split_free (&none);
    split_free (&more);
}

static void
test_misplaced_clauses_are_parse_errors (void)
{
    /* Every mistake on its own line, and the scan keeps going.  The
     * `else` on line 7 is inside a `repeat` inside an `if`: only the
     * innermost block can own it, so it is refused rather than quietly
     * closing the loop. */
    Split s = checked ("else\n"                /* 1: no if           */
                       "elseif 1\n"            /* 2: no if           */
                       "if 1\n"
                       "else\n"
                       "else\n"                /* 5: after else      */
                       "elseif 1\n"            /* 6: after else      */
                       "    repeat 2\n"
                       "    else\n"            /* 8: the loop's      */
                       "    end\n"
                       "end\n"
                       "if 1");                /* 11: no end         */

    PN_CHECK_CMPINT (s.errors->len, ==, 6);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "else without an if");
    PN_CHECK_CMPINT (error_line (&s, 0), ==, 1);
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "elseif without an if");
    PN_CHECK_CMPINT (error_line (&s, 1), ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "else after else");
    PN_CHECK_CMPINT (error_line (&s, 2), ==, 5);
    PN_CHECK_CMPSTR (error_text (&s, 3), ==, "elseif after else");
    PN_CHECK_CMPINT (error_line (&s, 3), ==, 6);
    PN_CHECK_CMPSTR (error_text (&s, 4), ==, "else without an if");
    PN_CHECK_CMPINT (error_line (&s, 4), ==, 8);
    PN_CHECK_CMPSTR (error_text (&s, 5), ==, "if without an end");
    PN_CHECK_CMPINT (error_line (&s, 5), ==, 11);
    split_free (&s);
}

static void
test_an_if_does_not_launder_a_nested_repeat (void)
{
    /* 86.2 is about the index: a `repeat` inside an `if` inside a
     * `repeat` would still rebind the outer loop's `i`.  A `repeat`
     * inside an `if` on its own is fine. */
    Split bad  = checked ("repeat 2\n"
                          "    if 1\n"
                          "        repeat 3\n"
                          "        end\n"
                          "    end\n"
                          "end");
    Split fine = checked ("if 1\n"
                          "    repeat 3\n"
                          "    end\n"
                          "end\n"
                          "repeat 2\n"
                          "end");

    PN_CHECK_CMPINT (bad.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&bad, 0), ==,
                     "repeat cannot be nested inside another repeat");
    PN_CHECK_CMPINT (error_line (&bad, 0), ==, 3);
    PN_CHECK_CMPINT (fine.errors->len, ==, 0);
    split_free (&bad);
    split_free (&fine);
}

static void
test_an_unchecked_program_cannot_run_away (void)
{
    /* The resolver never trusts the block check (block_links): stray
     * clauses and missing ends are walked without reading past the
     * program or looping.  What it draws is not the point -- that it
     * returns is. */
    const gchar *programs[] = {
        "end\nelse\nelseif 1\npoint 0, 0",
        "if 0\npoint 0, 0\nelse",
        "if 1\nrepeat 2\nelse\npoint i, 0\nend",
        "repeat 2\nif 0\nend\npoint i, 0",
        "else\nif 1\nend\nend\nend",
        "end\nwith color \"red\"\nelse\npoint 0, 0\nend\nend",
        "with width 2\nrepeat 2\nend\npoint i, 0",
        "end\norigin 1, 2\nelse\npoint 0, 0\nend\nend",
        "origin 0 / 0, 0\nwith width 2\npoint 0, 0",
        "origin 1, 2, 30\nwith width 2\nend\nend\nend\npoint 0, 0",
        "def a x\npoint x, 0\na 1\nend\na 2",
        "a 1\ndef a x\nend\nend\na 1, 2\na",
        "def a\nwith width 2\nend\nend\ndef b\na\nend\nb\nend\nb",
        "if 1\ndef a\nelse\npoint 0, 0\nend\na",
    };
    guint n;

    for (n = 0; n < G_N_ELEMENTS (programs); n++)
    {
        Split      s   = split (programs[n]);
        GPtrArray *ops;

        pn_figure_check_verbs (s.statements, NULL);
        pn_figure_parse_literals (s.statements, NULL);
        pn_figure_parse_expressions (s.statements, NULL);
        ops = pn_figure_resolve (s.statements, NULL, NULL, NULL,
                                 0, 0, 100, 100, FALSE, NULL);
        PN_CHECK (ops != NULL);
        g_ptr_array_unref (ops);
        split_free (&s);
    }
}

/* ------------------------------------------------------------------ */
/*  Scoped pen settings: with (TODO #91.2)                              */
/* ------------------------------------------------------------------ */

/* The column error @n was reported at, or -1. */
static gint
error_column (Split *self, guint n)
{
    return n < self->errors->len
           ? ((PnFigureError *) g_ptr_array_index (self->errors, n))->column
           : -1;
}

static void
test_a_trailing_with_scopes_one_statement (void)
{
    /* The fill ... nofill bracket, gone: the setting holds for the one
     * statement and the pen is given back straight after it. */
    gchar *text = dump100 ("circle 50, 50, 10 with fill \"red\"\n"
                           "circle 20, 20, 5");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "fill rgb(255,0,0)\n"
                     "circle 50.00 50.00 10.00\n"
                     "nofill\n"
                     "circle 20.00 80.00 5.00\n");
    g_free (text);
}

static void
test_a_with_list_takes_every_spelling (void)
{
    /* A piece that starts with a pen verb starts a setting; the pieces
     * after it are that setting's further arguments -- the three-number
     * colour and the two-argument dash both survive the commas. */
    gchar *text = dump100 ("line 0, 0, 10, 10 with color 1, 0, 0, "
                           "width 2, dash \"dot\", 0.5\n"
                           "line 0, 0, 20, 20");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "width 2.00\n"
                     "dash 0.25 0.75\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "color rgb(0,0,0)\n"
                     "width 1.00\n"
                     "dash solid\n"
                     "line 0.00 100.00 20.00 80.00\n");
    g_free (text);
}

static void
test_only_what_changed_is_given_back (void)
{
    /* A setting the pen already had costs no restore. */
    gchar *text = dump100 ("color \"red\"\n"
                           "line 0, 0, 10, 10 with color \"red\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "color rgb(255,0,0)\n"
                     "line 0.00 100.00 10.00 90.00\n");
    g_free (text);
}

static void
test_a_with_block_nests (void)
{
    /* The block form, and one inside another: each `end` gives back
     * what its own `with` found, not the frame's defaults. */
    gchar *text = dump100 ("with color \"blue\", width 3\n"
                           "    line 0, 0, 10, 10\n"
                           "    with width 5, align \"left\"\n"
                           "        text 50, 50, \"x\"\n"
                           "    end\n"
                           "    line 0, 0, 20, 20\n"
                           "end\n"
                           "line 0, 0, 30, 30");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(0,0,255)\n"
                     "width 3.00\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "width 5.00\n"
                     "align left middle\n"
                     "text 50.00 50.00 left middle \"x\"\n"
                     "width 3.00\n"
                     "align centre middle\n"
                     "line 0.00 100.00 20.00 80.00\n"
                     "color rgb(0,0,0)\n"
                     "width 1.00\n"
                     "line 0.00 100.00 30.00 70.00\n");
    g_free (text);
}

static void
test_the_pen_position_and_the_view_carry_on (void)
{
    /* `with` scopes HOW the pen draws, not WHERE: a lineto chain runs
     * on out of it, and a view set inside stays -- the width it gives
     * back is one user unit in the new view's device units. */
    gchar *chain = dump100 ("move 10, 10\n"
                            "lineto 20, 20 with color \"red\"\n"
                            "lineto 30, 30");
    gchar *view  = dump100 ("with width 2\n"
                            "    view 0, 0, 50, 50\n"
                            "end\n"
                            "line 0, 0, 10, 10");

    PN_CHECK_CMPSTR (chain, ==, HEAD_100
                     "move 10.00 90.00\n"
                     "color rgb(255,0,0)\n"
                     "line 10.00 90.00 20.00 80.00\n"
                     "color rgb(0,0,0)\n"
                     "line 20.00 80.00 30.00 70.00\n");
    PN_CHECK_CMPSTR (view, ==, HEAD_100
                     "width 2.00\n"
                     "# view 0 0 50 50 scale 2.00 rect 0.00 0.00 100.00 "
                     "100.00\n"
                     "width 4.00\n"
                     "dash solid\n"
                     "font 10.00\n"
                     "width 2.00\n"
                     "line 0.00 100.00 20.00 80.00\n");
    g_free (chain);
    g_free (view);
}

static void
test_a_with_inside_a_loop_scopes_each_pass (void)
{
    gchar *text = dump100 ("repeat 2\n"
                           "    point 10 * i, 10 with width 3\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 3.00\n"
                     "point 0.00 90.00 3.00\n"
                     "width 1.00\n"
                     "width 3.00\n"
                     "point 10.00 90.00 3.00\n"
                     "width 1.00\n");
    g_free (text);
}

static void
test_a_with_value_that_is_a_film_gives_back_per_frame (void)
{
    /* The setting is a vector, so what the `with` changes -- and so what
     * it has to give back -- is decided frame by frame: in frame 0 the
     * colour is the red the pen already had and nothing is restored, in
     * frame 1 it is yellow and the red comes back after the line. */
    const gchar *program = "color \"red\"\n"
                           "line 0, 0, 10, 10  with color 1, frame, 0\n"
                           "line 0, 0, 20, 20";
    gchar       *text;

    text = dump_film (program, NULL, 0, 2, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "color rgb(255,0,0)\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "line 0.00 100.00 20.00 80.00\n");
    g_free (text);

    text = dump_film (program, NULL, 1, 2, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "color rgb(255,255,0)\n"
                     "line 0.00 100.00 10.00 90.00\n"
                     "color rgb(255,0,0)\n"
                     "line 0.00 100.00 20.00 80.00\n");
    g_free (text);
}

static void
test_a_setting_that_cannot_be_drawn_is_skipped (void)
{
    /* 80.10(b) inside a list: the one setting is skipped, the statement
     * still draws with the pen it had, and there is nothing to give
     * back. */
    gchar *text = dump100 ("line 0, 0, 10, 10 with width 0 / 0");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 non-finite\n"
                     "line 0.00 100.00 10.00 90.00\n");
    g_free (text);
}

static void
test_with_is_a_keyword_only_outside_strings (void)
{
    gchar *text = dump100 ("text 0, 0, \"a with b\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "text 0.00 100.00 centre middle \"a with b\"\n");
    g_free (text);
}

static void
test_a_bad_with_list_is_located (void)
{
    /* Every mistake at its own word, and each exactly once: the empty
     * `with` keeps its head so its `end` is not a second message. */
    Split empty   = checked ("with\nend");
    Split no_pen  = checked ("circle 1, 2, 3 with view 0, 0, 1, 1");
    Split no_ink  = checked ("move 1, 2 with color \"red\"");
    Split arity   = checked ("circle 1, 2, 3 with fill 1, 2");
    Split colour  = parsed  ("circle 1, 2, 3 with fill \"nocolour\"");
    Split open    = checked ("with color \"red\"\nline 0, 0, 1, 1");
    Split in_with = checked ("if 1\n"
                             "    with color \"red\"\n"
                             "    else\n"
                             "    end\n"
                             "end");

    PN_CHECK_CMPINT (empty.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&empty, 0), ==, "with needs a pen setting: "
                     "color, fill, nofill, width, dash, font, align, "
                     "arrowhead or angle");

    PN_CHECK_CMPSTR (error_text (&no_pen, 0), ==, "expected a pen setting: "
                     "color, fill, nofill, width, dash, font, align, "
                     "arrowhead or angle");
    PN_CHECK_CMPINT (error_column (&no_pen, 0), ==, 21);

    PN_CHECK_CMPSTR (error_text (&no_ink, 0), ==,
                     "with can only follow a statement that draws");
    PN_CHECK_CMPINT (error_column (&no_ink, 0), ==, 11);

    PN_CHECK_CMPSTR (error_text (&arity, 0), ==,
                     "fill takes a quoted colour or 3 or 4 numbers, not 2");
    PN_CHECK_CMPINT (error_column (&arity, 0), ==, 21);

    PN_CHECK_CMPSTR (error_text (&colour, 0), ==,
                     "unknown colour \"nocolour\"");
    PN_CHECK_CMPINT (error_column (&colour, 0), ==, 26);

    PN_CHECK_CMPINT (open.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&open, 0), ==, "with without an end");

    PN_CHECK_CMPINT (in_with.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&in_with, 0), ==, "else without an if");
    PN_CHECK_CMPINT (error_line (&in_with, 0), ==, 3);

    split_free (&empty);
    split_free (&no_pen);
    split_free (&no_ink);
    split_free (&arity);
    split_free (&colour);
    split_free (&open);
    split_free (&in_with);
}

/* ------------------------------------------------------------------ */
/*  Local axes: origin                                                 */
/* ------------------------------------------------------------------ */

static void
test_an_origin_shifts_what_it_holds (void)
{
    /* Everything inside is drawn from the new origin; after `end` the
     * window's own axes are back. */
    gchar *text = dump100 ("origin 10, 20\n"
                           "    line 0, 0, 5, 0\n"
                           "    point 1, 1\n"
                           "end\n"
                           "point 1, 1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 10.00 80.00 15.00 80.00\n"
                     "point 11.00 79.00 1.00\n"
                     "point 1.00 99.00 1.00\n");
    g_free (text);
}

static void
test_an_origin_turns_its_axes (void)
{
    /* A quarter turn counter-clockwise: local x runs up the window,
     * local y runs left.  A rectangle turned by a whole quarter is
     * still a `rect`, normalised to its device corner. */
    gchar *text = dump100 ("origin 50, 50, 90\n"
                           "    line 0, 0, 10, 0\n"
                           "    rect 0, 0, 10, 5\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 50.00 50.00 50.00 40.00\n"
                     "rect 45.00 40.00 5.00 10.00\n");
    g_free (text);
}

static void
test_a_tilted_rect_is_a_polygon (void)
{
    /* Anything but a whole quarter tilts the rectangle, and the only
     * shape that can draw it is its four corners, closed. */
    gchar *text = dump100 ("origin 50, 50, 30\n"
                           "    rect 0, 0, 10, 0\n"
                           "end");
    gchar *poly = dump100 ("poly 50, 50, 50 + 10 * cos (pi / 6), "
                           "50 + 10 * sin (pi / 6), "
                           "50 + 10 * cos (pi / 6), "
                           "50 + 10 * sin (pi / 6), 50, 50");

    PN_CHECK_CMPSTR (text, ==, poly);
    g_free (text);
    g_free (poly);
}

static void
test_circles_arcs_and_labels_under_turned_axes (void)
{
    /* A circle keeps its radius; an arc's sweep turns with the axes; a
     * label's anchor moves and the label itself stays upright, so its
     * alignment is the pen's and not turned. */
    gchar *turned = dump100 ("origin 50, 50, 90\n"
                             "    circle 10, 0, 3\n"
                             "    arc 0, 0, 10, 0, 90\n"
                             "    text 10, 0, \"x\"\n"
                             "end");
    gchar *plain  = dump100 ("circle 50, 60, 3\n"
                             "arc 50, 50, 10, 90, 180\n"
                             "text 50, 60, \"x\"");

    PN_CHECK_CMPSTR (turned, ==, plain);
    g_free (turned);
    g_free (plain);
}

static void
test_origins_nest (void)
{
    /* Each block is placed in the axes of the one around it, and the
     * turns add up: two quarter turns are a half turn, exactly. */
    gchar *text = dump100 ("origin 10, 0, 90\n"
                           "    origin 10, 0, 90\n"
                           "        point 10, 0\n"
                           "        rect 0, 0, 10, 5\n"
                           "    end\n"
                           "    point 10, 0\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 0.00 90.00 1.00\n"
                     "rect 0.00 90.00 10.00 5.00\n"
                     "point 10.00 90.00 1.00\n");
    g_free (text);
}

static void
test_the_pen_leaves_an_origin_where_it_was (void)
{
    /* The pen is kept in the window's units, so a chain can run out of
     * a block; a relative step inside is turned with the axes. */
    gchar *chain = dump100 ("origin 10, 10\n"
                            "    move 0, 0\n"
                            "end\n"
                            "lineto 20, 20");
    gchar *step  = dump100 ("origin 50, 50, 90\n"
                            "    move 10, 0\n"
                            "    rline 10, 0\n"
                            "    rmove 0, 10\n"
                            "end\n"
                            "rline 5, 0");

    PN_CHECK_CMPSTR (chain, ==, HEAD_100
                     "move 10.00 90.00\n"
                     "line 10.00 90.00 20.00 80.00\n");
    PN_CHECK_CMPSTR (step, ==, HEAD_100
                     "move 50.00 40.00\n"
                     "line 50.00 40.00 50.00 30.00\n"
                     "move 40.00 30.00\n"
                     "line 40.00 30.00 45.00 30.00\n");
    g_free (chain);
    g_free (step);
}

static void
test_an_origin_is_not_a_pen_scope (void)
{
    /* It moves the axes and nothing else: a setting made inside stands
     * after it, a `with` inside it is its own scope, and a `view`
     * inside changes the window the axes are measured in -- the origin
     * is in user units, so it stays at user (10, 10). */
    gchar *pen  = dump100 ("origin 10, 10\n"
                           "    color \"red\"\n"
                           "    point 0, 0 with width 3\n"
                           "end\n"
                           "point 0, 0");
    gchar *view = dump100 ("origin 10, 10\n"
                           "    view 0, 0, 50, 50\n"
                           "    point 0, 0\n"
                           "end");

    PN_CHECK_CMPSTR (pen, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "width 3.00\n"
                     "point 10.00 90.00 3.00\n"
                     "width 1.00\n"
                     "point 0.00 100.00 1.00\n");
    PN_CHECK_CMPSTR (view, ==, HEAD_100
                     "# view 0 0 50 50 scale 2.00 rect 0.00 0.00 100.00 "
                     "100.00\n"
                     "width 2.00\n"
                     "dash solid\n"
                     "font 10.00\n"
                     "point 20.00 80.00 2.00\n");
    g_free (pen);
    g_free (view);
}

static void
test_an_origin_inside_a_loop (void)
{
    /* The spokes of a wheel: one statement turned by the index. */
    gchar *text = dump100 ("repeat 4\n"
                           "    origin 50, 50, 90 * i\n"
                           "        line 0, 0, 10, 0\n"
                           "    end\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 50.00 50.00 60.00 50.00\n"
                     "line 50.00 50.00 50.00 40.00\n"
                     "line 50.00 50.00 40.00 50.00\n"
                     "line 50.00 50.00 50.00 60.00\n");
    g_free (text);
}

static void
test_an_origin_that_cannot_be_placed_skips_the_block (void)
{
    /* 80.10(b): one marker at the `origin`, nothing of the block -- not
     * even the `with` inside it, whose `end` must not give back a pen
     * it never took -- and the figure carries on after it. */
    gchar *text = dump100 ("origin 0 / 0, 0\n"
                           "    point 1, 1\n"
                           "    with width 2\n"
                           "        point 2, 2\n"
                           "    end\n"
                           "end\n"
                           "point 3, 3");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 non-finite\n"
                     "point 3.00 97.00 1.00\n");
    g_free (text);
}

static void
test_turning_axes_animate_without_a_walk_per_frame (void)
{
    /* The angle is a film: each frame turns the axes by its element,
     * but the walk is done once -- unlike an `if` on a vector, which
     * has to be decided per frame. */
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("origin 50, 50, a\n"
                                         "    line 0, 0, 10, 0\n"
                                         "end");
    const gdouble     angles[2] = { 0.0, 90.0 };
    PnFigureFilm      film      = { 0, 0, PN_FIGURE_PLAY_LOOP };
    PnFigureTrace    *trace;
    GPtrArray        *ops;
    gchar            *text;

    snapshot_vector (snapshot, "a", angles, 2);

    trace = pn_figure_trace_new (f.parse.statements, f.names, snapshot,
                                 &film);
    PN_CHECK (pn_figure_trace_is_for (trace, 1));

    ops  = pn_figure_trace_draw (trace, 0, 0, 0, 100, 100, FALSE, NULL);
    text = pn_figure_display_to_string (ops);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 50.00 50.00 60.00 50.00\n");
    g_free (text);
    g_ptr_array_unref (ops);

    ops  = pn_figure_trace_draw (trace, 1, 0, 0, 100, 100, FALSE, NULL);
    text = pn_figure_display_to_string (ops);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 50.00 50.00 50.00 40.00\n");
    g_free (text);
    g_ptr_array_unref (ops);

    pn_figure_trace_free (trace);
    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_bad_origin_is_located (void)
{
    Split arity   = checked ("origin 1\nend");
    Split kind    = checked ("origin 1, 2, \"x\"\nend");
    Split open    = checked ("origin 1, 2\npoint 0, 0");
    Split in_orig = checked ("if 1\n"
                             "    origin 1, 2\n"
                             "    else\n"
                             "    end\n"
                             "end");

    PN_CHECK_CMPSTR (error_text (&arity, 0), ==,
                     "origin takes 2 or 3 arguments, not 1");
    PN_CHECK_CMPSTR (error_text (&kind, 0), ==,
                     "expected an expression, not a string");
    PN_CHECK_CMPINT (error_column (&kind, 0), ==, 14);

    PN_CHECK_CMPINT (open.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&open, 0), ==, "origin without an end");

    PN_CHECK_CMPINT (in_orig.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&in_orig, 0), ==, "else without an if");
    PN_CHECK_CMPINT (error_line (&in_orig, 0), ==, 3);

    split_free (&arity);
    split_free (&kind);
    split_free (&open);
    split_free (&in_orig);
}

/* ------------------------------------------------------------------ */
/*  User-defined shapes: def                                           */
/* ------------------------------------------------------------------ */

static void
test_a_shape_draws_where_it_is_called (void)
{
    /* A def draws nothing where it stands; each call draws the body
     * with its own arguments, exactly as if it were written out. */
    gchar *shape = dump100 ("def box x, y, w\n"
                            "    rect x, y, w, w / 2\n"
                            "    point x, y\n"
                            "end\n"
                            "box 10, 20, 8\n"
                            "box 50, 60, 4");
    gchar *plain = dump100 ("rect 10, 20, 8, 4\n"
                            "point 10, 20\n"
                            "rect 50, 60, 4, 2\n"
                            "point 50, 60");

    PN_CHECK_CMPSTR (shape, ==, plain);
    g_free (shape);
    g_free (plain);
}

static void
test_a_shape_may_take_nothing (void)
{
    /* A hub, drawn wherever an origin puts it. */
    gchar *shape = dump100 ("def hub\n"
                            "    circle 0, 0, 3\n"
                            "end\n"
                            "origin 20, 30\n"
                            "    hub\n"
                            "end\n"
                            "HUB");

    PN_CHECK_CMPSTR (shape, ==, HEAD_100
                     "circle 20.00 70.00 3.00\n"
                     "circle 0.00 100.00 3.00\n");
    g_free (shape);
}

static void
test_a_shape_has_a_store_of_its_own (void)
{
    /* A parameter shadows a program variable of the same name, a name
     * the body assigns is its own -- read before the assignment, it is
     * 0 -- and neither leaks out: after the calls, x is still 5 and y
     * is still unbound, so zero-filled. */
    gchar *text = dump100 ("x = 5\n"
                           "def s x\n"
                           "    point y, x\n"
                           "    y = x + 1\n"
                           "    point y, x\n"
                           "end\n"
                           "s 10\n"
                           "s 20\n"
                           "point x, y");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 0.00 90.00 1.00\n"
                     "point 11.00 90.00 1.00\n"
                     "point 0.00 80.00 1.00\n"
                     "point 21.00 80.00 1.00\n"
                     "point 5.00 100.00 1.00\n");
    g_free (text);
}

static void
test_a_call_gives_the_pen_back (void)
{
    /* Like a `with`: what the shape sets is given back at its end, and
     * only what differs.  The pen position carries on. */
    gchar *shape = dump100 ("def dot x, y\n"
                            "    color \"red\"\n"
                            "    width 3\n"
                            "    move x, y\n"
                            "end\n"
                            "width 3\n"
                            "dot 10, 10\n"
                            "lineto 20, 20");
    gchar *with  = dump100 ("width 3\n"
                            "with color \"red\"\n"
                            "    width 3\n"
                            "    move 10, 10\n"
                            "end\n"
                            "lineto 20, 20");

    PN_CHECK_CMPSTR (shape, ==, with);
    g_free (shape);
    g_free (with);
}

static void
test_a_call_takes_a_trailing_with (void)
{
    /* A call draws, so it may carry a list -- even when its def comes
     * further down, where the verb table then refuses the call for
     * coming first rather than the list for following it. */
    gchar *shape = dump100 ("def dot x, y\n"
                            "    point x, y\n"
                            "end\n"
                            "dot 10, 10 with color \"red\"\n"
                            "dot 20, 20");
    gchar *plain = dump100 ("point 10, 10 with color \"red\"\n"
                            "point 20, 20");
    Split  early = parsed ("dot 1, 1 with width 2\n"
                           "def dot x, y\n"
                           "end");

    PN_CHECK_CMPSTR (shape, ==, plain);
    PN_CHECK_CMPINT (early.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&early, 0), ==,
                     "shape \"dot\" is used before its def");
    g_free (shape);
    g_free (plain);
    split_free (&early);
}

static void
test_shapes_use_the_shapes_above_them (void)
{
    /* A pulley is a disc and a hub; the calls nest, each in its own
     * store, and each call gives back its own pen. */
    gchar *shape = dump100 ("def hub x, y\n"
                            "    point x, y with width 2\n"
                            "end\n"
                            "def pulley x, y, r\n"
                            "    circle x, y, r\n"
                            "    hub x, y\n"
                            "end\n"
                            "pulley 30, 30, 10\n"
                            "pulley 70, 30, 5");
    gchar *plain = dump100 ("circle 30, 30, 10\n"
                            "point 30, 30 with width 2\n"
                            "circle 70, 30, 5\n"
                            "point 70, 30 with width 2");

    PN_CHECK_CMPSTR (shape, ==, plain);
    g_free (shape);
    g_free (plain);
}

static void
test_a_shape_loops_inside_a_loop (void)
{
    /* `repeat` does not nest, but a shape with a loop of its own may be
     * called from one: its `i` is in its own store, so the caller's is
     * left alone. */
    gchar *text = dump100 ("def ticks x\n"
                           "    repeat 3\n"
                           "        point x, i\n"
                           "    end\n"
                           "end\n"
                           "repeat 2\n"
                           "    ticks 10 * i\n"
                           "    point 50, 10 * i\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 0.00 100.00 1.00\n"
                     "point 0.00 99.00 1.00\n"
                     "point 0.00 98.00 1.00\n"
                     "point 50.00 100.00 1.00\n"
                     "point 10.00 100.00 1.00\n"
                     "point 10.00 99.00 1.00\n"
                     "point 10.00 98.00 1.00\n"
                     "point 50.00 90.00 1.00\n");
    g_free (text);
}

static void
test_a_runaway_walk_empties_the_figure (void)
{
    /* A thousand calls of a thousand-step loop is a million statements:
     * over the bound, the frame is an error, not a frozen editor. */
    Figure  f     = figure ("def row\n"
                            "    repeat 1000\n"
                            "        point i, 0\n"
                            "    end\n"
                            "end\n"
                            "repeat 1000\n"
                            "    row\n"
                            "end");
    gchar  *error = NULL;
    gchar  *text  = figure_dump (&f, NULL, 100, 100, FALSE, &error);

    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (error, ==,
                     "line 3, column 9: the figure runs more than 200000 "
                     "statements");
    g_free (text);
    g_free (error);
    figure_free (&f);
}

static void
test_a_vector_argument_animates_the_shape (void)
{
    /* An argument is evaluated in the caller as the vector it is, and
     * the body computes the whole film from it, like any statement. */
    PnFigureSnapshot *snapshot = pn_figure_snapshot_new ();
    Figure            f        = figure ("def dot x\n"
                                         "    point x * 2, 0\n"
                                         "end\n"
                                         "dot a");
    const gdouble     xs[2]    = { 1.0, 5.0 };
    gchar            *text;

    snapshot_vector (snapshot, "a", xs, 2);

    text = figure_dump_frame (&f, snapshot, 1, 100, 100, FALSE, NULL);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "point 10.00 100.00 1.00\n");
    g_free (text);

    figure_free (&f);
    pn_figure_snapshot_free (snapshot);
}

static void
test_a_comparison_in_a_shape_is_per_frame (void)
{
    /* A shape evaluates in a store of its own, and that store compares
     * elementwise like the program's: `frame` passed in as `v` makes
     * `v > 1` a 0, 0, 1, 1 film that the `if` follows, and `v >= 2` a
     * coordinate that moves only from frame 2 on.  A store that fell back
     * to the Calculator's all-elements answer would give 0 in every
     * frame. */
    const gchar *program = "def d v\n"
                           "    if v > 1\n"
                           "        point 10, 10\n"
                           "    else\n"
                           "        point 20, 20\n"
                           "    end\n"
                           "    point 30, 30 * (v >= 2)\n"
                           "end\n"
                           "d frame";
    gchar       *text;

    text = dump_film (program, NULL, 1, 4, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 20.00 80.00 1.00\n"
                     "point 30.00 100.00 1.00\n");
    g_free (text);

    text = dump_film (program, NULL, 2, 4, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "point 10.00 90.00 1.00\n"
                     "point 30.00 70.00 1.00\n");
    g_free (text);
}

static void
test_a_shape_reads_only_what_it_is_given (void)
{
    /* The body's names are not the program's: they are neither free
     * names to zero-fill nor allowed to be. */
    Figure f   = figure ("def s x\n"
                         "    y = x * k\n"
                         "    point y, t\n"
                         "end\n"
                         "s 1\n"
                         "point a, 0");
    Split  ok  = parsed ("def s x\n"
                         "    repeat x\n"
                         "        point i, pi\n"
                         "    end\n"
                         "    z = 1\n"
                         "end");
    Split  idx = parsed ("def s x\n"
                         "    point i, x\n"
                         "end");

    PN_CHECK_CMPINT (f.parse.errors->len, ==, 2);
    PN_CHECK_CMPSTR (error_text (&f.parse, 0), ==,
                     "shape \"s\" cannot read \"k\": pass it in as a "
                     "parameter");
    PN_CHECK_CMPINT (error_line (&f.parse, 0), ==, 2);
    PN_CHECK_CMPSTR (error_text (&f.parse, 1), ==,
                     "shape \"s\" cannot read \"t\": pass it in as a "
                     "parameter");
    PN_CHECK_CMPINT (error_column (&f.parse, 1), ==, 14);
    PN_CHECK_CMPINT (f.names->len, ==, 1);
    PN_CHECK_CMPSTR (g_ptr_array_index (f.names, 0), ==, "a");

    PN_CHECK_CMPINT (ok.errors->len, ==, 0);

    /* Outside a loop of its own, `i` is nobody's. */
    PN_CHECK_CMPSTR (error_text (&idx, 0), ==,
                     "shape \"s\" cannot read \"i\": pass it in as a "
                     "parameter");

    figure_free (&f);
    split_free (&ok);
    split_free (&idx);
}

static void
test_an_arrow_ends_in_a_solid_head (void)
{
    /* The shaft stops at the head's base, and the head is a triangle
     * filled in the stroke colour with a hairline outline, the tip on
     * the second point; the pen gets its fill and width straight back. */
    gchar *text = dump100 ("arrow 10, 10, 50, 10\n"
                           "line 0, 0, 1, 1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 10.00 90.00 47.00 90.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 90.00 47.00 88.80 47.00 91.20\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 0.00 100.00 1.00 99.00\n");
    g_free (text);
}

static void
test_an_arrowhead_is_never_dashed_or_hollow (void)
{
    /* A dashed shaft, a fill of another colour: the head is still solid
     * and still the stroke's colour, and everything is given back. */
    gchar *text = dump100 ("color \"red\"\n"
                           "fill \"blue\"\n"
                           "dash \"dash\"\n"
                           "arrow 50, 10, 50, 50");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "fill rgb(0,0,255)\n"
                     "dash 3.00 2.00\n"
                     "line 50.00 90.00 50.00 53.00\n"
                     "dash solid\n"
                     "width 0.00\n"
                     "fill rgb(255,0,0)\n"
                     "poly 50.00 50.00 48.80 53.00 51.20 53.00\n"
                     "fill rgb(0,0,255)\n"
                     "width 1.00\n"
                     "dash 3.00 2.00\n");
    g_free (text);
}

static void
test_a_short_arrow_shrinks_its_head (void)
{
    /* Half as long as the head: no shaft, and a head half the size, so
     * the arrow never reaches back past its first point.  A `head`
     * never shrinks -- its first point only gives the direction. */
    gchar *arrow = dump100 ("arrow 10, 10, 11.5, 10");
    gchar *head  = dump100 ("head 10, 10, 11.5, 10");

    PN_CHECK_CMPSTR (arrow, ==, HEAD_100
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 11.50 90.00 10.00 89.40 10.00 90.60\n"
                     "nofill\n"
                     "width 1.00\n");
    PN_CHECK_CMPSTR (head, ==, HEAD_100
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 11.50 90.00 8.50 88.80 8.50 91.20\n"
                     "nofill\n"
                     "width 1.00\n");
    g_free (arrow);
    g_free (head);
}

static void
test_the_arrowhead_setting_sizes_the_head (void)
{
    /* A pen setting like any other: it holds until changed, a `with`
     * scopes it, and it emits nothing of its own. */
    gchar *text = dump100 ("arrowhead 4, 2\n"
                           "head 0, 50, 50, 50\n"
                           "head 0, 50, 50, 50 with arrowhead 10, 6\n"
                           "head 0, 50, 50, 50");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 50.00 46.00 49.00 46.00 51.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 50.00 40.00 47.00 40.00 53.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 50.00 46.00 49.00 46.00 51.00\n"
                     "nofill\n"
                     "width 1.00\n");
    g_free (text);
}

static void
test_an_arrow_with_no_direction_is_skipped (void)
{
    /* Two equal points point nowhere, and a head of no size is no head:
     * both are values, skipped as a zero radius is. */
    gchar *text = dump100 ("arrow 5, 5, 5, 5\n"
                           "head 5, 5, 5, 5\n"
                           "arrowhead 0, 2\n"
                           "arrowhead 2, -1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n"
                     "# skip 3 degenerate\n"
                     "# skip 4 degenerate\n");
    g_free (text);
}

static void
test_an_arrow_turns_with_its_axes_and_leaves_the_pen (void)
{
    /* On axes turned a quarter the arrow points up, and like the other
     * shapes it is placed absolutely: the `lineto` after it starts from
     * where the pen was, the frame's origin. */
    gchar *turned = dump100 ("origin 50, 50, 90\n"
                             "    arrow 0, 0, 20, 0\n"
                             "end\n"
                             "lineto 10, 10");
    gchar *plain  = dump100 ("arrow 50, 50, 50, 70\n"
                             "lineto 10, 10");

    PN_CHECK_CMPSTR (turned, ==, plain);
    PN_CHECK_CMPSTR (plain, ==, HEAD_100
                     "line 50.00 50.00 50.00 33.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 30.00 48.80 33.00 51.20 33.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 0.00 100.00 10.00 90.00\n");
    g_free (turned);
    g_free (plain);
}

static void
test_a_trailing_with_scopes_the_whole_arrow (void)
{
    /* Two scopes, one inside the other: the head takes its hairline and
     * solid fill from the `with` colour and gives back the shaft's pen
     * (nofill, width 2), and then the `with` gives back the figure's
     * (black, width 1) -- the line after it is drawn in neither. */
    gchar *text = dump100 ("arrow 10, 10, 50, 10  with color \"red\", width 2\n"
                           "line 0, 0, 1, 1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "width 2.00\n"
                     "line 10.00 90.00 47.00 90.00\n"
                     "width 0.00\n"
                     "fill rgb(255,0,0)\n"
                     "poly 50.00 90.00 47.00 88.80 47.00 91.20\n"
                     "nofill\n"
                     "width 2.00\n"
                     "color rgb(0,0,0)\n"
                     "width 1.00\n"
                     "line 0.00 100.00 1.00 99.00\n");
    g_free (text);
}

static void
test_an_arrow_in_a_film_is_sized_per_frame (void)
{
    /* One walk, three frames, three different arrows: of no length it
     * is skipped, 2 long it is a head shrunk to 2/3 with no shaft, 4
     * long it is a full head (3 by 2.4) on a shaft of 1. */
    const gchar *program = "arrow 10, 10, 10 + 2 * frame, 10";
    gchar       *text;

    text = dump_film (program, NULL, 0, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "# skip 1 degenerate\n");
    g_free (text);

    text = dump_film (program, NULL, 1, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 12.00 90.00 10.00 89.20 10.00 90.80\n"
                     "nofill\n"
                     "width 1.00\n");
    g_free (text);

    text = dump_film (program, NULL, 2, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 10.00 90.00 11.00 90.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 14.00 90.00 11.00 88.80 11.00 91.20\n"
                     "nofill\n"
                     "width 1.00\n");
    g_free (text);
}

static void
test_an_arrow_stretches_with_the_view (void)
{
    /* The head is part of the drawing, built in user units, so a
     * stretched window stretches it like any line: 3 long along x at
     * scale 1, and 2.4 across it along y at scale 2 -- 4.8 pixels. */
    Figure  f    = figure ("view 0, 0, 100, 50\narrow 10, 10, 50, 10");
    gchar  *text = figure_dump (&f, NULL, 100, 100, TRUE, NULL);

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 0 0 100 50 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00 stretch 1.00 2.00\n"
                     "line 10.00 80.00 47.00 80.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 80.00 47.00 77.60 47.00 82.40\n"
                     "nofill\n"
                     "width 1.00\n");
    g_free (text);
    figure_free (&f);
}

static void
test_a_hatch_strokes_its_left_side (void)
{
    /* A ceiling drawn left to right: the line at the pen's width, then
     * strokes at half of it, above, leaning forward, 10 apart and
     * centred -- 40 long takes four, 5 clear at each end. */
    gchar *text = dump100 ("width 2\n"
                           "hatch 10, 50, 50, 50, 10, 4");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 2.00\n"
                     "line 10.00 50.00 50.00 50.00\n"
                     "width 1.00\n"
                     "line 15.00 50.00 19.00 46.00\n"
                     "line 25.00 50.00 29.00 46.00\n"
                     "line 35.00 50.00 39.00 46.00\n"
                     "line 45.00 50.00 49.00 46.00\n"
                     "width 2.00\n");
    g_free (text);
}

static void
test_a_negative_depth_hatches_the_other_side (void)
{
    /* Below the line, and the depth left out is 3/4 of the spacing.
     * Half of width 1 is below the painter's thinnest stroke, 0.75. */
    gchar *text = dump100 ("hatch 10, 50, 30, 50, 10, -7.5");
    gchar *same = dump100 ("hatch 30, 50, 10, 50, 10");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 10.00 50.00 30.00 50.00\n"
                     "width 0.75\n"
                     "line 15.00 50.00 22.50 57.50\n"
                     "line 25.00 50.00 32.50 57.50\n"
                     "width 1.00\n");

    /* Run the other way, the left side is below too, and the strokes
     * lean the other way. */
    PN_CHECK_CMPSTR (same, ==, HEAD_100
                     "line 30.00 50.00 10.00 50.00\n"
                     "width 0.75\n"
                     "line 25.00 50.00 17.50 57.50\n"
                     "line 15.00 50.00 7.50 57.50\n"
                     "width 1.00\n");
    g_free (text);
    g_free (same);
}

static void
test_a_hatch_turns_with_its_axes (void)
{
    gchar *turned = dump100 ("origin 50, 50, 90\n"
                             "    hatch 0, 0, 20, 0, 10, 4\n"
                             "end");
    gchar *plain  = dump100 ("hatch 50, 50, 50, 70, 10, 4");

    PN_CHECK_CMPSTR (turned, ==, plain);
    PN_CHECK_CMPSTR (plain, ==, HEAD_100
                     "line 50.00 50.00 50.00 30.00\n"
                     "width 0.75\n"
                     "line 50.00 45.00 46.00 41.00\n"
                     "line 50.00 35.00 46.00 31.00\n"
                     "width 1.00\n");
    g_free (turned);
    g_free (plain);
}

static void
test_a_hatch_that_cannot_be_drawn_is_skipped (void)
{
    /* No length, no spacing, no depth, or a spacing that would ask for
     * more strokes than a repeat may run. */
    gchar *text = dump100 ("hatch 5, 5, 5, 5, 1\n"
                           "hatch 0, 0, 10, 0, 0\n"
                           "hatch 0, 0, 10, 0, 1, 0\n"
                           "hatch 0, 0, 100, 0, 0.01\n"
                           "hatch 0, 0, 0.1, 0, 1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n"
                     "# skip 3 degenerate\n"
                     "# skip 4 too-many\n"
                     "line 0.00 100.00 0.10 100.00\n"
                     "width 0.75\n"
                     "line 0.05 100.00 0.80 99.25\n"
                     "width 1.00\n");
    g_free (text);
}

static void
test_a_trailing_with_scopes_the_whole_hatch (void)
{
    /* The strokes take half the `with` width, not half the figure's,
     * and the hatch's own restore (width 2) comes before the `with`'s
     * (black, width 1). */
    gchar *text = dump100 ("hatch 10, 50, 30, 50, 10, 4  "
                           "with color \"red\", width 2\n"
                           "line 0, 0, 1, 1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "width 2.00\n"
                     "line 10.00 50.00 30.00 50.00\n"
                     "width 1.00\n"
                     "line 15.00 50.00 19.00 46.00\n"
                     "line 25.00 50.00 29.00 46.00\n"
                     "width 2.00\n"
                     "color rgb(0,0,0)\n"
                     "width 1.00\n"
                     "line 0.00 100.00 1.00 99.00\n");
    g_free (text);
}

static void
test_a_hatch_in_a_film_counts_its_strokes_per_frame (void)
{
    /* The number of strokes depends on the length, and the length is a
     * film: 10, 30 and 50 long take 1, 3 and 5 strokes, 10 apart and
     * centred, from the one walk the whole film shares. */
    const gchar *program = "hatch 0, 50, 10 + 20 * frame, 50, 10";
    gchar       *text;

    text = dump_film (program, NULL, 0, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 50.00 10.00 50.00\n"
                     "width 0.75\n"
                     "line 5.00 50.00 12.50 42.50\n"
                     "width 1.00\n");
    g_free (text);

    text = dump_film (program, NULL, 2, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 50.00 50.00 50.00\n"
                     "width 0.75\n"
                     "line 5.00 50.00 12.50 42.50\n"
                     "line 15.00 50.00 22.50 42.50\n"
                     "line 25.00 50.00 32.50 42.50\n"
                     "line 35.00 50.00 42.50 42.50\n"
                     "line 45.00 50.00 52.50 42.50\n"
                     "width 1.00\n");
    g_free (text);

    /* And the middle frame, counted: the line, three strokes and
     * the two width changes around them. */
    text = dump_film (program, NULL, 1, 3, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPINT (count_lines (text), ==, 6 + head_lines (HEAD_100));
    g_free (text);
}

static void
test_a_dimension_ticks_both_ends (void)
{
    /* Upwards, so the left side is towards smaller x: each tick runs
     * from right to left, 4 long, centred on its end. */
    gchar *text = dump100 ("dimension 10, 20, 10, 60, 4  with width 2");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "width 2.00\n"
                     "line 10.00 80.00 10.00 40.00\n"
                     "line 12.00 80.00 8.00 80.00\n"
                     "line 12.00 40.00 8.00 40.00\n"
                     "width 1.00\n");
    g_free (text);
}

static void
test_a_dimension_tick_is_3_when_left_out (void)
{
    gchar *text = dump100 ("dimension 20, 50, 60, 50");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 20.00 50.00 60.00 50.00\n"
                     "line 20.00 51.50 20.00 48.50\n"
                     "line 60.00 51.50 60.00 48.50\n");
    g_free (text);
}

static void
test_a_dimension_turns_with_its_axes_and_leaves_the_pen (void)
{
    gchar *turned = dump100 ("move 5, 5\n"
                             "origin 50, 50, 90\n"
                             "    dimension 0, 0, 20, 0, 4\n"
                             "end\n"
                             "lineto 10, 10");
    gchar *plain  = dump100 ("move 5, 5\n"
                             "dimension 50, 50, 50, 70, 4\n"
                             "lineto 10, 10");

    PN_CHECK_CMPSTR (turned, ==, plain);
    PN_CHECK_CMPSTR (plain, ==, HEAD_100
                     "move 5.00 95.00\n"
                     "line 50.00 50.00 50.00 30.00\n"
                     "line 52.00 50.00 48.00 50.00\n"
                     "line 52.00 30.00 48.00 30.00\n"
                     "line 5.00 95.00 10.00 90.00\n");
    g_free (turned);
    g_free (plain);
}

static void
test_a_dimension_that_cannot_be_drawn_is_skipped (void)
{
    /* No span to measure, or a tick with no length. */
    gchar *text = dump100 ("dimension 5, 5, 5, 5\n"
                           "dimension 0, 0, 10, 0, 0\n"
                           "dimension 0, 0, 10, 0, -1");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n"
                     "# skip 3 degenerate\n");
    g_free (text);
}

static void
test_an_anglemark_labels_its_bisector (void)
{
    /* The arc as `arc` draws it, and the label on the bisector 2 (0.4
     * of the font) past it, aligned to grow away up and to the right. */
    gchar *text = dump100 ("anglemark 50, 50, 10, 0, 90, \"%.0f deg\", 90");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "arc 50.00 50.00 10.00 0.00 -90.00 negative\n"
                     "align left bottom\n"
                     "text 58.49 41.51 left bottom \"90 deg\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_an_anglemark_label_is_optional (void)
{
    gchar *mark = dump100 ("anglemark 50, 50, 10, 30, 60");
    gchar *arc  = dump100 ("arc 50, 50, 10, 30, 60");

    PN_CHECK_CMPSTR (mark, ==, arc);
    g_free (mark);
    g_free (arc);
}

static void
test_an_anglemark_label_picks_its_side (void)
{
    /* Straight down, straight left, and down-left: the label always
     * grows away from the arc, whatever the pen's own alignment. */
    gchar *text = dump100 ("align \"right\", \"top\"\n"
                           "anglemark 50, 50, 10, -100, -80, \"S\"\n"
                           "anglemark 50, 50, 10, 170, 190, \"W\"\n"
                           "anglemark 50, 50, 10, 200, 250, \"SW\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "align right top\n"
                     "arc 50.00 50.00 10.00 100.00 80.00 negative\n"
                     "align centre top\n"
                     "text 50.00 62.00 centre top \"S\"\n"
                     "align right top\n"
                     "arc 50.00 50.00 10.00 -170.00 -190.00 negative\n"
                     "align right middle\n"
                     "text 38.00 50.00 right middle \"W\"\n"
                     "align right top\n"
                     "arc 50.00 50.00 10.00 -200.00 -250.00 negative\n"
                     "text 41.51 58.49 right top \"SW\"\n");
    g_free (text);
}

static void
test_an_anglemark_turns_with_its_axes (void)
{
    /* A quarter turn: the sweep turns, the label moves to the upper
     * left and stays upright. */
    gchar *text = dump100 ("origin 50, 50, 90\n"
                           "    anglemark 0, 0, 10, 0, 90, \"A\"  with angle 45\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "arc 50.00 50.00 10.00 -90.00 -180.00 negative\n"
                     "align right bottom\n"
                     "text 41.51 41.51 right bottom \"A\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_an_anglemark_that_cannot_be_drawn_is_skipped (void)
{
    gchar *text = dump100 ("anglemark 5, 5, 0, 0, 90, \"A\"\n"
                           "anglemark 5, 5, -1, 0, 90");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n");
    g_free (text);
}

static void
test_an_anglemark_label_is_a_format (void)
{
    Split s = literals ("anglemark 0, 0, 5, 0, 90, \"%.1f\"\n"
                        "anglemark 0, 0, 5, 0, 90, \"%s\", a\n"
                        "anglemark 0, 0, 5, 0, 90, 7");

    PN_CHECK_CMPINT (s.statements->len, ==, 0);
    PN_CHECK_CMPINT (s.errors->len, ==, 3);
    /* The kinds are checked before any format is read. */
    PN_CHECK_CMPSTR (error_text (&s, 0), ==, "expected a quoted string");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "format needs 1 value, not 0");
    PN_CHECK_CMPSTR (error_text (&s, 2), ==, "unsupported conversion \"%s\"");

    split_free (&s);
}

static void
test_an_anglemark_in_a_film_follows_its_angle (void)
{
    /* The sweep, the label's value and the side it grows to all come
     * from the frame: at 30 degrees the bisector is at 15, nearly level,
     * so the label hangs off the arc's right, and at 90 the bisector is
     * at 45 and the label grows up and to the right. */
    const gchar *program = "anglemark 50, 50, 10, 0, 30 + 60 * frame, "
                           "\"%.0f\", 30 + 60 * frame";
    gchar       *text;

    text = dump_film (program, NULL, 0, 2, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "arc 50.00 50.00 10.00 0.00 -30.00 negative\n"
                     "align left middle\n"
                     "text 61.59 46.89 left middle \"30\"\n"
                     "align centre middle\n");
    g_free (text);

    text = dump_film (program, NULL, 1, 2, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "arc 50.00 50.00 10.00 0.00 -90.00 negative\n"
                     "align left bottom\n"
                     "text 58.49 41.51 left bottom \"90\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_an_anglemark_in_a_shape (void)
{
    /* The format's value is a parameter like any other argument, and a
     * call's `with` covers the arc and the label alike. */
    gchar *shape = dump100 ("def mark a\n"
                            "    anglemark 50, 50, 10, 0, a, \"%.0f\", a\n"
                            "end\n"
                            "mark 90  with color \"red\"");
    gchar *plain = dump100 ("anglemark 50, 50, 10, 0, 90, \"%.0f\", 90  "
                            "with color \"red\"");

    PN_CHECK_CMPSTR (shape, ==, plain);
    PN_CHECK_CMPSTR (plain, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "arc 50.00 50.00 10.00 0.00 -90.00 negative\n"
                     "align left bottom\n"
                     "text 58.49 41.51 left bottom \"90\"\n"
                     "align centre middle\n"
                     "color rgb(0,0,0)\n");
    g_free (shape);
    g_free (plain);
}

static void
test_an_anglemark_label_follows_a_mirror (void)
{
    /* With x increasing leftwards the bisector points up and to the
     * LEFT on the card, so the label goes there and grows away from
     * the arc that way, right-aligned -- the arc itself is mirrored as
     * `arc` mirrors it. */
    gchar *text = dump100 ("view 100, 0, 0, 100\n"
                           "anglemark 50, 50, 10, 0, 90, \"A\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 100 0 0 100 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00\n"
                     "arc 50.00 50.00 10.00 180.00 270.00 positive\n"
                     "align right bottom\n"
                     "text 41.51 41.51 right bottom \"A\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_a_curve_keeps_its_control_points (void)
{
    gchar *text = dump100 ("curve 10, 10, 10, 30, 30, 30, 30, 10,\n"
                           "      30, -10, 50, -10, 50, 10");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "curve 10.00 90.00 10.00 70.00 30.00 70.00 30.00 90.00"
                     " 30.00 110.00 50.00 110.00 50.00 90.00\n");
    g_free (text);
}

static void
test_a_curve_that_comes_back_is_closed (void)
{
    /* The lens: two segments ending where the first began, drawn on
     * turned axes so the control points are seen to turn too. */
    gchar *text = dump100 ("origin 50, 50, 90\n"
                           "    curve 0, 30, 4, 10, 4, -10, 0, -30,\n"
                           "          -4, -10, -4, 10, 0, 30\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "curve 20.00 50.00 40.00 46.00 60.00 46.00 80.00 50.00"
                     " 60.00 54.00 40.00 54.00 20.00 50.00 closed\n");
    g_free (text);
}

static void
test_a_curve_counts_its_points (void)
{
    Split s = checked ("curve 0,0, 1,1, 2,2\n"
                       "curve 0,0, 1,1, 2,2, 3,3, 4,4\n"
                       "curve 0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6");

    PN_CHECK_CMPINT (s.statements->len, ==, 1);
    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "curve takes at least 8 arguments, not 6");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==,
                     "curve takes a start point and then three points per "
                     "segment, not 10 numbers");

    split_free (&s);
}

static void
test_a_curve_stretches_with_the_view (void)
{
    /* Every point, control points included, goes through the per-axis
     * scale, so the stretched curve is the same curve twice as tall. */
    Figure  f    = figure ("view 0, 0, 100, 50\n"
                           "curve 10, 10, 10, 30, 30, 30, 30, 10");
    gchar  *text = figure_dump (&f, NULL, 100, 100, TRUE, NULL);

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# view 0 0 100 50 scale 1.00"
                     " rect 0.00 0.00 100.00 100.00 stretch 1.00 2.00\n"
                     "curve 10.00 80.00 10.00 40.00 30.00 40.00 30.00 80.00\n");
    g_free (text);
    figure_free (&f);
}

static void
test_a_trailing_with_fills_a_closed_curve (void)
{
    /* A closed curve is filled like a poly, so `with fill` fills this
     * one alone and the circle after it is an outline again. */
    gchar *text = dump100 ("curve 10, 10, 10, 30, 30, 30, 10, 10  "
                           "with fill \"red\"\n"
                           "circle 50, 50, 5");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "fill rgb(255,0,0)\n"
                     "curve 10.00 90.00 10.00 70.00 30.00 70.00 10.00 90.00"
                     " closed\n"
                     "nofill\n"
                     "circle 50.00 50.00 5.00\n");
    g_free (text);
}

static void
test_axes_draw_two_arrows_and_name_them (void)
{
    gchar *text = dump100 ("axes 10, 10, 50, 40, \"x\", \"y\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 10.00 90.00 57.00 90.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 60.00 90.00 57.00 88.80 57.00 91.20\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 10.00 90.00 10.00 53.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 10.00 50.00 8.80 53.00 11.20 53.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "align left middle\n"
                     "text 62.00 90.00 left middle \"x\"\n"
                     "align centre middle\n"
                     "align centre bottom\n"
                     "text 10.00 48.00 centre bottom \"y\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_axes_may_point_the_other_way (void)
{
    /* Negative lengths: the arrows point left and down, and the names
     * follow them.  The name of the y axis is left out. */
    gchar *text = dump100 ("axes 50, 50, -20, -20, \"t\"  with arrowhead 2, 2");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 50.00 50.00 32.00 50.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 30.00 50.00 32.00 51.00 32.00 49.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 50.00 50.00 50.00 68.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 70.00 51.00 68.00 49.00 68.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "align right middle\n"
                     "text 28.00 50.00 right middle \"t\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_axes_with_no_length_are_skipped (void)
{
    gchar *text = dump100 ("axes 5, 5, 0, 10\n"
                           "axes 5, 5, 10, 0, \"x\", \"y\"");
    Split  s    = literals ("axes 0, 0, 10, 10, \"%.1f\"\n"
                            "axes 0, 0, 10, 10, \"x\", \"y\", \"z\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n");
    g_free (text);

    /* A name is a label, not a format with values: "%%" is a per cent. */
    PN_CHECK_CMPINT (s.errors->len, ==, 2);
    PN_CHECK_CMPSTR (error_text (&s, 0), ==,
                     "axes takes 4 to 6 arguments, not 7");
    PN_CHECK_CMPSTR (error_text (&s, 1), ==, "format needs 1 value, not 0");
    split_free (&s);
}

static void
test_axes_turn_with_an_origin (void)
{
    /* A quarter turn: the x axis points up the card and the y axis to
     * the left, and each name still sits just past its own tip, upright
     * and aligned away from it -- "x" above, "y" to the left. */
    gchar *text = dump100 ("origin 50, 50, 90\n"
                           "    axes 0, 0, 20, 10, \"x\", \"y\"\n"
                           "end");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 50.00 50.00 50.00 33.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 50.00 30.00 48.80 33.00 51.20 33.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 50.00 50.00 43.00 50.00\n"
                     "width 0.00\n"
                     "fill rgb(0,0,0)\n"
                     "poly 40.00 50.00 43.00 51.20 43.00 48.80\n"
                     "nofill\n"
                     "width 1.00\n"
                     "align centre bottom\n"
                     "text 50.00 28.00 centre bottom \"x\"\n"
                     "align centre middle\n"
                     "align right middle\n"
                     "text 38.00 50.00 right middle \"y\"\n"
                     "align centre middle\n");
    g_free (text);
}

static void
test_axes_in_a_shape_take_the_pen_of_the_call (void)
{
    /* A call's `with color` reaches the shafts, the solid heads (which
     * fill in the stroke colour) and the names, and is given back once,
     * after the whole pair. */
    gchar *shape = dump100 ("def ax x, y\n"
                            "    axes x, y, 20, 10, \"x\", \"y\"\n"
                            "end\n"
                            "ax 10, 10  with color \"red\"\n"
                            "line 0, 0, 1, 1");

    PN_CHECK_CMPSTR (shape, ==, HEAD_100
                     "color rgb(255,0,0)\n"
                     "line 10.00 90.00 27.00 90.00\n"
                     "width 0.00\n"
                     "fill rgb(255,0,0)\n"
                     "poly 30.00 90.00 27.00 88.80 27.00 91.20\n"
                     "nofill\n"
                     "width 1.00\n"
                     "line 10.00 90.00 10.00 83.00\n"
                     "width 0.00\n"
                     "fill rgb(255,0,0)\n"
                     "poly 10.00 80.00 8.80 83.00 11.20 83.00\n"
                     "nofill\n"
                     "width 1.00\n"
                     "align left middle\n"
                     "text 32.00 90.00 left middle \"x\"\n"
                     "align centre middle\n"
                     "align centre bottom\n"
                     "text 10.00 78.00 centre bottom \"y\"\n"
                     "align centre middle\n"
                     "color rgb(0,0,0)\n"
                     "line 0.00 100.00 1.00 99.00\n");
    g_free (shape);
}

static void
test_the_angle_setting_turns_a_label (void)
{
    /* Counter-clockwise on the plate is clockwise-negative on the
     * device; quarter turns are exact, and a whole turn is upright. */
    gchar *text = dump100 ("text 50, 50, \"A\"  with angle 30\n"
                           "text 50, 40, \"B\"\n"
                           "angle 90\n"
                           "text 10, 10, \"C\"\n"
                           "angle 180\n"
                           "text 10, 10, \"D\"\n"
                           "angle -450\n"
                           "text 10, 10, \"E\"\n"
                           "angle 360\n"
                           "text 10, 10, \"F\"");

    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "text 50.00 50.00 centre middle angle -30.00 \"A\"\n"
                     "text 50.00 60.00 centre middle \"B\"\n"
                     "text 10.00 90.00 centre middle angle -90.00 \"C\"\n"
                     "text 10.00 90.00 centre middle angle 180.00 \"D\"\n"
                     "text 10.00 90.00 centre middle angle 90.00 \"E\"\n"
                     "text 10.00 90.00 centre middle \"F\"\n");
    g_free (text);
}

static void
test_a_label_angle_ignores_the_axes (void)
{
    /* The axes move the anchor only; the angle is the drawing's. */
    gchar *turned = dump100 ("origin 50, 50, 90\n"
                             "    text 10, 0, \"U\"\n"
                             "    text 10, 0, \"T\"  with angle 30\n"
                             "end");

    PN_CHECK_CMPSTR (turned, ==, HEAD_100
                     "text 50.00 40.00 centre middle \"U\"\n"
                     "text 50.00 40.00 centre middle angle -30.00 \"T\"\n");
    g_free (turned);
}

static void
test_a_label_angle_follows_a_stretch_not_a_mirror (void)
{
    /* Stretched twice as wide as high, 45 degrees on the plate lies
     * along the same line a 45-degree ray is drawn on: atan(1/2). */
    Figure  f        = figure ("text 50, 50, \"S\"  with angle 45");
    gchar  *stretch  = figure_dump (&f, NULL, 200, 100, TRUE, NULL);
    gchar  *mirrored = dump100 ("view 100, 0, 0, 100\n"
                                "text 50, 50, \"M\"  with angle 30");

    PN_CHECK (strstr (stretch, "angle -26.57 \"S\"") != NULL);

    /* Reversed bounds do not mirror a turn, as they do not mirror an
     * upright label. */
    PN_CHECK (strstr (mirrored, "angle -30.00 \"M\"") != NULL);

    g_free (stretch);
    g_free (mirrored);
    figure_free (&f);
}

static void
test_a_bad_def_is_located (void)
{
    const struct
    {
        const gchar *program;
        const gchar *message;
        gint         line;
        gint         column;
    } cases[] = {
        { "def\nend",                  "def needs the name of the shape it defines", 1, 4 },
        { "def 3x\nend",               "def needs the name of the shape it defines", 1, 5 },
        { "def a, x\nend",             "expected a space after the name of the shape", 1, 6 },
        { "def a x, 2\nend",           "expected a parameter name", 1, 10 },
        { "def a x, , y\nend",         "expected a parameter name", 1, 10 },
        { "def a x, y, x\nend",        "parameter \"x\" is named twice", 1, 13 },
        { "def a pi\nend",             "\"pi\" cannot name a parameter", 1, 7 },
        { "def Circle x\nend",         "\"Circle\" is a verb and cannot name a shape", 1, 5 },
        { "def a\nend\ndef A\nend",    "shape \"A\" is already defined", 3, 5 },
        { "def a\n    a\nend",         "shape \"a\" cannot use itself", 2, 5 },
        { "b\ndef b\nend",             "shape \"b\" is used before its def", 1, 1 },
        { "def a\n    b\nend\ndef b\nend", "shape \"b\" is used before its def", 2, 5 },
        { "sprocket 1, 2",             "unknown verb \"sprocket\"", 1, 1 },
        { "def a x, y\nend\na 1",      "a takes 2 arguments, not 1", 3, 1 },
        { "def a x\nend\na \"red\"",   "expected an expression, not a string", 3, 3 },
        { "if 1\n    def a\n    end\nend", "def must stand outside every block", 2, 5 },
        { "def a\n    view 0, 0, 1, 1\nend", "a shape cannot set the view", 2, 5 },
        { "def a\n    point 0, 0",     "def without an end", 1, 1 },
    };
    guint n;

    for (n = 0; n < G_N_ELEMENTS (cases); n++)
    {
        Split s = parsed (cases[n].program);

        PN_CHECK_CMPINT (s.errors->len, ==, 1);
        PN_CHECK_CMPSTR (error_text (&s, 0), ==, cases[n].message);
        PN_CHECK_CMPINT (error_line (&s, 0), ==, cases[n].line);
        PN_CHECK_CMPINT (error_column (&s, 0), ==, cases[n].column);
        split_free (&s);
    }
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
test_a_vector_input_is_a_film (void)
{
    PnNode     *self    = node ("line 0, 0, value1, 0", 1);
    PnMessage  *message = pn_message_new (NULL, NULL);
    gdouble     numbers[3] = { 10.0, 20.0, 30.0 };
    PnVector   *vec     = pn_vector_new_copy (numbers, 3);
    gchar      *text;

    /* A vector source wired in is a film now, not class (c): nothing
     * red, three frames, and the node shows the first until 82.3's
     * timer moves it. */
    pn_message_set_vector (message, "value", vec);
    pn_node_receive_message_on_input (self, message, 0);
    g_object_unref (vec);
    g_object_unref (message);

    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");
    PN_CHECK (!pn_node_get_has_error (self));
    PN_CHECK_CMPINT (pn_figure_get_frame_count (PN_FIGURE (self)), ==, 3);
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 0);

    text = node_dump (self);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 10.00 100.00\n");
    g_free (text);

    /* Any frame can be dumped (80.16f) without moving the one shown. */
    text = pn_figure_dump (PN_FIGURE (self), 2, 0, 0, 100, 100);
    PN_CHECK_CMPSTR (text, ==, HEAD_100
                     "line 0.00 100.00 30.00 100.00\n");
    g_free (text);
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 0);

    g_object_unref (self);
}

/* The steps @mode takes from frame 0 through a film of @count frames,
 * as "0 1 2 ...", @n ticks long; a tick that ends the film prints
 * "|" after its frame. */
static gchar *
steps (PnFigurePlayMode mode, guint count, gint n)
{
    GString *out       = g_string_new ("0");
    guint    frame     = 0;
    gint     direction = 1;
    gint     k;

    for (k = 0; k < n; k++)
    {
        gboolean more = pn_figure_step_frame (mode, count, &frame, &direction);

        g_string_append_printf (out, " %u%s", frame, more ? "" : "|");
    }

    return g_string_free (out, FALSE);
}

static void
test_the_play_modes_step (void)
{
    gchar *text;

    /* Loop wraps; once stops on the last frame and says so, and keeps
     * holding it; ping-pong bounces without showing an end twice
     * (80.17a). */
    text = steps (PN_FIGURE_PLAY_LOOP, 3, 5);
    PN_CHECK_CMPSTR (text, ==, "0 1 2 0 1 2");
    g_free (text);

    text = steps (PN_FIGURE_PLAY_ONCE, 3, 3);
    PN_CHECK_CMPSTR (text, ==, "0 1 2| 2|");
    g_free (text);

    text = steps (PN_FIGURE_PLAY_PING_PONG, 3, 6);
    PN_CHECK_CMPSTR (text, ==, "0 1 2 1 0 1 2");
    g_free (text);

    text = steps (PN_FIGURE_PLAY_PING_PONG, 2, 3);
    PN_CHECK_CMPSTR (text, ==, "0 1 0 1");
    g_free (text);

    /* A still never moves and never asks for another tick. */
    text = steps (PN_FIGURE_PLAY_LOOP, 1, 2);
    PN_CHECK_CMPSTR (text, ==, "0 0| 0|");
    g_free (text);
}

static void
test_a_frame_past_the_end_steps_from_inside (void)
{
    guint frame     = 7;
    gint  direction = 1;

    /* A shorter film arrived under a running index: brought inside
     * first, then stepped. */
    PN_CHECK (pn_figure_step_frame (PN_FIGURE_PLAY_LOOP, 3, &frame,
                                    &direction));
    PN_CHECK_CMPINT (frame, ==, 0);
}

static void
on_repaint_count (PnNode *node, gpointer user_data)
{
    (void) node;
    *(gint *) user_data += 1;
}

/* Deliver a vector on input 0. */
static void
send_vector (PnNode *self, const gdouble *numbers, gsize n)
{
    PnMessage *message = pn_message_new (NULL, NULL);
    PnVector  *vec     = pn_vector_new_copy (numbers, n);

    pn_message_set_vector (message, "value", vec);
    pn_node_receive_message_on_input (self, message, 0);
    g_object_unref (vec);
    g_object_unref (message);
}

/* Run the default main context until @done says so or a second has
 * passed.  Returns what @done said last. */
static gboolean
pump_until (gboolean (*done) (PnFigure *), PnFigure *figure)
{
    gint64 end = g_get_monotonic_time () + G_TIME_SPAN_SECOND;

    while (!done (figure) && g_get_monotonic_time () < end)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (500);
    }

    return done (figure);
}

static gboolean
at_last_frame (PnFigure *figure)
{
    return pn_figure_get_frame (figure) == 2;
}

static gboolean
stopped (PnFigure *figure)
{
    return !pn_figure_is_playing (figure);
}

static void
test_a_headless_film_does_not_tick (void)
{
    PnNode        *self      = node ("line 0, 0, value1, 0", 1);
    const gdouble  numbers[] = { 10.0, 20.0, 30.0 };

    /* Nothing connected to repaint-needed: no painter, no timer
     * (80.17c) -- the check g_signal_has_handler_pending() makes. */
    send_vector (self, numbers, 3);
    PN_CHECK_CMPINT (pn_figure_get_frame_count (PN_FIGURE (self)), ==, 3);
    PN_CHECK (!pn_figure_is_playing (PN_FIGURE (self)));

    g_object_unref (self);
}

static void
test_a_watched_film_plays_and_stops_when_unwatched (void)
{
    PnNode        *self      = node ("line 0, 0, value1, 0", 1);
    const gdouble  numbers[] = { 10.0, 20.0, 30.0 };
    gint           repaints  = 0;
    gulong         handler;

    handler = g_signal_connect (self, "repaint-needed",
                                G_CALLBACK (on_repaint_count), &repaints);

    send_vector (self, numbers, 3);
    PN_CHECK (pn_figure_is_playing (PN_FIGURE (self)));
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 0);

    /* The timer turns the frames and asks for a repaint each time. */
    PN_CHECK (pump_until (at_last_frame, PN_FIGURE (self)));
    PN_CHECK_CMPINT (repaints, >=, 2);

    /* New data of the same length is the same film with new values:
     * it carries on from where it stands.  A different length is a new
     * film and starts again at frame 0 (80.17b, 82.3d amended). */
    {
        const gdouble longer[] = { 1.0, 2.0, 3.0, 4.0 };
        guint         before   = pn_figure_get_frame (PN_FIGURE (self));

        send_vector (self, numbers, 3);
        PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, before);

        send_vector (self, longer, 4);
        PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 0);
    }

    /* The listener goes -- a node lifted out of its worksheet -- and
     * the next tick stops the timer rather than ticking on. */
    g_signal_handler_disconnect (self, handler);
    PN_CHECK (pump_until (stopped, PN_FIGURE (self)));

    g_object_unref (self);
}

static gboolean
past_frame_five (PnFigure *figure)
{
    return pn_figure_get_frame (figure) > 5;
}

static void
test_a_knob_does_not_rewind_the_film (void)
{
    PnNode *self     = node ("circle 50, 50, value1 * (1 + t)", 1);
    gint    repaints = 0;
    guint   before;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);
    g_object_set (self, "frames", 50, "fps", 60, NULL);
    PN_CHECK (pump_until (past_frame_five, PN_FIGURE (self)));

    /* A scalar input turning -- a knob on a pendulum's length -- is
     * the same fifty-frame film with a new value: the swing carries on
     * instead of snapping back to frame 0 on every step. */
    before = pn_figure_get_frame (PN_FIGURE (self));
    send (self, 0, "value", 3.0);
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, before);
    PN_CHECK (pn_figure_is_playing (PN_FIGURE (self)));

    /* So does an edit that leaves the length alone. */
    g_object_set (self, "program", "circle 50, 50, value1 * (2 - t)", NULL);
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, before);

    g_object_unref (self);
}

static void
test_a_still_does_not_tick_even_when_watched (void)
{
    PnNode *self     = node ("circle 50, 50, value1", 1);
    gint    repaints = 0;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);

    /* Never started when the frame count is 1 (80.17b). */
    send (self, 0, "value", 5.0);
    PN_CHECK (!pn_figure_is_playing (PN_FIGURE (self)));

    g_object_unref (self);
}

static void
test_a_late_watcher_starts_the_film_on_paint (void)
{
    PnNode        *self      = node ("line 0, 0, value1, 0", 1);
    const gdouble  numbers[] = { 10.0, 20.0, 30.0 };
    gint           repaints  = 0;
    GPtrArray     *ops;

    /* Data first, worksheet later -- the order a loaded sheet sees.
     * The painter's first render is what starts the film. */
    send_vector (self, numbers, 3);
    PN_CHECK (!pn_figure_is_playing (PN_FIGURE (self)));

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);
    ops = pn_figure_render (PN_FIGURE (self), 0, 0, 100, 100);
    g_ptr_array_unref (ops);
    PN_CHECK (pn_figure_is_playing (PN_FIGURE (self)));

    /* Disposing a playing figure removes its timer (80.17b); pumping
     * afterwards must not tick into freed memory. */
    g_object_unref (self);
    g_main_context_iteration (NULL, FALSE);
    g_usleep (60 * 1000);
    g_main_context_iteration (NULL, FALSE);
    PN_CHECK (TRUE);
}

static void
test_the_animation_properties_default (void)
{
    PnNode           *self = node (NULL, 1);
    gint              fps = 0, frames = -1;
    PnFigurePlayMode  mode = PN_FIGURE_PLAY_ONCE;

    /* 25 fps, loop, and the length taken from the data (80.17a) --
     * loop because a film that plays once is over before anybody
     * looks at the card (82.4). */
    g_object_get (self, "fps", &fps, "play-mode", &mode,
                  "frames", &frames, NULL);
    PN_CHECK_CMPINT (fps,    ==, 25);
    PN_CHECK_CMPINT (mode,   ==, PN_FIGURE_PLAY_LOOP);
    PN_CHECK_CMPINT (frames, ==, 0);

    g_object_unref (self);
}

static void
test_an_explicit_frame_count_needs_no_input (void)
{
    PnNode *self     = node ("circle 50, 50, 10", 1);
    gint    repaints = 0;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);

    /* The turning wheel of 80.17(e): nothing wired, and still a film
     * that plays -- the property alone starts it for a watcher. */
    g_object_set (self, "frames", 40, NULL);
    PN_CHECK_CMPINT (pn_figure_get_frame_count (PN_FIGURE (self)), ==, 40);
    PN_CHECK (pn_figure_is_playing (PN_FIGURE (self)));

    g_object_set (self, "frames", 0, NULL);
    PN_CHECK_CMPINT (pn_figure_get_frame_count (PN_FIGURE (self)), ==, 1);
    PN_CHECK (!pn_figure_is_playing (PN_FIGURE (self)));

    g_object_unref (self);
}

static void
test_a_once_film_stops_on_its_last_frame (void)
{
    PnNode        *self      = node ("line 0, 0, value1, 0", 1);
    const gdouble  numbers[] = { 10.0, 20.0, 30.0 };
    gint           repaints  = 0;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);
    g_object_set (self, "play-mode", PN_FIGURE_PLAY_ONCE, "fps", 60, NULL);

    send_vector (self, numbers, 3);
    PN_CHECK (pump_until (stopped, PN_FIGURE (self)));
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 2);

    /* A finished film stays finished across a repaint... */
    g_ptr_array_unref (pn_figure_render (PN_FIGURE (self), 0, 0, 100, 100));
    PN_CHECK (!pn_figure_is_playing (PN_FIGURE (self)));

    /* ... until a mode with somewhere to go picks it up where it
     * stands, without rewinding. */
    g_object_set (self, "play-mode", PN_FIGURE_PLAY_LOOP, NULL);
    PN_CHECK (pn_figure_is_playing (PN_FIGURE (self)));
    PN_CHECK_CMPINT (pn_figure_get_frame (PN_FIGURE (self)), ==, 2);

    g_object_unref (self);
}

static void
test_the_animation_properties_round_trip (void)
{
    PnFlow  *flow  = pn_flow_new ();
    PnFlow  *flow2 = pn_flow_new ();
    PnNode  *self  = node ("circle 50, 50, 10", 1);
    PnNode  *loaded;
    GError  *error = NULL;
    gchar   *json;
    gint     fps = 0, frames = 0;
    PnFigurePlayMode mode = PN_FIGURE_PLAY_LOOP;

    /* Plain properties, so the save format picks them up with no code
     * of its own; the mode is saved by its nick. */
    g_object_set (self, "fps", 12, "play-mode", PN_FIGURE_PLAY_PING_PONG,
                  "frames", 90, NULL);
    pn_flow_add_node (flow, self);
    g_object_unref (self);

    json = pn_flow_to_string (flow);
    PN_CHECK (strstr (json, "\"ping-pong\"") != NULL);
    PN_CHECK (pn_flow_load_from_data (flow2, json, &error));

    loaded = pn_node_store_get_node (pn_flow_get_nodes (flow2), 0);
    PN_CHECK (PN_IS_FIGURE (loaded));
    g_object_get (loaded, "fps", &fps, "play-mode", &mode,
                  "frames", &frames, NULL);
    PN_CHECK_CMPINT (fps,    ==, 12);
    PN_CHECK_CMPINT (mode,   ==, PN_FIGURE_PLAY_PING_PONG);
    PN_CHECK_CMPINT (frames, ==, 90);

    g_clear_error (&error);
    g_free (json);
    g_object_unref (flow2);
    g_object_unref (flow);
}

static void
test_a_node_with_no_input_animates_on_t (void)
{
    PnNode *self = node ("line 0, 0, t * 100, 0", 1);
    gchar  *text;

    /* The turning wheel end to end: `frames` alone makes the film, and
     * `t` is what moves in it. */
    g_object_set (self, "frames", 5, "play-mode", PN_FIGURE_PLAY_ONCE, NULL);
    text = pn_figure_dump (PN_FIGURE (self), 4, 0, 0, 100, 100);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 100.00 100.00\n");
    g_free (text);

    /* The mode moves where `t` ends: a loop of five stops at 4/5. */
    g_object_set (self, "play-mode", PN_FIGURE_PLAY_LOOP, NULL);
    text = pn_figure_dump (PN_FIGURE (self), 4, 0, 0, 100, 100);
    PN_CHECK_CMPSTR (text, ==, HEAD_100 "line 0.00 100.00 80.00 100.00\n");
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

/* With 2+ inputs the named input rows stack under the header; the
 * drawing moves down below them instead of painting over them, and it
 * keeps its full height. */
static void
test_the_client_area_clears_the_input_rows (void)
{
    PnNode  *self = node (NULL, 2);
    double   x = -1, y = -1, w = -1, h = -1;
    double   width = 0, height = 0;
    double   ports = pn_node_get_port_section_height (self);

    PN_CHECK      (ports > 0.0);
    pn_node_get_size (self, &width, &height);
    PN_CHECK_NEAR (height, PN_FIGURE_TOTAL_HEIGHT + ports, 1e-9);

    PN_CHECK      (pn_node_get_client_area (self, &x, &y, &w, &h));
    PN_CHECK      (y >= PN_FIGURE_HEADER_HEIGHT + ports);
    PN_CHECK_NEAR (y + h, height, 1e-9);
    PN_CHECK_NEAR (h, PN_FIGURE_CLIENT_HEIGHT, 1e-9);

    g_object_unref (self);
}

/* ------------------------------------------------------------------ */
/*  The evaluated film is kept between frames (#87)                     */
/*                                                                      */
/*  Each check drives one node through a change and then compares its   */
/*  frame with the same frame of a FRESH node built straight into the   */
/*  final state: a kept film that went stale would draw the old one.    */
/* ------------------------------------------------------------------ */

static gchar *
frame_dump (PnNode *self, guint frame)
{
    return pn_figure_dump (PN_FIGURE (self), frame, 0, 0, 100, 100);
}

static guint
count_ops (const gchar *dump, const gchar *op)
{
    gchar       *needle = g_strdup_printf ("\n%s ", op);
    const gchar *at     = dump;
    guint        n      = 0;

    while ((at = strstr (at, needle)) != NULL)
    {
        n++;
        at++;
    }

    g_free (needle);
    return n;
}

static void
test_the_kept_film_follows_new_input (void)
{
    const gchar *program = "line 0, 0, value1 * (1 + frame), 0";
    PnNode      *self    = node (program, 1);
    PnNode      *fresh   = node (program, 1);
    gchar       *before, *after, *expect;

    g_object_set (self,  "frames", 3, NULL);
    g_object_set (fresh, "frames", 3, NULL);

    send (self, 0, "value", 10.0);
    before = frame_dump (self, 1);
    send (self, 0, "value", 20.0);
    after  = frame_dump (self, 1);

    send (fresh, 0, "value", 20.0);
    expect = frame_dump (fresh, 1);

    PN_CHECK_CMPSTR (after, !=, before);
    PN_CHECK_CMPSTR (after, ==, expect);

    g_free (before);
    g_free (after);
    g_free (expect);
    g_object_unref (self);
    g_object_unref (fresh);
}

static void
test_the_kept_film_follows_a_program_edit (void)
{
    PnNode *self  = node ("line 0, 0, 10 * frame, 0", 1);
    PnNode *fresh = node ("line 0, 0, 20 * frame, 0", 1);
    gchar  *before, *after, *expect;

    g_object_set (self,  "frames", 3, NULL);
    g_object_set (fresh, "frames", 3, NULL);

    before = frame_dump (self, 2);
    g_object_set (self, "program", "line 0, 0, 20 * frame, 0", NULL);
    after  = frame_dump (self, 2);
    expect = frame_dump (fresh, 2);

    PN_CHECK_CMPSTR (after, !=, before);
    PN_CHECK_CMPSTR (after, ==, expect);

    g_free (before);
    g_free (after);
    g_free (expect);
    g_object_unref (self);
    g_object_unref (fresh);
}

/* `t` is laid out by the frame count and the play mode (82.5), so
 * changing either one changes every frame's `t`. */
static void
test_the_kept_film_follows_the_film_shape (void)
{
    const gchar *program = "line 0, 0, 100 * t, 0";
    PnNode      *self    = node (program, 1);
    PnNode      *fresh   = node (program, 1);
    gchar       *before, *after, *expect;

    g_object_set (self, "frames", 4, NULL);
    before = frame_dump (self, 1);                 /* t = 1/4 */

    g_object_set (self,  "frames", 2, NULL);
    g_object_set (fresh, "frames", 2, NULL);
    after  = frame_dump (self, 1);                 /* t = 1/2 */
    expect = frame_dump (fresh, 1);
    PN_CHECK_CMPSTR (after, !=, before);
    PN_CHECK_CMPSTR (after, ==, expect);
    g_free (before);
    g_free (expect);

    before = after;
    g_object_set (self,  "play-mode", PN_FIGURE_PLAY_ONCE, NULL);
    g_object_set (fresh, "play-mode", PN_FIGURE_PLAY_ONCE, NULL);
    after  = frame_dump (self, 1);                 /* t = 1 */
    expect = frame_dump (fresh, 1);
    PN_CHECK_CMPSTR (after, !=, before);
    PN_CHECK_CMPSTR (after, ==, expect);

    g_free (before);
    g_free (after);
    g_free (expect);
    g_object_unref (self);
    g_object_unref (fresh);
}

/* A vector repeat count runs a different walk in every frame, so the
 * kept film is only good for the frame it was walked for: asking for
 * the frames out of order must still give each its own pass count. */
static void
test_a_vector_repeat_count_is_walked_per_frame (void)
{
    PnNode *self = node ("repeat 1 + frame\n"
                         "  line i, 0, i, 10\n"
                         "end", 1);
    gchar  *dump;

    g_object_set (self, "frames", 3, NULL);

    dump = frame_dump (self, 2);
    PN_CHECK_CMPINT (count_ops (dump, "line"), ==, 3);
    g_free (dump);

    dump = frame_dump (self, 0);
    PN_CHECK_CMPINT (count_ops (dump, "line"), ==, 1);
    g_free (dump);

    dump = frame_dump (self, 1);
    PN_CHECK_CMPINT (count_ops (dump, "line"), ==, 2);
    g_free (dump);

    g_object_unref (self);
}

/* The same for an `if` whose condition is a comparison of a film: the
 * branch taken differs by frame, so the kept walk is good for one frame
 * only and must be redone when a different one is asked for -- here out
 * of order, so a walk kept from the previous frame would pick the wrong
 * branch every time. */
static void
test_a_film_condition_is_walked_per_frame (void)
{
    PnNode *self = node ("if frame > 1\n"
                         "  point 10, 10\n"
                         "else\n"
                         "  point 20, 20\n"
                         "end", 1);
    const struct
    {
        guint        frame;
        const gchar *point;
    } steps[] = {
        { 3, "point 10.00 90.00 1.00" },
        { 0, "point 20.00 80.00 1.00" },
        { 2, "point 10.00 90.00 1.00" },
        { 1, "point 20.00 80.00 1.00" },
    };
    guint n;

    g_object_set (self, "frames", 4, NULL);

    for (n = 0; n < G_N_ELEMENTS (steps); n++)
    {
        gchar *dump = frame_dump (self, steps[n].frame);
        gchar *want = g_strconcat (HEAD_100, steps[n].point, "\n", NULL);

        PN_CHECK_CMPSTR (dump, ==, want);
        g_free (want);
        g_free (dump);
    }

    g_object_unref (self);
}

/* The kept film does not depend on the rectangle: the zoom overlay
 * draws the same frame bigger from the same walk. */
static void
test_the_kept_film_redraws_at_any_size (void)
{
    const gchar *program = "line 0, 0, 10 * (1 + frame), 50";
    PnNode      *self    = node (program, 1);
    PnNode      *fresh   = node (program, 1);
    gchar       *small, *big, *expect;

    g_object_set (self,  "frames", 3, NULL);
    g_object_set (fresh, "frames", 3, NULL);

    small  = frame_dump (self, 1);
    big    = pn_figure_dump (PN_FIGURE (self),  1, 0, 0, 400, 400);
    expect = pn_figure_dump (PN_FIGURE (fresh), 1, 0, 0, 400, 400);

    PN_CHECK_CMPSTR (big, !=, small);
    PN_CHECK_CMPSTR (big, ==, expect);

    g_free (small);
    g_free (big);
    g_free (expect);
    g_object_unref (self);
    g_object_unref (fresh);
}

static gboolean
has_error (PnFigure *figure)
{
    return pn_node_get_has_error (PN_NODE (figure));
}

static gboolean
has_no_error (PnFigure *figure)
{
    return !pn_node_get_has_error (PN_NODE (figure));
}

static void
test_a_watched_message_is_judged_later (void)
{
    PnNode *self    = node ("line 0, 0, value1, 0", 1);
    gint    repaints = 0;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);

    /* With a painter the walk waits for it: on receive the node is not
     * judged yet, so an empty vector has not reddened it. */
    send_vector (self, NULL, 0);
    PN_CHECK_FALSE (pn_node_get_has_error (self));

    /* A read of the property judges it there and then. */
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==,
                     "line 1, column 12: empty vector argument;"
                     " nothing to draw");
    PN_CHECK (pn_node_get_has_error (self));

    g_object_unref (self);
}

static void
test_an_unpainted_message_is_judged_when_idle (void)
{
    PnNode        *self      = node ("line 0, 0, value1, 0", 1);
    const gdouble  numbers[] = { 10.0 };
    gint           repaints  = 0;

    g_signal_connect (self, "repaint-needed",
                      G_CALLBACK (on_repaint_count), &repaints);

    /* Watched, but nothing ever paints -- a node scrolled out of view.
     * The idle judges it, even behind a throttled repaint: the second
     * message lands inside the throttle window. */
    send_vector (self, numbers, 1);
    send_vector (self, NULL, 0);
    PN_CHECK (pump_until (has_error, PN_FIGURE (self)));

    send_vector (self, numbers, 1);
    PN_CHECK (pump_until (has_no_error, PN_FIGURE (self)));
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");

    g_object_unref (self);
}

/* ------------------------------------------------------------------ */
/*  The field (TODO #94)                                               */
/* ------------------------------------------------------------------ */

/* The one frame of @program in the 100x100 rectangle, from a fresh
 * node, with its error checked empty. */
static gchar *
field_dump (const gchar *program)
{
    PnNode *self = node (program, 1);
    gchar  *text = node_dump (self);

    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==, "");
    g_object_unref (self);
    return text;
}

static void
test_a_field_samples_each_cell_centre (void)
{
    gchar *text = field_dump ("field 0, 0, 100, 100, 2, 2, x / 100\n"
                              "field 0, 0, 100, 100, 2, 2, y / 100");

    /* The centres are at 25 and 75, so 0.25 and 0.75 of full: rows run
     * from the rectangle's y upwards, each from its x, and the corners
     * are the device parallelogram -- corner, x edge end, y edge end. */
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 2x2 40bf/40bf\n"
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 2x2 4040/bfbf\n");
    g_free (text);
}

static void
test_a_field_clamps_and_empties_a_nan (void)
{
    gchar *text = field_dump ("field 0, 0, 100, 100, 4, 1, (x - 37.5) / 25\n"
                              "field 0, 0, 100, 100, 2, 1, sqrt(x - 50)");

    /* -1, 0, 1, 2 clamp into 0..1; a cell with no reading is empty, and
     * is no reason to skip the others (80.10b). */
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 4x1 0000ffff\n"
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 2x1 00ff\n");
    g_free (text);
}

static void
test_a_field_shadows_x_in_its_expression_only (void)
{
    gchar *text = field_dump ("x = 7\n"
                              "field 0, 0, 100, 100, 1, 1, x / 100\n"
                              "circle x, 50, 5");

    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 80\n"
                     "circle 7.00 50.00 5.00\n");
    g_free (text);
}

static void
test_a_field_reads_the_program_and_the_index (void)
{
    gchar *text = field_dump ("a = 0.2\n"
                              "repeat 2\n"
                              "    field i * 50, 0, 50, 100, 1, 1, a + i * 0.5\n"
                              "end\n"
                              "a = 1");

    /* Each pass's `i`, and `a` as it stood at the statement -- not the
     * value the program gives it afterwards. */
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 50.00 100.00 0.00 0.00 1x1 33\n"
                     "field 50.00 100.00 100.00 100.00 50.00 0.00 1x1 b3\n");
    g_free (text);
}

static void
test_a_field_is_not_an_input (void)
{
    Figure f = figure ("field 0, 0, 1, 1, 2, 2, x * y * k\n"
                       "point x, 0");

    /* The cell's x and y are the field's; the `point`'s x is the
     * program's, and k is the program's everywhere. */
    PN_CHECK_CMPINT (f.parse.errors->len, ==, 0);
    PN_CHECK_CMPINT (f.names->len, ==, 2);
    PN_CHECK_CMPSTR (g_ptr_array_index (f.names, 0), ==, "k");
    PN_CHECK_CMPSTR (g_ptr_array_index (f.names, 1), ==, "x");
    figure_free (&f);
}

static void
test_a_field_animates (void)
{
    PnNode *self = node ("field 0, 0, 100, 100, 1, 1, t", 1);
    gchar  *one, *two, *again;

    g_object_set (self, "frames", 4, "play-mode", PN_FIGURE_PLAY_LOOP, NULL);

    one   = frame_dump (self, 1);
    two   = frame_dump (self, 2);
    again = frame_dump (self, 1);

    PN_CHECK_CMPSTR (one, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 40\n");
    PN_CHECK_CMPSTR (two, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 80\n");
    /* The kept cells are the same cells. */
    PN_CHECK_CMPSTR (again, ==, one);

    g_free (one);
    g_free (two);
    g_free (again);
    g_object_unref (self);
}

static void
test_a_field_follows_new_input (void)
{
    PnNode *self = node ("field 0, 0, 100, 100, 1, 1, value1", 1);
    gchar  *before, *after;

    send (self, 0, "value", 0.5);
    before = node_dump (self);
    send (self, 0, "value", 1.0);
    after  = node_dump (self);

    PN_CHECK_CMPSTR (before, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 80\n");
    PN_CHECK_CMPSTR (after, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 ff\n");

    g_free (before);
    g_free (after);
    g_object_unref (self);
}

static void
test_a_field_turns_with_its_axes (void)
{
    gchar *text = field_dump ("origin 50, 50, 90\n"
                              "    field 0, 0, 10, 20, 1, 1, 1\n"
                              "end");

    /* Local x points up the card and local y to its left. */
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 50.00 50.00 50.00 40.00 30.00 50.00 1x1 ff\n");
    g_free (text);
}

static void
test_a_field_skips_what_it_cannot_draw (void)
{
    gchar *text = field_dump ("field 0, 0, 0, 100, 2, 2, x\n"
                              "field 0, 0, 100, 100, 0, 2, x\n"
                              "field 0, 0, 100, 100, 1000, 1000, x\n"
                              "field 0, 0, 100, 100, 2, 2 / 0, x");

    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "# skip 1 degenerate\n"
                     "# skip 2 degenerate\n"
                     "# skip 3 too-many\n"
                     "# skip 4 non-finite\n");
    g_free (text);
}

static void
test_a_field_mean_for_a_big_grid (void)
{
    gchar *text = field_dump ("field 0, 0, 100, 100, 10, 10, 1");

    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 10x10"
                     " mean 255.00\n");
    g_free (text);
}

static void
test_a_field_takes_seven_arguments (void)
{
    PnNode *self = node ("field 0, 0, 100, 100, 2, 2", 1);

    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==,
                     "line 1, column 1: field takes 7 arguments, not 6");
    g_object_unref (self);
}

static void
test_a_field_in_a_shape (void)
{
    Split  ok  = parsed ("def blob r\n"
                         "    field -r, -r, 2 * r, 2 * r, 2, 2,"
                         " x * x + y * y < r * r\n"
                         "end\n"
                         "blob 10");
    Split  bad = parsed ("def blob r\n"
                         "    field -r, -r, 2 * r, 2 * r, 2, 2, k\n"
                         "end\n"
                         "blob 10");

    /* Inside a shape the cell's own x and y are the shape's to read, as
     * `i` is inside its own loop; anything else still comes in as a
     * parameter. */
    PN_CHECK_CMPINT (ok.errors->len, ==, 0);
    PN_CHECK_CMPINT (bad.errors->len, ==, 1);
    PN_CHECK_CMPSTR (error_text (&bad, 0), ==,
                     "shape \"blob\" cannot read \"k\": pass it in as a "
                     "parameter");

    split_free (&ok);
    split_free (&bad);
}

static void
test_a_field_with_a_vector_past_its_end (void)
{
    PnNode        *self      = node ("field 0, 0, 100, 100, 1, 1, value1", 1);
    const gdouble  numbers[] = { 0.25, 0.5 };
    gchar         *text;

    send_vector (self, numbers, 2);
    text = frame_dump (self, 1);
    PN_CHECK_CMPSTR (text, ==,
                     HEAD_100
                     "field 0.00 100.00 100.00 100.00 0.00 0.00 1x1 80\n");
    g_free (text);

    /* A frame the vector has no element for stops the frame, reported
     * at the cell expression, as it would be at any argument. */
    text = frame_dump (self, 2);
    PN_CHECK_CMPSTR (text, ==, "");
    PN_CHECK_CMPSTR (pn_figure_get_error (PN_FIGURE (self)), ==,
                     "line 1, column 29: frame 2 is past the end of a "
                     "2-element vector");
    g_free (text);
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
    pn_test_add ("film_indexes",        test_a_vector_argument_is_a_film);
    pn_test_add ("film_assignment",     test_a_vector_survives_assignment_and_arithmetic);
    pn_test_add ("film_pen_state",      test_a_vector_pen_state_animates);
    pn_test_add ("film_repeat_count",   test_a_vector_repeat_count_animates);
    pn_test_add ("film_past_the_end",   test_a_frame_past_the_end_is_an_error);
    pn_test_add ("film_empty_argument", test_an_empty_vector_argument_is_an_error);
    pn_test_add ("film_still",          test_a_still_figure_is_one_frame);
    pn_test_add ("film_shortest",       test_the_shortest_vector_sets_the_frame_count);
    pn_test_add ("film_unread",         test_an_unread_vector_does_not_count);
    pn_test_add ("film_explicit",       test_an_explicit_count_joins_the_minimum);
    pn_test_add ("film_empty",          test_an_empty_vector_leaves_no_frame);
    pn_test_add ("film_frame_and_t",    test_frame_and_t_are_bound_as_the_film);
    pn_test_add ("film_t_ends_on_one",  test_t_ends_on_one_when_the_film_ends);
    pn_test_add ("film_t_from_data",    test_t_follows_a_film_from_the_data);
    pn_test_add ("film_still_t",        test_a_still_reads_t_as_zero);
    pn_test_add ("film_input_t_wins",   test_an_input_named_t_wins);
    pn_test_add ("film_assign_t_wins",  test_an_assignment_to_t_still_wins);
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
    pn_test_add ("block_stray_end",     test_an_end_without_a_block_is_a_parse_error);
    pn_test_add ("block_no_nested_rep", test_a_repeat_does_not_nest_in_a_repeat);
    pn_test_add ("block_error_draws_nothing", test_a_structural_error_draws_nothing);
    pn_test_add ("if_runs_when_true",   test_an_if_runs_its_body_only_when_true);
    pn_test_add ("if_no_op",            test_an_if_leaves_no_operation_of_its_own);
    pn_test_add ("if_first_true_wins",  test_the_first_true_clause_wins);
    pn_test_add ("if_not_a_scope",      test_an_if_is_not_a_scope);
    pn_test_add ("if_condition_value",  test_a_condition_is_a_value_not_a_program);
    pn_test_add ("if_nests",            test_an_if_nests_inside_anything);
    pn_test_add ("if_vector_animates",  test_a_vector_condition_animates);
    pn_test_add ("if_film_comparison",  test_a_comparison_of_a_film_chooses_per_frame);
    pn_test_add ("if_folding",          test_a_condition_folds_like_any_argument);
    pn_test_add ("if_arities",          test_the_if_verbs_have_arities);
    pn_test_add ("if_misplaced",        test_misplaced_clauses_are_parse_errors);
    pn_test_add ("if_no_nested_repeat", test_an_if_does_not_launder_a_nested_repeat);
    pn_test_add ("if_unchecked_safe",   test_an_unchecked_program_cannot_run_away);
    pn_test_add ("with_one_statement",  test_a_trailing_with_scopes_one_statement);
    pn_test_add ("with_every_spelling", test_a_with_list_takes_every_spelling);
    pn_test_add ("with_only_changes",   test_only_what_changed_is_given_back);
    pn_test_add ("with_block_nests",    test_a_with_block_nests);
    pn_test_add ("with_position_view",  test_the_pen_position_and_the_view_carry_on);
    pn_test_add ("with_in_a_loop",      test_a_with_inside_a_loop_scopes_each_pass);
    pn_test_add ("with_film",           test_a_with_value_that_is_a_film_gives_back_per_frame);
    pn_test_add ("with_bad_setting",    test_a_setting_that_cannot_be_drawn_is_skipped);
    pn_test_add ("with_keyword",        test_with_is_a_keyword_only_outside_strings);
    pn_test_add ("with_errors",         test_a_bad_with_list_is_located);
    pn_test_add ("origin_shifts",       test_an_origin_shifts_what_it_holds);
    pn_test_add ("origin_turns",        test_an_origin_turns_its_axes);
    pn_test_add ("origin_tilted_rect",  test_a_tilted_rect_is_a_polygon);
    pn_test_add ("origin_round_shapes", test_circles_arcs_and_labels_under_turned_axes);
    pn_test_add ("origin_nests",        test_origins_nest);
    pn_test_add ("origin_pen_position", test_the_pen_leaves_an_origin_where_it_was);
    pn_test_add ("origin_no_pen_scope", test_an_origin_is_not_a_pen_scope);
    pn_test_add ("origin_in_a_loop",    test_an_origin_inside_a_loop);
    pn_test_add ("origin_non_finite",   test_an_origin_that_cannot_be_placed_skips_the_block);
    pn_test_add ("origin_animates",     test_turning_axes_animate_without_a_walk_per_frame);
    pn_test_add ("origin_errors",       test_a_bad_origin_is_located);
    pn_test_add ("def_calls",           test_a_shape_draws_where_it_is_called);
    pn_test_add ("def_no_parameters",   test_a_shape_may_take_nothing);
    pn_test_add ("def_own_store",       test_a_shape_has_a_store_of_its_own);
    pn_test_add ("def_pen_scope",       test_a_call_gives_the_pen_back);
    pn_test_add ("def_trailing_with",   test_a_call_takes_a_trailing_with);
    pn_test_add ("def_nested_calls",    test_shapes_use_the_shapes_above_them);
    pn_test_add ("def_loop_in_a_loop",  test_a_shape_loops_inside_a_loop);
    pn_test_add ("def_step_bound",      test_a_runaway_walk_empties_the_figure);
    pn_test_add ("def_animates",        test_a_vector_argument_animates_the_shape);
    pn_test_add ("def_compares_frames", test_a_comparison_in_a_shape_is_per_frame);
    pn_test_add ("def_reads",           test_a_shape_reads_only_what_it_is_given);
    pn_test_add ("def_errors",          test_a_bad_def_is_located);
    pn_test_add ("arrow_draws",         test_an_arrow_ends_in_a_solid_head);
    pn_test_add ("arrow_solid_head",    test_an_arrowhead_is_never_dashed_or_hollow);
    pn_test_add ("arrow_short",         test_a_short_arrow_shrinks_its_head);
    pn_test_add ("arrow_head_size",     test_the_arrowhead_setting_sizes_the_head);
    pn_test_add ("arrow_degenerate",    test_an_arrow_with_no_direction_is_skipped);
    pn_test_add ("arrow_origin",        test_an_arrow_turns_with_its_axes_and_leaves_the_pen);
    pn_test_add ("arrow_with",          test_a_trailing_with_scopes_the_whole_arrow);
    pn_test_add ("arrow_film",          test_an_arrow_in_a_film_is_sized_per_frame);
    pn_test_add ("arrow_stretch",       test_an_arrow_stretches_with_the_view);
    pn_test_add ("hatch_draws",         test_a_hatch_strokes_its_left_side);
    pn_test_add ("hatch_other_side",    test_a_negative_depth_hatches_the_other_side);
    pn_test_add ("hatch_origin",        test_a_hatch_turns_with_its_axes);
    pn_test_add ("hatch_degenerate",    test_a_hatch_that_cannot_be_drawn_is_skipped);
    pn_test_add ("hatch_with",          test_a_trailing_with_scopes_the_whole_hatch);
    pn_test_add ("hatch_film",          test_a_hatch_in_a_film_counts_its_strokes_per_frame);
    pn_test_add ("dimension_draws",     test_a_dimension_ticks_both_ends);
    pn_test_add ("dimension_tick",      test_a_dimension_tick_is_3_when_left_out);
    pn_test_add ("dimension_origin",    test_a_dimension_turns_with_its_axes_and_leaves_the_pen);
    pn_test_add ("dimension_degenerate", test_a_dimension_that_cannot_be_drawn_is_skipped);
    pn_test_add ("anglemark_draws",     test_an_anglemark_labels_its_bisector);
    pn_test_add ("anglemark_no_label",  test_an_anglemark_label_is_optional);
    pn_test_add ("anglemark_sides",     test_an_anglemark_label_picks_its_side);
    pn_test_add ("anglemark_origin",    test_an_anglemark_turns_with_its_axes);
    pn_test_add ("anglemark_degenerate", test_an_anglemark_that_cannot_be_drawn_is_skipped);
    pn_test_add ("anglemark_format",    test_an_anglemark_label_is_a_format);
    pn_test_add ("anglemark_film",      test_an_anglemark_in_a_film_follows_its_angle);
    pn_test_add ("anglemark_def",       test_an_anglemark_in_a_shape);
    pn_test_add ("anglemark_mirror",    test_an_anglemark_label_follows_a_mirror);
    pn_test_add ("curve_draws",         test_a_curve_keeps_its_control_points);
    pn_test_add ("curve_closed",        test_a_curve_that_comes_back_is_closed);
    pn_test_add ("curve_arity",         test_a_curve_counts_its_points);
    pn_test_add ("curve_stretch",       test_a_curve_stretches_with_the_view);
    pn_test_add ("curve_with",          test_a_trailing_with_fills_a_closed_curve);
    pn_test_add ("axes_draw",           test_axes_draw_two_arrows_and_name_them);
    pn_test_add ("axes_reversed",       test_axes_may_point_the_other_way);
    pn_test_add ("axes_degenerate",     test_axes_with_no_length_are_skipped);
    pn_test_add ("axes_origin",         test_axes_turn_with_an_origin);
    pn_test_add ("axes_def",            test_axes_in_a_shape_take_the_pen_of_the_call);
    pn_test_add ("angle_turns_text",    test_the_angle_setting_turns_a_label);
    pn_test_add ("angle_origin",        test_a_label_angle_ignores_the_axes);
    pn_test_add ("angle_stretch",       test_a_label_angle_follows_a_stretch_not_a_mirror);
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
    pn_test_add ("node_vector_input",   test_a_vector_input_is_a_film);
    pn_test_add ("timer_play_modes",    test_the_play_modes_step);
    pn_test_add ("timer_past_the_end",  test_a_frame_past_the_end_steps_from_inside);
    pn_test_add ("timer_headless",      test_a_headless_film_does_not_tick);
    pn_test_add ("timer_watched",       test_a_watched_film_plays_and_stops_when_unwatched);
    pn_test_add ("timer_knob",          test_a_knob_does_not_rewind_the_film);
    pn_test_add ("timer_still",         test_a_still_does_not_tick_even_when_watched);
    pn_test_add ("timer_late_watcher",  test_a_late_watcher_starts_the_film_on_paint);
    pn_test_add ("anim_defaults",       test_the_animation_properties_default);
    pn_test_add ("anim_frames",         test_an_explicit_frame_count_needs_no_input);
    pn_test_add ("anim_once",           test_a_once_film_stops_on_its_last_frame);
    pn_test_add ("anim_round_trip",     test_the_animation_properties_round_trip);
    pn_test_add ("anim_node_t",         test_a_node_with_no_input_animates_on_t);
    pn_test_add ("node_error_property", test_the_error_property_reads_back);
    pn_test_add ("node_client_area",    test_the_client_area_is_the_body);
    pn_test_add ("node_client_inputs",  test_the_client_area_clears_the_input_rows);
    pn_test_add ("kept_film_input",     test_the_kept_film_follows_new_input);
    pn_test_add ("kept_film_program",   test_the_kept_film_follows_a_program_edit);
    pn_test_add ("kept_film_shape",     test_the_kept_film_follows_the_film_shape);
    pn_test_add ("kept_film_repeat",    test_a_vector_repeat_count_is_walked_per_frame);
    pn_test_add ("kept_film_if",        test_a_film_condition_is_walked_per_frame);
    pn_test_add ("kept_film_any_size",  test_the_kept_film_redraws_at_any_size);
    pn_test_add ("judge_on_read",       test_a_watched_message_is_judged_later);
    pn_test_add ("judge_when_idle",     test_an_unpainted_message_is_judged_when_idle);
    pn_test_add ("field_cell_centres",  test_a_field_samples_each_cell_centre);
    pn_test_add ("field_clamp_nan",     test_a_field_clamps_and_empties_a_nan);
    pn_test_add ("field_shadows_x",     test_a_field_shadows_x_in_its_expression_only);
    pn_test_add ("field_reads_program", test_a_field_reads_the_program_and_the_index);
    pn_test_add ("field_not_an_input",  test_a_field_is_not_an_input);
    pn_test_add ("field_animates",      test_a_field_animates);
    pn_test_add ("field_new_input",     test_a_field_follows_new_input);
    pn_test_add ("field_turns",         test_a_field_turns_with_its_axes);
    pn_test_add ("field_skips",         test_a_field_skips_what_it_cannot_draw);
    pn_test_add ("field_big_grid",      test_a_field_mean_for_a_big_grid);
    pn_test_add ("field_arity",         test_a_field_takes_seven_arguments);
    pn_test_add ("field_in_a_shape",    test_a_field_in_a_shape);
    pn_test_add ("field_vector_end",    test_a_field_with_a_vector_past_its_end);
    return pn_test_run ();
}
