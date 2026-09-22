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

#ifndef PN_FIGURE_H
#define PN_FIGURE_H

#include <glib.h>

#include "pn-color.h"
#include "pn-expr-parser.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFigureError                                                      */
/*                                                                     */
/*  One program error, with the place in the SOURCE text that caused   */
/*  it.  Deliberately not a #GError domain: the figure language parses */
/*  the whole program and collects every error it finds, then shows    */
/*  the first one with a count ("3 errors, first on line 7") in the    */
/*  node's client area — the only channel this node has.  A #GError    */
/*  carries one failure and aborts at it, which is the opposite.       */
/*                                                                     */
/*  @line and @column are 1-based, and @column counts CHARACTERS, not  */
/*  bytes, so a message points at the right place in an editor after   */
/*  a non-ASCII string literal earlier on the line.                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint   line;
    gint   column;
    gchar *message; /* owned, human-readable, no line/column in it */
} PnFigureError;

/**
 * pn_figure_error_free:
 * @self: (nullable) (transfer full): an error, or %NULL
 *
 * Frees @self and its message.  Safe to call with %NULL.
 */
void pn_figure_error_free (PnFigureError *self);

/**
 * pn_figure_errors_new:
 *
 * Returns: (transfer full) (element-type PnFigureError): an empty error
 *   list with the right element free function, ready to hand to
 *   pn_figure_scan() and the parse stages after it.
 */
GPtrArray *pn_figure_errors_new (void);

/* ------------------------------------------------------------------ */
/*  The line scanner                                                   */
/*                                                                     */
/*  First stage of the figure language's front end: raw program text   */
/*  in, LOGICAL lines out.  It strips comments (knowing that a "#"     */
/*  inside a string is a character, not a comment), drops blank and    */
/*  comment-only lines, trims the free leading and trailing            */
/*  whitespace, and joins a line whose last code character is a comma  */
/*  to the line that follows it.                                       */
/*                                                                     */
/*  That joining is why a logical line is more than a string: an error */
/*  in the tail of a continued `poly` must still name the source line  */
/*  the tail was typed on.  So each logical line also carries the      */
/*  PIECES it was assembled from, and pn_figure_line_locate() maps any */
/*  offset in the joined text back to a source line and column.        */
/*                                                                     */
/*  Pieces are joined with no separator, which is unambiguous because  */
/*  a join only ever happens directly after a comma.                   */
/* ------------------------------------------------------------------ */

typedef struct
{
    gsize offset; /* byte offset in PnFigureLine.text where it starts  */
    gint  line;   /* 1-based source line the piece was typed on        */
    gint  column; /* 1-based source column of text[offset]             */
} PnFigurePiece;

typedef struct
{
    gchar  *text;   /* the joined, comment-free, trimmed logical line  */
    gint    line;   /* 1-based source line it starts on                */
    GArray *pieces; /* #PnFigurePiece, at least one, ascending offset  */
} PnFigureLine;

/**
 * pn_figure_line_free:
 * @self: (nullable) (transfer full): a logical line, or %NULL
 *
 * Frees @self, its text and its piece table.  Safe to call with %NULL.
 */
void pn_figure_line_free (PnFigureLine *self);

/**
 * pn_figure_line_locate:
 * @self:       the logical line
 * @offset:     byte offset into @self->text; clamped to its length
 * @out_line:   (out) (optional): 1-based source line
 * @out_column: (out) (optional): 1-based source column, in characters
 *
 * Maps a position in the joined text back to where it came from, which
 * for an uncontinued line is simply the line it was typed on, and for a
 * continued one is whichever piece covers @offset.
 */
void pn_figure_line_locate (const PnFigureLine *self,
                            gsize               offset,
                            gint               *out_line,
                            gint               *out_column);

/**
 * pn_figure_scan:
 * @program: (nullable): the program text, as typed
 * @errors:  (nullable) (element-type PnFigureError): collector to
 *           append to, from pn_figure_errors_new()
 *
 * Splits @program into logical lines.  Blank lines, comment-only lines
 * and the comment tail of a code line disappear; a line ending in a
 * comma swallows the line after it, and keeps swallowing across
 * comment-only lines, so a long `poly` may be commented from the
 * inside.  A truly blank line ends a continuation rather than
 * swallowing it, leaving the dangling comma in the text for the
 * statement splitter to complain about — the mistake is worth a
 * message, and joining the next statement onto it would hide it.
 *
 * The one error the scanner can report itself is an unterminated
 * string, since it has to track quotes to find comments at all.  Such a
 * line is dropped rather than handed on, so no later stage re-reports
 * the same broken quoting; a program with any error draws nothing
 * anyway.
 *
 * An empty program is an empty list, not an error.
 *
 * Returns: (transfer full) (element-type PnFigureLine): the logical
 *   lines, in order.
 */
