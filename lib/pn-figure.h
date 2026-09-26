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
#include "pn-node.h"
#include "pn-var-store.h"

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

    /* the one block the language has (80.18a, TODO #86) */
    PN_FIGURE_VERB_REPEAT,
    PN_FIGURE_VERB_END,
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
/*  Blocks                                                             */
/*                                                                     */
/*  A stage of its own between the verb table and the literals, and    */
/*  the only one that looks at a statement's NEIGHBOURS: `repeat` and  */
/*  `end` are ordinary verbs to every other stage, and it is here that */
/*  they are read as a pair (TODO #86.3).                              */
/*                                                                     */
/*  It runs after the verb table because the structure cannot be read  */
/*  until the verbs are known, and before everything after it because  */
/*  nothing downstream should have to ask whether the block it stands  */
/*  in closes.                                                         */
/* ------------------------------------------------------------------ */

/**
 * pn_figure_check_blocks:
 * @statements: (element-type PnFigureStatement): statements already
 *              through pn_figure_check_verbs()
 * @errors:     (nullable) (element-type PnFigureError): collector
 *
 * Matches every `repeat` with an `end`, reporting the three ways that
 * can fail: an `end` with no `repeat` open, a `repeat` still open at
 * the end of the program, and a `repeat` inside a `repeat` — nesting
 * is refused for now (86.2), so a grid is one loop and the floor/mod
 * arithmetic its index affords.
 *
 * Every failure is reported with its line and the scan goes on, so a
 * program with two structural mistakes still says how many there were.
 * Statements are NOT removed: an unmatched block cannot be repaired by
 * dropping a line, and the program draws nothing anyway (80.10a).
 *
 * Returns: %TRUE when every block is closed exactly once.
 */
