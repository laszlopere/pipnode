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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-figure.h"

#include <stdarg.h>
#include <string.h>

/* ================================================================== */
/*  Errors                                                             */
/* ================================================================== */

void
pn_figure_error_free (
        PnFigureError *self)
{
    if (self == NULL)
        return;

    g_free (self->message);
    g_free (self);
}

GPtrArray *
pn_figure_errors_new (void)
{
    return g_ptr_array_new_with_free_func (
               (GDestroyNotify) pn_figure_error_free);
}

/* Append one error to @errors, which may be %NULL when the caller does
 * not want them — the scan still runs, it just stays quiet. */
static void figure_error_add (GPtrArray   *errors,
                              gint         line,
                              gint         column,
                              const gchar *format,
                              ...) G_GNUC_PRINTF (4, 5);

static void
figure_error_add (
        GPtrArray   *errors,
        gint         line,
        gint         column,
        const gchar *format,
        ...)
{
    PnFigureError *error;
    va_list        args;

    if (errors == NULL)
        return;

    error         = g_new0 (PnFigureError, 1);
    error->line   = line;
    error->column = column;

    va_start (args, format);
    error->message = g_strdup_vprintf (format, args);
    va_end (args);

    g_ptr_array_add (errors, error);
}

/* ================================================================== */
/*  The line scanner                                                   */
/* ================================================================== */

/* Columns count characters, not bytes, and this is the whole of what
 * that costs: every byte that is not a UTF-8 continuation byte starts
 * one character.  Valid for any well-formed UTF-8, which is what a
 * GtkSourceView buffer hands us. */
#define IS_CONTINUATION(b) (((b) & 0xC0) == 0x80)

/* TRUE when the raw line holds nothing but whitespace.  Asked of the
 * RAW line on purpose: after comment stripping a comment-only line
 * looks blank too, and the two must part company inside a
 * continuation. */
static gboolean
is_blank (
        const gchar *raw)
{
    for (; *raw != '\0'; raw++)
        if (!g_ascii_isspace (*raw))
            return FALSE;

    return TRUE;
}

/* Finds where the code on @raw ends: at an unquoted "#", or at the end
 * of the line.  Sets @out_ok to %FALSE and reports an error when a
 * string was still open at the end — the reason this walk has to know
 * about quotes in the first place. */
static gsize
strip_comment (
        const gchar *raw,
        gint         lineno,
        GPtrArray   *errors,
        gboolean    *out_ok)
{
    gboolean in_string    = FALSE;
    gboolean escaped      = FALSE;
    gint     quote_column = 0;
    gint     column       = 0;
    gsize    i;

    for (i = 0; raw[i] != '\0'; i++)
    {
        guchar c = (guchar) raw[i];

        if (!IS_CONTINUATION (c))
            column++; /* now the 1-based column of raw[i] */

        if (escaped)
        {
            /* Whatever followed the backslash is part of the string.
             * Only \" and \\ mean anything, but neither may close it. */
            escaped = FALSE;
        }
        else if (in_string)
        {
            if (c == '\\')
                escaped = TRUE;
            else if (c == '"')
                in_string = FALSE;
        }
        else if (c == '"')
        {
            in_string    = TRUE;
            quote_column = column;
        }
        else if (c == '#')
        {
            break;
        }
    }

    *out_ok = !in_string;

    if (in_string)
        figure_error_add (errors, lineno, quote_column,
                          "unterminated string");

    return i;
}

/* The logical line being assembled, and the list it goes into. */
typedef struct
{
    GPtrArray *lines;   /* PnFigureLine *, the result                  */
    GString   *text;    /* the joined text so far, %NULL between lines */
    GArray    *pieces;  /* PnFigurePiece for @text                     */
    gboolean   broken;  /* an error killed this line: drop it          */
    gboolean   pending; /* the last code character was a comma         */
} Scan;

/* Adds one source piece to the line under construction, starting a new
 * line when there is none.  @len is always non-zero. */
static void
scan_append (
        Scan        *scan,
        const gchar *piece,
        gsize        len,
        gint         line,
        gint         column)
{
    PnFigurePiece entry;

    if (scan->text == NULL)
    {
        scan->text   = g_string_new (NULL);
        scan->pieces = g_array_new (FALSE, FALSE, sizeof (PnFigurePiece));
        scan->broken = FALSE;
    }

    entry.offset = scan->text->len;
    entry.line   = line;
    entry.column = column;
    g_array_append_val (scan->pieces, entry);

    g_string_append_len (scan->text, piece, len);
}