GPtrArray *pn_figure_scan (const gchar *program,
                           GPtrArray   *errors);

/* ------------------------------------------------------------------ */
/*  The statement splitter                                             */
/*                                                                     */
/*  Second stage: one logical line in, one statement out.  It decides  */
/*  verb-or-assignment on a single lookahead (80.2 rule 1), splits a   */
/*  verb's arguments on the commas that are at paren depth 0 and       */
/*  outside quotes (rule 3), and resolves the escapes in a quoted      */
/*  string.  It does NOT know which verbs exist or how many arguments  */
/*  they take — that is the verb table's business — and it does not    */
/*  parse an expression, only delimit one.                             */
/*                                                                     */
/*  Quoted means literal and unquoted means expression, in every       */
/*  argument position (rule 7), so this is where an argument's KIND is */
/*  settled once and for all.                                          */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_FIGURE_ARG_EXPRESSION, /* unquoted: text for the calculator     */
    PN_FIGURE_ARG_STRING,     /* quoted: the contents, escapes resolved */
} PnFigureArgKind;

typedef struct
{
    PnFigureArgKind  kind;
    gchar           *text;   /* owned: the fragment, or the contents   */
    gsize            offset; /* where it starts in PnFigureLine.text   */

    /* Filled in by pn_figure_parse_literals(), for the string
     * arguments whose verb gives them a meaning: a colour, or the
     * dash / alignment word this one names, or — for a format string —
     * how many values it wants. */
    PnColor          color;
    gint             word;

    /* Filled in by pn_figure_parse_expressions(), for an expression
     * argument: either a tree to evaluate every frame, or — when the
     * argument names nothing that can change — the number it always
     * comes to.  Most of a figure is the second kind. */
    PnExprNode      *ast;    /* owned, %NULL when folded or a string  */
    gdouble          value;  /* the constant, when @folded            */
    gboolean         folded;
} PnFigureArg;

typedef enum
{
    PN_FIGURE_STATEMENT_VERB,
    PN_FIGURE_STATEMENT_ASSIGNMENT,
} PnFigureStatementKind;

/* Every verb the language has, which is every verb 80.5, 80.6 and 80.7
 * settled and not one more.  PN_FIGURE_VERB_NONE is what an assignment
 * carries, and what a verb statement carries until
 * pn_figure_check_verbs() has looked at it. */
typedef enum
{
    PN_FIGURE_VERB_NONE = 0,

    /* pen state (80.5), plus the window (80.2 rule 11) */
    PN_FIGURE_VERB_VIEW,
    PN_FIGURE_VERB_COLOR,
    PN_FIGURE_VERB_FILL,
    PN_FIGURE_VERB_NOFILL,
    PN_FIGURE_VERB_WIDTH,
    PN_FIGURE_VERB_DASH,
    PN_FIGURE_VERB_FONT,
    PN_FIGURE_VERB_ALIGN,

    /* geometry (80.6) */
    PN_FIGURE_VERB_MOVE,
    PN_FIGURE_VERB_RMOVE,
    PN_FIGURE_VERB_LINETO,
    PN_FIGURE_VERB_RLINE,
    PN_FIGURE_VERB_LINE,
    PN_FIGURE_VERB_POINT,
    PN_FIGURE_VERB_CIRCLE,
    PN_FIGURE_VERB_ARC,
    PN_FIGURE_VERB_RECT,
    PN_FIGURE_VERB_POLY,
    PN_FIGURE_VERB_PATH,

    /* text (80.7) */
    PN_FIGURE_VERB_TEXT,
} PnFigureVerb;

/* A statement borrows the logical line it came from, for
 * pn_figure_line_locate(): the line list must outlive the statement
 * list, which it does for the whole of a parse.
 *
 * An assignment carries no arguments — rule 1 hands the WHOLE line,
 * @source->text, to pn_expr_parser_parse(), which already understands
 * `name = expr`.  Its @name is the target it binds, kept exactly as
 * typed because variable names are case-sensitive; a verb's @name is
 * folded to lower case because verbs are not (rule 2).
 *
 * The verb itself always starts at offset 0 of @source->text, the
 * scanner having trimmed the line, so an error about the verb — an
 * unknown one, or the wrong number of arguments — locates there. */