gboolean pn_figure_check_blocks (GPtrArray *statements,
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

/* ------------------------------------------------------------------ */
/*  The binding snapshot                                               */
/*                                                                     */
/*  A figure REPAINTS -- on an expose, on a zoom, later on a timer --  */
/*  long after the message that last changed it, so unlike Calculator  */
/*  2 it cannot rebuild its variables from the message in hand.  It    */
/*  keeps them, and the snapshot is that keeping (80.8e): a small      */
/*  name -> value table the node refills when a message arrives and    */
/*  the resolver re-applies to a cleared store before every frame.     */
/*                                                                     */
/*  A value may be a vector, because an input may be one, and a vector */
/*  is what makes a figure a film (80.16): the resolver picks one      */
/*  element of it per frame, at the ARGUMENT where it lands.           */
/* ------------------------------------------------------------------ */

typedef struct _PnFigureSnapshot PnFigureSnapshot;

/**
 * pn_figure_snapshot_new:
 *
 * Returns: (transfer full): an empty snapshot.
 */
PnFigureSnapshot *pn_figure_snapshot_new (void);

/**
 * pn_figure_snapshot_free:
 * @self: (nullable) (transfer full): a snapshot, or %NULL
 *
 * Frees @self and every value in it.  Safe to call with %NULL.
 */
void pn_figure_snapshot_free (PnFigureSnapshot *self);

/**
 * pn_figure_snapshot_clear:
 * @self: the snapshot
 *
 * Drops every name, which is what a node does before re-latching a
 * message's inputs into it.
 */
void pn_figure_snapshot_clear (PnFigureSnapshot *self);

/**
 * pn_figure_snapshot_set:
 * @self:  the snapshot
 * @name:  the variable name
 * @value: the value to bind
 *
 * Binds @name, replacing any previous binding.
 */
void pn_figure_snapshot_set (PnFigureSnapshot *self,
                             const gchar      *name,
                             gdouble           value);

/**
 * pn_figure_snapshot_set_vector:
 * @self: the snapshot
 * @name: the variable name
 * @vec:  the vector; the snapshot takes its own reference
 *
 * Binds @name to a vector, replacing any previous binding.
 */
void pn_figure_snapshot_set_vector (PnFigureSnapshot *self,
                                    const gchar      *name,
                                    PnVector         *vec);

/**
 * pn_figure_frame_count:
 * @free_names: (nullable) (element-type utf8): what the program reads,
 *              from pn_figure_free_names()
 * @snapshot:   (nullable): the latched inputs
 * @frames:     the explicit count, 0 to take it from the data (80.17a)
 *
 * How many frames the film has (TODO #82.1): the SHORTEST of every
 * vector input the program reads and of @frames when it is non-zero.
 * A vector input the program never reads does not count.
 *
 * Measured at the inputs, because the inputs are the only place a
 * vector can come from -- every operator and function is elementwise,
 * a comparison collapses to a scalar, and the tail rule only ever
 * LENGTHENS -- so every vector argument is at least this long and the
 * frame indexer never runs off an end.  And it is known before the
 * program runs, which is what lets `t` and `frame` be bound up front.
 *
 * Returns: 1 for a still figure; 0 when a vector the program reads is
 *   EMPTY, so there is no frame to draw.
 */
guint pn_figure_frame_count (GPtrArray              *free_names,
                             const PnFigureSnapshot *snapshot,
                             guint                   frames);

/* ------------------------------------------------------------------ */
/*  The resolved display list                                          */
/*                                                                     */
/*  What a frame comes to: every expression evaluated, every user      */
/*  coordinate mapped through the view transform of 80.4, every length */
/*  scaled -- a list of operations in DEVICE units that the painter    */
/*  walks and strokes without evaluating anything (80.4i, 80.10d).     */
/*                                                                     */
/*  The list is a transcript of the program and not only of its ink:   */
/*  a pen-state change is an operation too, so the state machine is    */
/*  observable in pn_figure_display_to_string() rather than merely     */
/*  implied by the shapes that follow (80.12a).  The painter carries   */
/*  the state as it walks; it never has to work anything out.          */
/*                                                                     */
/*  The language's verbs collapse on the way in: `lineto`, `rline` and */
/*  `line` are all %PN_FIGURE_OP_LINE between two device points, and   */
/*  `rmove` is an absolute %PN_FIGURE_OP_MOVE.  Resolving them is the  */
/*  back end's job precisely so the painter does not have to know the  */
/*  pen is in user units.                                              */
/*                                                                     */
/*  An assignment leaves NO operation: it changes a variable, not the  */
/*  drawing, and its effect is already in the numbers of the           */
/*  statements that follow it.                                         */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_FIGURE_OP_VIEW,   /* a window came into force; see @window      */
    PN_FIGURE_OP_COLOR,  /* stroke colour <- @color                    */
    PN_FIGURE_OP_FILL,   /* fill colour <- @color, and filling ON      */
    PN_FIGURE_OP_NOFILL, /* filling OFF                                */
    PN_FIGURE_OP_WIDTH,  /* stroke width <- @value, 0 = device hairline */
    PN_FIGURE_OP_DASH,   /* dash pattern <- @dashes                    */
    PN_FIGURE_OP_FONT,   /* text size <- @value                        */
    PN_FIGURE_OP_ALIGN,  /* text anchoring <- @halign, @valign         */

    PN_FIGURE_OP_MOVE,   /* pen to (@x, @y), no ink                    */
    PN_FIGURE_OP_LINE,   /* @points: one segment, two points           */
    PN_FIGURE_OP_POINT,  /* a filled disc, centre (@x, @y), radius @r  */
    PN_FIGURE_OP_CIRCLE, /* centre (@x, @y), radius @r                 */
    PN_FIGURE_OP_ARC,    /* ... from @a0 to @a1, DEVICE degrees        */
    PN_FIGURE_OP_RECT,   /* @x, @y, @w, @h, already normalised         */
    PN_FIGURE_OP_POLY,   /* @points, closed                            */
    PN_FIGURE_OP_PATH,   /* @points, open                              */
    PN_FIGURE_OP_TEXT,   /* @text at (@x, @y), anchored @halign/@valign */

    PN_FIGURE_OP_SKIP,   /* a statement that was not run, and why      */
} PnFigureOpKind;

/* One operation.  A plain struct with a field per need rather than a
 * union: there are a few dozen of these in a frame, the kind says which
 * fields mean anything, and a union buys nothing but casts.
 *
 * Lengths -- @value, @r, @dashes -- are DEVICE units, already scaled by
 * the view and clamped (80.4b), which is why a `view` that changes the
 * scale re-emits the state that depended on it.
 *
 * @a0 and @a1 are DEVICE degrees, not the user degrees the program
 * wrote: our y flip turns a counter-clockwise user sweep into a
 * clockwise device one, and a reversed bound (80.4d) turns it back.
 * Core knows the signs, so core does the arithmetic and @negative says
 * plainly which of cairo_arc() and cairo_arc_negative() to call (80.6d).
 *
 * @scale_x and @scale_y on a %PN_FIGURE_OP_VIEW are the per-axis scales
 * divided by @scale: both 1 unless `stretch` is on, and what a circle
 * is drawn inside so that it becomes the right ellipse (80.6e). */