/* Ends the logical line under construction: appends it to the result,
 * or throws it away when it is broken.  A no-op when there is none. */
static void
scan_flush (
        Scan *scan)
{
    if (scan->text != NULL)
    {
        if (scan->broken)
        {
            g_string_free (scan->text, TRUE);
            g_array_unref (scan->pieces);
        }
        else
        {
            PnFigureLine *line = g_new0 (PnFigureLine, 1);

            line->pieces = scan->pieces;
            line->line   = g_array_index (scan->pieces,
                                          PnFigurePiece, 0).line;
            line->text   = g_string_free (scan->text, FALSE);

            g_ptr_array_add (scan->lines, line);
        }

        scan->text   = NULL;
        scan->pieces = NULL;
    }

    scan->broken  = FALSE;
    scan->pending = FALSE;
}

void
pn_figure_line_free (
        PnFigureLine *self)
{
    if (self == NULL)
        return;

    g_free (self->text);
    if (self->pieces != NULL)
        g_array_unref (self->pieces);
    g_free (self);
}

void
pn_figure_line_locate (
        const PnFigureLine *self,
        gsize               offset,
        gint               *out_line,
        gint               *out_column)
{
    const PnFigurePiece *piece;
    gsize                length;
    gsize                i;
    gint                 column;
    guint                n;

    g_return_if_fail (self != NULL);
    g_return_if_fail (self->pieces != NULL && self->pieces->len > 0);

    length = strlen (self->text);
    if (offset > length)
        offset = length;

    /* The piece that covers @offset is the last one starting at or
     * before it; there are never more than a handful. */
    piece = &g_array_index (self->pieces, PnFigurePiece, 0);
    for (n = 1; n < self->pieces->len; n++)
    {
        const PnFigurePiece *next =
            &g_array_index (self->pieces, PnFigurePiece, n);

        if (next->offset > offset)
            break;
        piece = next;
    }

    column = piece->column;
    for (i = piece->offset; i < offset; i++)
        if (!IS_CONTINUATION ((guchar) self->text[i]))
            column++;

    if (out_line != NULL)
        *out_line = piece->line;
    if (out_column != NULL)
        *out_column = column;
}

GPtrArray *
pn_figure_scan (
        const gchar *program,
        GPtrArray   *errors)
{
    Scan    scan = { NULL, NULL, NULL, FALSE, FALSE };
    gchar **raw;
    guint   i;

    scan.lines = g_ptr_array_new_with_free_func (
                     (GDestroyNotify) pn_figure_line_free);

    if (program == NULL || *program == '\0')
        return scan.lines;

    raw = g_strsplit (program, "\n", -1);

    for (i = 0; raw[i] != NULL; i++)
    {
        gint     lineno    = (gint) i + 1;
        gboolean ok        = TRUE;
        gboolean blank_raw = is_blank (raw[i]);
        gsize    end       = strip_comment (raw[i], lineno, errors, &ok);
        gsize    start     = 0;

        if (!ok)
        {
            /* Broken quoting takes the whole logical line with it, so
             * no later stage trips over the same mistake. */
            scan.broken = TRUE;
            scan_flush (&scan);
            continue;
        }

        /* Leading whitespace is free; trailing whitespace has to go
         * before the continuation comma can be the last character. */
        while (start < end && g_ascii_isspace (raw[i][start]))
            start++;
        while (end > start && g_ascii_isspace (raw[i][end - 1]))
            end--;

        if (scan.pending)
        {
            if (blank_raw)
            {
                /* A blank line ends the continuation and leaves the
                 * dangling comma to be complained about. */
                scan_flush (&scan);
                continue;
            }
            if (end == start)
                continue; /* comment-only: keep swallowing */
        }
        else if (end == start)
        {
            continue;     /* blank or comment-only */
        }

        /* Everything skipped above is ASCII whitespace, one byte to the
         * character, so the column of the first kept byte is its byte
         * offset plus one. */
        scan_append (&scan, raw[i] + start, end - start,
                     lineno, (gint) start + 1);

        scan.pending = scan.text->str[scan.text->len - 1] == ',';
        if (!scan.pending)
            scan_flush (&scan);
    }

    scan_flush (&scan); /* a comma on the program's last line */

    g_strfreev (raw);
    return scan.lines;
}
