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

G_END_DECLS

#endif /* PN_FIGURE_H */