typedef struct
{
    PnFigureStatementKind  kind;
    PnFigureVerb           verb;   /* filled in by pn_figure_check_verbs() */
    gchar                 *name;   /* owned: verb, or assignment target */
    GPtrArray             *args;   /* #PnFigureArg, empty for an assignment */
    const PnFigureLine    *source; /* borrowed                          */
    PnExprNode            *ast;    /* owned: an assignment's whole line */
} PnFigureStatement;

/**
 * pn_figure_statement_free:
 * @self: (nullable) (transfer full): a statement, or %NULL
 *
 * Frees @self, its name and its arguments.  Safe to call with %NULL.
 */
void pn_figure_statement_free (PnFigureStatement *self);

/**
 * pn_figure_split:
 * @lines:  (element-type PnFigureLine): logical lines from
 *          pn_figure_scan(), which must outlive the result
 * @errors: (nullable) (element-type PnFigureError): collector
 *
 * Splits each logical line into a statement.  The errors it can report
 * are a line that does not begin with an identifier, an empty argument
 * — which is what a comma with nothing after it comes to — text after
 * a closing quote, and an undefined escape.  The escapes the language
 * defines are `\"`, `\\` and `\n`, the last because 80.7(d) makes a
 * newline split a label into lines; anything else is a mistake worth
 * saying out loud rather than drawing as a backslash.
 *
 * A line that fails is left out of the result and the scan goes on, so
 * one broken line does not hide the errors on the next (80.2 rule 9).
 *
 * Returns: (transfer full) (element-type PnFigureStatement): the
 *   statements, in order.
 */
GPtrArray *pn_figure_split (GPtrArray *lines,
                            GPtrArray *errors);

/* ------------------------------------------------------------------ */
/*  The verb table                                                     */
/*                                                                     */
/*  Third stage: the only thing in the front end that knows what the   */
/*  language can draw.  Each verb has a name, an arity and a kind per  */
/*  argument position, and a statement is measured against its row.    */
/*                                                                     */
/*  Two shapes do not fit a plain count, and both were decided rather  */
/*  than discovered: `poly` and `path` are variadic in PAIRS, at least */
/*  three points (80.2 rule 8, 80.6g); and a COLOUR argument has two   */
/*  spellings — one quoted literal, or three-to-four expressions for   */
/*  r, g, b and an optional a — which is the one two-form arity in the */
/*  language (80.5).  `text` is variadic too, but plainly so: two      */
/*  coordinates, a format string, and one expression per conversion.   */
/*                                                                     */
/*  What a literal MEANS is still nobody's business here: an unknown   */
/*  colour name or dash style is 80.22.4's error, not this stage's.    */
/* ------------------------------------------------------------------ */

/**
 * pn_figure_check_verbs:
 * @statements: (element-type PnFigureStatement): statements from
 *              pn_figure_split(), checked and annotated in place
 * @errors:     (nullable) (element-type PnFigureError): collector
 *
 * Looks each verb statement up in the verb table and fills in its
 * #PnFigureStatement.verb, checking the argument count and the kind of
 * every argument on the way.  Assignments are passed over untouched.
 *
 * A statement that does not check out is REMOVED from @statements,
 * since nothing after this stage should have to ask whether a verb is
 * real; the rest are still checked, so one typo does not hide the next
 * (80.2 rule 9).
 *
 * Returns: %TRUE when every statement checked out.
 */
gboolean pn_figure_check_verbs (GPtrArray *statements,
                                GPtrArray *errors);

/* ------------------------------------------------------------------ */
/*  Literals                                                           */
/*                                                                     */
/*  Fourth stage, and the last one that can be done without evaluating */
/*  anything: what the quoted literals MEAN.  Quoted means literal     */
/*  (80.2 rule 7), so a colour, a dash style, an alignment word and a  */
/*  `text` format are all known while the program is being read — and  */
/*  are therefore all errors with a line and a column, rather than     */
/*  surprises discovered mid-frame.  Runtime colour errors simply do   */
/*  not exist (80.5h).                                                 */
/*                                                                     */
/*  The format check is safety, not tidiness: handing a user-typed     */
/*  format to printf with double arguments is how "%s" dereferences a  */
/*  double.  Only "%%" and the numeric conversions get through, with   */
/*  their count matched against the values that follow (80.7c).        */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_FIGURE_DASH_SOLID = 0,
    PN_FIGURE_DASH_DOT,
    PN_FIGURE_DASH_DASH,
    PN_FIGURE_DASH_DASHDOT,
} PnFigureDash;

typedef enum
{
    PN_FIGURE_HALIGN_LEFT = 0,
    PN_FIGURE_HALIGN_CENTRE,
    PN_FIGURE_HALIGN_RIGHT,
} PnFigureHAlign;