typedef struct
{
    PnFigureOpKind  kind;
    gint            line;     /* the source line it came from          */

    PnColor         color;    /* COLOR, FILL                           */
    gdouble         value;    /* WIDTH, FONT                           */
    gdouble         dashes[4];/* DASH                                  */
    gint            n_dashes; /* DASH: 0 for solid                     */
    PnFigureHAlign  halign;   /* ALIGN, TEXT                           */
    PnFigureVAlign  valign;   /* ALIGN, TEXT                           */

    gdouble         x, y;     /* MOVE, POINT, CIRCLE, ARC, RECT, TEXT  */
    gdouble         w, h;     /* RECT; VIEW: the device box            */
    gdouble         r;        /* POINT, CIRCLE, ARC                    */
    gdouble         a0, a1;   /* ARC, in device degrees                */
    gboolean        negative; /* ARC: sweep clockwise in device space  */
    GArray         *points;   /* LINE, POLY, PATH: #gdouble, x,y pairs */
    gchar          *text;     /* TEXT: the label; SKIP: the reason     */

    gdouble         window[4];/* VIEW: xmin, ymin, xmax, ymax, user    */
    gdouble         scale;    /* VIEW: device units per user unit      */
    gdouble         scale_x;  /* VIEW: x scale over @scale             */
    gdouble         scale_y;  /* VIEW: y scale over @scale             */
} PnFigureOp;

/* The name a `repeat` block binds its iteration index to (TODO #86.1b):
 * always this one, never a name the block chooses, so the language has
 * one form to remember and one paragraph to document.  A program
 * variable or an input called `i` is shadowed inside the block. */
#define PN_FIGURE_INDEX_NAME "i"

/* How many times a `repeat` may run (TODO #86.5).  A count above it
 * skips the block whole rather than clamping: a figure that freezes
 * the editor for a mistyped exponent is worse than one that refuses
 * out loud, and a silently shortened loop draws a lie. */
#define PN_FIGURE_MAX_REPEAT 1000

/**
 * pn_figure_resolve:
 * @statements: (element-type PnFigureStatement): the parsed program,
 *              whose lines must still be alive
 * @free_names: (nullable) (element-type utf8): what the program reads,
 *              from pn_figure_free_names(); anything the snapshot does
 *              not supply is bound to 0 (80.2 rule 12)
 * @snapshot:   (nullable): the latched inputs (80.8e)
 * @index:      the frame, 0-based: a vector argument contributes this
 *              element of itself, a scalar the same value in every
 *              frame (80.16, 82.2).  0 for a still.
 * @x:          device rectangle: left
 * @y:          ... top
 * @w:          ... width
 * @h:          ... height
 * @stretch:    %TRUE to fill the rectangle instead of preserving the
 *              drawing's aspect and centring it (80.4h)
 * @out_error:  (out) (optional) (nullable): a message when the frame
 *              could not be resolved at all, %NULL when it could
 *
 * Runs one frame: a store cleared to the language's constants and the
 * snapshot, then every statement in order, into a device-space display
 * list.
 *
 * The program is NOT re-run with each vector replaced by its element:
 * the store is elementwise, so evaluating it with the vectors in place
 * already computes the whole film, and @index only chooses which
 * element of each argument is drawn (80.16a).
 *
 * The three error classes of 80.10 are three different things here.  A
 * PROGRAM error never reaches this function -- the front end kept the
 * statement out.  A runtime VALUE problem, which is what a knob winding
 * through zero produces, skips its statement, leaves a
 * %PN_FIGURE_OP_SKIP marker saying why, and lets the rest of the figure
 * draw.  A runtime TYPE problem -- a vector argument with no element
 * @index, empty or shorter than pn_figure_frame_count() promised -- is
 * none of those: winding a knob will not cure it, so it empties the
 * list and sets @out_error, and the node paints red.
 *
 * Nothing is drawn until everything is resolved (80.10d), which is what
 * makes "nothing is drawn" honest: the list this returns is either the
 * whole figure or empty.
 *
 * Returns: (transfer full) (element-type PnFigureOp): the display list,
 *   never %NULL, empty when @out_error was set.
 */
GPtrArray *pn_figure_resolve (GPtrArray              *statements,
                              GPtrArray              *free_names,
                              const PnFigureSnapshot *snapshot,
                              guint                   index,
                              gdouble                 x,
                              gdouble                 y,
                              gdouble                 w,
                              gdouble                 h,
                              gboolean                stretch,
                              gchar                 **out_error);

/**
 * pn_figure_display_to_string:
 * @ops: (nullable) (element-type PnFigureOp): a resolved display list
 *
 * The display list as text, one line per operation, device numbers at
 * two decimals and every one of them locale-independent.  It is what
 * 80.12's headless tests assert, and it is the debugging tool as well:
 * a figure that draws the wrong thing is a dump away from saying why.
 *
 * The first line is a comment carrying the window, the scale and the
 * device box the window was fitted into, so a transform is one line to
 * check; a `view` statement that changes any of it prints another.  The
 * whole pen state follows it, because a frame begins by resetting it
 * (80.5g) and the reset is worth seeing.  A skipped statement prints
 * `# skip <line> <reason>`, so 80.10(b) is asserted by what is there
 * rather than by what is missing.
 *
 * Returns: (transfer full): the text, "" for an empty list.
 */
gchar *pn_figure_display_to_string (GPtrArray *ops);

/* ------------------------------------------------------------------ */
/*  The node                                                           */
/*                                                                     */
/*  A sink (80.8g): a figure is a readout, not a stage in a chain.     */
/*  Its inputs become the variables the program reads, latched by the  */
/*  core and kept in the snapshot so a repaint long after the last     */
/*  message still draws the same figure.                               */
/*                                                                     */
/*  The node owns the whole core half and nothing of the painting:     */
/*  pn_figure_render() hands back a resolved display list in DEVICE    */
/*  units, and pn-figure-gui.c walks it (80.4i, 80.10d).               */
/* ------------------------------------------------------------------ */

/* Geometry, following PnPlot's 280-wide card with a client area a
 * little taller than its 173, so the drawing area is roughly 4:3 —
 * the shape a plate wants and the one that wastes least room to the
 * letterbox of 80.4 (80.9e). */
#define PN_FIGURE_WIDTH          280.0
#define PN_FIGURE_HEADER_HEIGHT   40.0
#define PN_FIGURE_GAP              4.0
#define PN_FIGURE_CLIENT_HEIGHT  210.0
#define PN_FIGURE_TOTAL_HEIGHT   (PN_FIGURE_HEADER_HEIGHT + \
                                  PN_FIGURE_GAP +           \
                                  PN_FIGURE_CLIENT_HEIGHT)

/* How a film plays (80.17a).  ONCE stops on the last frame and holds
 * it, which is why there is no fourth "hold last" mode. */
typedef enum
{
    PN_FIGURE_PLAY_ONCE,
    PN_FIGURE_PLAY_LOOP,
    PN_FIGURE_PLAY_PING_PONG,
} PnFigurePlayMode;

#define PN_TYPE_FIGURE_PLAY_MODE (pn_figure_play_mode_get_type ())
GType pn_figure_play_mode_get_type (void) G_GNUC_CONST;

/* Frames per second when nothing says otherwise, and the most anyone
 * may ask for (80.17a). */
#define PN_FIGURE_DEFAULT_FPS 25
#define PN_FIGURE_MAX_FPS     60

/* The largest explicit `frames` count: 400 seconds at the default rate.
 * A bound because 82.5 builds `t` and `frame` as vectors this long. */
#define PN_FIGURE_MAX_FRAMES  10000

/**
 * pn_figure_step_frame:
 * @mode:      how the film plays
 * @count:     the frame count, from pn_figure_frame_count()
 * @frame:     (inout): the frame being shown, moved to the next one
 * @direction: (inout): +1 or -1; only %PN_FIGURE_PLAY_PING_PONG reads
 *             or writes it, and anything else counts as +1
 *
 * One tick of the timer (TODO #82.3), with no timer in it, so every
 * mode is testable without a clock.  A @frame past the end of the film
 * is brought inside it first.
 *
 * Returns: %TRUE when there is another tick to come, %FALSE when the
 *   film is standing still -- a still (@count of 1 or less) or a
 *   %PN_FIGURE_PLAY_ONCE film that has reached its last frame.
 */
gboolean pn_figure_step_frame (PnFigurePlayMode  mode,
                               guint             count,
                               guint            *frame,
                               gint             *direction);

#define PN_TYPE_FIGURE (pn_figure_get_type ())
G_DECLARE_FINAL_TYPE (PnFigure, pn_figure, PN, FIGURE, PnNode)