typedef enum
{
    PN_FIGURE_VALIGN_TOP = 0,
    PN_FIGURE_VALIGN_MIDDLE,
    PN_FIGURE_VALIGN_BASELINE,
    PN_FIGURE_VALIGN_BOTTOM,
} PnFigureVAlign;

/**
 * pn_figure_parse_literals:
 * @statements: (element-type PnFigureStatement): statements already
 *              through pn_figure_check_verbs(), annotated in place
 * @errors:     (nullable) (element-type PnFigureError): collector
 *
 * Gives every quoted literal its meaning: a colour through
 * pn_color_parse(), a dash style, an alignment word, and a `text`
 * format validated conversion by conversion with its count matched
 * against the expressions that follow it.  The result lands in
 * #PnFigureArg.color or #PnFigureArg.word, the latter holding the
 * conversion count for a format string.
 *
 * As in the stage before, a statement that fails is removed and the
 * rest are still read, so one bad literal does not hide the next.
 *
 * Returns: %TRUE when every literal made sense.
 */
gboolean pn_figure_parse_literals (GPtrArray *statements,
                                   GPtrArray *errors);

/* ------------------------------------------------------------------ */
/*  Expressions                                                        */
/*                                                                     */
/*  Fifth stage: every unquoted argument becomes an AST, through the   */
/*  calculator's own parser and nothing else (80.3).  An assignment    */
/*  line goes to that parser WHOLE, since it already understands       */
/*  `name = expr` and already returns the ASSIGN node the evaluator    */
/*  wants (80.2).                                                      */
/*                                                                     */
/*  Two things fall out of the same walk.  An argument that names      */
/*  nothing which can change is evaluated once, here, and kept as a    */
/*  number: most of a figure is its `view`, its widths and its fixed   */
/*  coordinates, so the per-frame work shrinks to the handful of       */
/*  arguments that actually move (80.3b).  And what IS named is        */
/*  collected, because an unbound name is an evaluation FAILURE in     */
/*  PnVarStore rather than a zero — so an unwired figure only draws at */
/*  all if someone binds those names to 0 first (80.2 rule 12).        */
/*                                                                     */
/*  `pi` and `e` are the language's own constants, not names a program */
/*  expects from outside: they fold like numbers and never appear in   */
/*  the collected list.  They live here only until #81 gives PnVarStore */
/*  constants of its own (80.3d).                                      */
/* ------------------------------------------------------------------ */

/**
 * pn_figure_parse_expressions:
 * @statements: (element-type PnFigureStatement): statements already
 *              through pn_figure_parse_literals(), filled in in place
 * @errors:     (nullable) (element-type PnFigureError): collector
 *
 * Parses every expression argument and every assignment line, folding
 * the arguments that cannot change into constants.  A parse failure is
 * reported at the character it happened at, not merely at the argument:
 * the calculator's messages carry a position within the fragment, which
 * this stage folds into the column and leaves out of the text.
 *
 * A statement that fails is removed, the rest still parsed.
 *
 * Returns: %TRUE when everything parsed.
 */
gboolean pn_figure_parse_expressions (GPtrArray *statements,
                                      GPtrArray *errors);

/**
 * pn_figure_free_names:
 * @statements: (element-type PnFigureStatement): parsed statements
 *
 * Every variable name the program READS, sorted and without repeats —
 * which is what has to be bound before a frame runs, whether from an
 * input, from an assignment the program makes on the way, or from the
 * zero-fill that keeps an unwired figure drawing.
 *
 * A name the program assigns is included when the program also reads
 * it, deliberately: the read may come FIRST, and a figure that draws
 * has to survive that too.
 *
 * Returns: (transfer full) (element-type utf8): the names.
 */
GPtrArray *pn_figure_free_names (GPtrArray *statements);

/* ------------------------------------------------------------------ */
/*  Reporting                                                          */
/* ------------------------------------------------------------------ */

/**
 * pn_figure_errors_to_string:
 * @errors: (nullable) (element-type PnFigureError): everything the
 *          front end collected
 *
 * Renders the collected errors the way the client area shows them: the
 * EARLIEST one in the program, with its line and column, and — when
 * there are several — a second line saying how many there were (80.2
 * rule 9, 80.10a).  Earliest by position, not by the order the stages
 * happened to find them in, because a person reads their program top
 * to bottom.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when there
 *   are no errors at all.
 */
gchar *pn_figure_errors_to_string (GPtrArray *errors);

G_END_DECLS

#endif /* PN_FIGURE_H */