/**
 * pn_figure_new:
 *
 * Returns: (transfer full): a figure carrying the default program of
 *   80.11(g), so a node dragged in from the palette draws something at
 *   once instead of showing an empty box.
 */
PnFigure *pn_figure_new (void);

/**
 * pn_figure_get_frame_count:
 * @self: the figure
 *
 * pn_figure_frame_count() for the current program and latched inputs.
 *
 * Returns: 1 for a still, 0 when an input the program reads is an
 *   empty vector.
 */
guint pn_figure_get_frame_count (PnFigure *self);

/**
 * pn_figure_get_frame:
 * @self: the figure
 *
 * The frame pn_figure_render() draws: the node's current frame, already
 * brought inside the film when a new message shortened it.
 *
 * Returns: the 0-based frame index.
 */
guint pn_figure_get_frame (PnFigure *self);

/**
 * pn_figure_is_playing:
 * @self: the figure
 *
 * Whether the film timer is running.  It runs only while the film has
 * more than one frame, has somewhere left to go, and something is
 * connected to #PnNode::repaint-needed -- a headless figure has no
 * painter, and a timer nobody watches is pure waste (80.17c).
 *
 * Returns: %TRUE while the timer is running.
 */
gboolean pn_figure_is_playing (PnFigure *self);

/**
 * pn_figure_render:
 * @self: the figure
 * @x:    device rectangle: left
 * @y:    ... top
 * @w:    ... width
 * @h:    ... height
 *
 * Resolves the current frame (pn_figure_get_frame()) into @self's
 * device rectangle, starts the film timer if a film is waiting for one
 * -- a call from the painter is the proof that something is watching --
 * and updates the
 * node's error state — the `error` property and, for the two classes
 * that deserve it, pn_node_set_has_error() (80.10).
 *
 * This is the painter's seam.  A program that did not parse resolves to
 * an EMPTY list, because a program error draws nothing at all (80.10a);
 * a runtime value problem leaves its %PN_FIGURE_OP_SKIP marker in an
 * otherwise complete list and does not colour the node red (80.10b).
 *
 * Returns: (transfer full) (element-type PnFigureOp): the display list,
 *   never %NULL.
 */
GPtrArray *pn_figure_render (PnFigure *self,
                             gdouble   x,
                             gdouble   y,
                             gdouble   w,
                             gdouble   h);

/**
 * pn_figure_dump:
 * @self:  the figure
 * @frame: the frame to dump, 0-based; one past the film is an error in
 *         the dump rather than a quiet clamp, so a test that miscounts
 *         the film finds out (80.16f)
 * @x:    device rectangle: left
 * @y:    ... top
 * @w:    ... width
 * @h:    ... height
 *
 * pn_figure_render() of @frame, rendered as text by
 * pn_figure_display_to_string().  The node's own current frame is left
 * where it was.
 * Not a test-only hack (80.12a): it is the debugging tool and the D-Bus
 * automation surface as well — a figure that draws the wrong thing is
 * one call away from saying why.
 *
 * Returns: (transfer full): the dump, "" when nothing was drawn.
 */
gchar *pn_figure_dump (PnFigure *self,
                       guint     frame,
                       gdouble   x,
                       gdouble   y,
                       gdouble   w,
                       gdouble   h);

/**
 * pn_figure_get_error:
 * @self: the figure
 *
 * The text the client area shows in place of the figure, and the value
 * of the read-only `error` property (80.10f).  It reflects the LAST
 * pn_figure_render() for the runtime classes, and the current program
 * for the parse class.
 *
 * Returns: (transfer none): the message, or "" when all is well.
 */
const gchar *pn_figure_get_error (PnFigure *self);

/**
 * pn_figure_get_background_color:
 * @self: the figure
 * @out:  (out): the colour the client rectangle is filled with before
 *        the program runs (80.4f)
 *
 * A painter read-accessor, so the gui half needs no property lookups.
 */
void pn_figure_get_background_color (PnFigure *self,
                                     PnColor  *out);

/**
 * pn_figure_get_font_family:
 * @self: the figure
 *
 * The family every `text` in this figure is drawn in — a node property
 * rather than a verb, so one figure is typographically consistent
 * (80.7i).
 *
 * Returns: (transfer none): the family name; "" means the theme default.
 */
const gchar *pn_figure_get_font_family (PnFigure *self);

G_END_DECLS

#endif /* PN_FIGURE_H */
