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

#include "pn-expr-bind.h"
#include "pn-expr-funcs.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-var-store.h"

#include <math.h>
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

/* ================================================================== */
/*  The statement splitter                                            */
/* ================================================================== */

static void
pn_figure_arg_free (
        PnFigureArg *self)
{
    if (self == NULL)
        return;

    pn_expr_node_free (self->ast);
    g_free (self->text);
    g_free (self);
}

void
pn_figure_statement_free (
        PnFigureStatement *self)
{
    if (self == NULL)
        return;

    pn_expr_node_free (self->ast);
    g_free (self->name);
    if (self->args != NULL)
        g_ptr_array_unref (self->args);
    g_free (self);
}

/* Reports an error at @offset in @line, turning the offset into the
 * source line and column it really came from. */
static void report_at (GPtrArray          *errors,
                       const PnFigureLine *line,
                       gsize               offset,
                       const gchar        *format,
                       ...) G_GNUC_PRINTF (4, 5);

static void
report_at (
        GPtrArray          *errors,
        const PnFigureLine *line,
        gsize               offset,
        const gchar        *format,
        ...)
{
    PnFigureError *error;
    va_list        args;
    gint           lineno = 0;
    gint           column = 0;

    if (errors == NULL)
        return;

    pn_figure_line_locate (line, offset, &lineno, &column);

    error         = g_new0 (PnFigureError, 1);
    error->line   = lineno;
    error->column = column;

    va_start (args, format);
    error->message = g_strdup_vprintf (format, args);
    va_end (args);

    g_ptr_array_add (errors, error);
}

/* Copies the body of a string literal, resolving the three escapes the
 * language defines.  @body runs to the closing quote, which is not
 * included.  Returns %NULL, with the error reported, on an escape that
 * means nothing. */
static gchar *
unescape (
        const PnFigureLine *line,
        gsize               body,
        gsize               end,
        GPtrArray          *errors)
{
    const gchar *text = line->text;
    GString     *out  = g_string_new (NULL);
    gsize        i;

    for (i = body; i < end; i++)
    {
        if (text[i] != '\\')
        {
            g_string_append_c (out, text[i]);
            continue;
        }

        /* The scanner already proved the literal is closed, so a
         * backslash always has a character after it. */
        switch (text[i + 1])
        {
        case '"':  g_string_append_c (out, '"');  break;
        case '\\': g_string_append_c (out, '\\'); break;
        case 'n':  g_string_append_c (out, '\n'); break;

        default:
        {
            const gchar *next = text + i + 1;
            const gchar *stop = g_utf8_find_next_char (next, text + end);

            if (stop == NULL)
                stop = text + end;

            report_at (errors, line, i, "unknown escape \"\\%.*s\"",
                       (int) (stop - next), next);
            g_string_free (out, TRUE);
            return NULL;
        }
        }

        i++; /* the escaped character */
    }

    return g_string_free (out, FALSE);
}

/* Turns the slice [@start, @end) of @line into one argument and appends
 * it to @args.  The slice is what stood between two commas, whitespace
 * and all.  Returns %FALSE, with the error reported, when it is not an
 * argument at all. */
static gboolean
add_argument (
        GPtrArray          *args,
        const PnFigureLine *line,
        gsize               start,
        gsize               end,
        GPtrArray          *errors)
{
    const gchar *text = line->text;
    PnFigureArg *arg;

    while (start < end && g_ascii_isspace (text[start]))
        start++;
    while (end > start && g_ascii_isspace (text[end - 1]))
        end--;

    if (start == end)
    {
        /* A comma with nothing after it — the dangling comma a
         * continuation left behind reaches us exactly here. */
        report_at (errors, line, start, "empty argument");
        return FALSE;
    }

    arg         = g_new0 (PnFigureArg, 1);
    arg->offset = start;

    if (text[start] == '"')
    {
        gboolean escaped = FALSE;
        gsize    close;

        for (close = start + 1; close < end; close++)
        {
            if (escaped)
                escaped = FALSE;
            else if (text[close] == '\\')
                escaped = TRUE;
            else if (text[close] == '"')
                break;
        }

        /* Quoted means literal, so the quotes have to be the whole
         * argument: "A" is a string, "A" + 1 is a mistake.  The slice
         * is already right-trimmed, so there is something to point at
         * past the whitespace. */
        if (close + 1 != end)
        {
            gsize junk = close + 1;

            while (junk < end && g_ascii_isspace (text[junk]))
                junk++;

            report_at (errors, line, junk,
                       "unexpected text after a string");
            g_free (arg);
            return FALSE;
        }

        arg->kind = PN_FIGURE_ARG_STRING;
        arg->text = unescape (line, start + 1, close, errors);

        if (arg->text == NULL)
        {
            g_free (arg);
            return FALSE;
        }
    }
    else
    {
        arg->kind = PN_FIGURE_ARG_EXPRESSION;
        arg->text = g_strndup (text + start, end - start);
    }

    g_ptr_array_add (args, arg);
    return TRUE;
}

/* Splits one logical line.  Returns %NULL, with every error it found
 * reported, when the line is not a statement. */
static PnFigureStatement *
split_line (
        const PnFigureLine *line,
        GPtrArray          *errors)
{
    const gchar       *text = line->text;
    PnFigureStatement *statement;
    GPtrArray         *args;
    gboolean           ok      = TRUE;
    gboolean           in_string = FALSE;
    gboolean           escaped = FALSE;
    gint               depth   = 0;
    gsize              name_end;
    gsize              rest;
    gsize              start;
    gsize              i;

    /* Rule 1: a statement begins with an identifier, spelled the way
     * the expression lexer spells one. */
    if (!g_ascii_isalpha (text[0]) && text[0] != '_')
    {
        report_at (errors, line, 0, "expected a verb or an assignment");
        return NULL;
    }

    for (name_end = 0;
         g_ascii_isalnum (text[name_end]) || text[name_end] == '_';
         name_end++)
        ;

    rest = name_end;
    while (g_ascii_isspace (text[rest]))
        rest++;

    /* One lookahead settles it: a lone "=" assigns, "==" compares. */
    if (text[rest] == '=' && text[rest + 1] != '=')
    {
        statement         = g_new0 (PnFigureStatement, 1);
        statement->kind   = PN_FIGURE_STATEMENT_ASSIGNMENT;
        statement->name   = g_strndup (text, name_end);
        statement->args   = g_ptr_array_new_with_free_func (
                                (GDestroyNotify) pn_figure_arg_free);
        statement->source = line;
        return statement;
    }

    args = g_ptr_array_new_with_free_func ((GDestroyNotify) pn_figure_arg_free);

    /* Rule 3: split on the commas at paren depth 0, outside quotes.
     * A verb with nothing after it has no arguments, as against one
     * empty argument. */
    if (text[rest] != '\0')
    {
        for (i = start = rest; ; i++)
        {
            gchar c = text[i];

            if (c == '\0' || (!in_string && depth == 0 && c == ','))
            {
                if (!add_argument (args, line, start, i, errors))
                    ok = FALSE;
                if (c == '\0')
                    break;
                start = i + 1;
                continue;
            }

            if (escaped)
                escaped = FALSE;
            else if (in_string)
            {
                if (c == '\\')
                    escaped = TRUE;
                else if (c == '"')
                    in_string = FALSE;
            }
            else if (c == '"')
                in_string = TRUE;
            else if (c == '(')
                depth++;
            else if (c == ')' && depth > 0)
                depth--;
        }
    }

    if (!ok)
    {
        g_ptr_array_unref (args);
        return NULL;
    }

    statement         = g_new0 (PnFigureStatement, 1);
    statement->kind   = PN_FIGURE_STATEMENT_VERB;
    statement->name   = g_ascii_strdown (text, (gssize) name_end);
    statement->args   = args;
    statement->source = line;
    return statement;
}

GPtrArray *
pn_figure_split (
        GPtrArray *lines,
        GPtrArray *errors)
{
    GPtrArray *statements;
    guint      i;

    statements = g_ptr_array_new_with_free_func (
                     (GDestroyNotify) pn_figure_statement_free);

    g_return_val_if_fail (lines != NULL, statements);

    for (i = 0; i < lines->len; i++)
    {
        const PnFigureLine *line = g_ptr_array_index (lines, i);
        PnFigureStatement  *statement = split_line (line, errors);

        if (statement != NULL)
            g_ptr_array_add (statements, statement);
    }

    return statements;
}

/* ================================================================== */
/*  The verb table                                                    */
/* ================================================================== */

typedef enum
{
    VERB_PLAIN  = 0,
    VERB_PAIRS  = 1 << 0, /* an even count: the arguments are x,y points */
    VERB_COLOUR = 1 << 1, /* one quoted literal, or 3-4 expressions      */
} VerbFlags;

/* One row of the table.  @kinds gives the required kind of each
 * argument position — 'e' for an expression, 's' for a string — and
 * @tail the kind of every position past the end of @kinds, or 0 when
 * there is none.  Two characters and a tail describe every verb in the
 * language except the colour pair, which @flags picks out. */
typedef struct
{
    const gchar  *name;
    PnFigureVerb  verb;
    guint         min;
    guint         max;   /* G_MAXUINT when variadic */
    const gchar  *kinds;
    gchar         tail;
    guint         flags;
} VerbInfo;

static const VerbInfo verb_table[] =
{
    /* pen state (80.5) and the window (80.2 rule 11) */
    { "view",   PN_FIGURE_VERB_VIEW,   4, 4,          "",    'e', VERB_PLAIN  },
    { "color",  PN_FIGURE_VERB_COLOR,  1, 4,          "",    0,   VERB_COLOUR },
    { "fill",   PN_FIGURE_VERB_FILL,   1, 4,          "",    0,   VERB_COLOUR },
    { "nofill", PN_FIGURE_VERB_NOFILL, 0, 0,          "",    0,   VERB_PLAIN  },
    { "width",  PN_FIGURE_VERB_WIDTH,  1, 1,          "",    'e', VERB_PLAIN  },
    { "dash",   PN_FIGURE_VERB_DASH,   1, 2,          "se",  0,   VERB_PLAIN  },
    { "font",   PN_FIGURE_VERB_FONT,   1, 1,          "",    'e', VERB_PLAIN  },
    { "align",  PN_FIGURE_VERB_ALIGN,  1, 2,          "",    's', VERB_PLAIN  },

    /* geometry (80.6) */
    { "move",   PN_FIGURE_VERB_MOVE,   2, 2,          "",    'e', VERB_PLAIN  },
    { "rmove",  PN_FIGURE_VERB_RMOVE,  2, 2,          "",    'e', VERB_PLAIN  },
    { "lineto", PN_FIGURE_VERB_LINETO, 2, 2,          "",    'e', VERB_PLAIN  },
    { "rline",  PN_FIGURE_VERB_RLINE,  2, 2,          "",    'e', VERB_PLAIN  },
    { "line",   PN_FIGURE_VERB_LINE,   4, 4,          "",    'e', VERB_PLAIN  },
    { "point",  PN_FIGURE_VERB_POINT,  2, 2,          "",    'e', VERB_PLAIN  },
    { "circle", PN_FIGURE_VERB_CIRCLE, 3, 3,          "",    'e', VERB_PLAIN  },
    { "arc",    PN_FIGURE_VERB_ARC,    5, 5,          "",    'e', VERB_PLAIN  },
    { "rect",   PN_FIGURE_VERB_RECT,   4, 4,          "",    'e', VERB_PLAIN  },
    { "poly",   PN_FIGURE_VERB_POLY,   6, G_MAXUINT,  "",    'e', VERB_PAIRS  },
    { "path",   PN_FIGURE_VERB_PATH,   6, G_MAXUINT,  "",    'e', VERB_PAIRS  },

    /* text (80.7): x, y, format, then one expression per conversion */
    { "text",   PN_FIGURE_VERB_TEXT,   3, G_MAXUINT,  "ees", 'e', VERB_PLAIN  },

    /* the block (80.18a, #86): the count is an expression like every
     * other argument, so `repeat cols * rows` needs no new syntax */
    { "repeat", PN_FIGURE_VERB_REPEAT, 1, 1,          "",    'e', VERB_PLAIN  },
    { "end",    PN_FIGURE_VERB_END,    0, 0,          "",    0,   VERB_PLAIN  },
};

/* The table is small and a program is a few dozen lines, so a linear
 * walk is not worth improving on.  @name is already folded. */
static const VerbInfo *
verb_lookup (
        const gchar *name)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS (verb_table); i++)
        if (g_strcmp0 (verb_table[i].name, name) == 0)
            return &verb_table[i];

    return NULL;
}

/* The kind argument @n must have, or 0 when the table does not say. */
static gchar
kind_at (
        const VerbInfo *info,
        guint           n)
{
    return n < strlen (info->kinds) ? info->kinds[n] : info->tail;
}

/* Checks one argument's kind, reporting at the argument itself so the
 * message does not have to say which one it means. */
static gboolean
check_kind (
        const PnFigureStatement *statement,
        guint                    n,
        gchar                    want,
        GPtrArray               *errors)
{
    const PnFigureArg *arg = g_ptr_array_index (statement->args, n);
    PnFigureArgKind    is  = want == 's' ? PN_FIGURE_ARG_STRING
                                         : PN_FIGURE_ARG_EXPRESSION;

    if (want == 0 || arg->kind == is)
        return TRUE;

    report_at (errors, statement->source, arg->offset,
               want == 's' ? "expected a quoted string"
                           : "expected an expression, not a string");
    return FALSE;
}

/* The two-spelling colour argument, and the only place in the language
 * where the argument COUNT decides what the arguments mean (80.5). */
static gboolean
check_colour (
        const PnFigureStatement *statement,
        const VerbInfo          *info,
        GPtrArray               *errors)
{
    guint n = statement->args->len;
    guint i;

    if (n == 1)
    {
        const PnFigureArg *arg = g_ptr_array_index (statement->args, 0);

        if (arg->kind == PN_FIGURE_ARG_STRING)
            return TRUE;

        /* The generic "expected a quoted string" would be true and
         * unhelpful: what someone who wrote `color c` needs to be told
         * is that the other spelling exists. */
        report_at (errors, statement->source, arg->offset,
                   "expected a quoted colour or 3 or 4 numbers");
        return FALSE;
    }

    if (n < 3 || n > 4)
    {
        report_at (errors, statement->source, 0,
                   "%s takes a quoted colour or 3 or 4 numbers, not %u",
                   info->name, n);
        return FALSE;
    }

    for (i = 0; i < n; i++)
        if (!check_kind (statement, i, 'e', errors))
            return FALSE;

    return TRUE;
}

/* Measures one statement against its row.  Reports at the verb for a
 * count that is wrong and at the argument for a kind that is. */
static gboolean
check_statement (
        PnFigureStatement *statement,
        GPtrArray         *errors)
{
    const VerbInfo *info = verb_lookup (statement->name);
    guint           n    = statement->args->len;
    guint           i;

    if (info == NULL)
    {
        /* Quote what was typed, not what it folded to. */
        gchar *typed = g_strndup (statement->source->text,
                                  strlen (statement->name));

        report_at (errors, statement->source, 0, "unknown verb \"%s\"", typed);
        g_free (typed);
        return FALSE;
    }

    statement->verb = info->verb;

    if (info->flags & VERB_COLOUR)
        return check_colour (statement, info, errors);

    if (n < info->min || n > info->max)
    {
        if (info->max == G_MAXUINT)
            report_at (errors, statement->source, 0,
                       "%s takes at least %u arguments, not %u",
                       info->name, info->min, n);
        else if (info->min != info->max)
            report_at (errors, statement->source, 0,
                       "%s takes %u or %u arguments, not %u",
                       info->name, info->min, info->max, n);
        else
            report_at (errors, statement->source, 0,
                       "%s takes %u argument%s, not %u", info->name,
                       info->min, info->min == 1 ? "" : "s", n);
        return FALSE;
    }

    if ((info->flags & VERB_PAIRS) && (n % 2) != 0)
    {
        report_at (errors, statement->source, 0,
                   "%s takes x and y in pairs", info->name);
        return FALSE;
    }

    for (i = 0; i < n; i++)
        if (!check_kind (statement, i, kind_at (info, i), errors))
            return FALSE;

    return TRUE;
}

gboolean
pn_figure_check_verbs (
        GPtrArray *statements,
        GPtrArray *errors)
{
    gboolean ok = TRUE;
    guint    i  = 0;

    g_return_val_if_fail (statements != NULL, FALSE);

    while (i < statements->len)
    {
        PnFigureStatement *statement = g_ptr_array_index (statements, i);

        if (statement->kind == PN_FIGURE_STATEMENT_ASSIGNMENT
            || check_statement (statement, errors))
        {
            i++;
            continue;
        }

        /* Nothing after this stage should have to ask whether a verb
         * is real, so the bad ones do not travel. */
        g_ptr_array_remove_index (statements, i);
        ok = FALSE;
    }

    return ok;
}

/* ================================================================== */
/*  Blocks                                                            */
/* ================================================================== */

gboolean
pn_figure_check_blocks (
        GPtrArray *statements,
        GPtrArray *errors)
{
    const PnFigureStatement *open  = NULL; /* the outermost open block */
    gint                     depth = 0;
    gboolean                 ok    = TRUE;
    guint                    i;

    g_return_val_if_fail (statements != NULL, FALSE);

    /* Counting DEPTH rather than holding one flag is what keeps a
     * nested block from cascading: the inner `repeat` is reported once
     * and still counted, so the `end` that closes it is not then
     * reported a second time as an `end` with nothing open. */
    for (i = 0; i < statements->len; i++)
    {
        const PnFigureStatement *statement = g_ptr_array_index (statements, i);

        if (statement->kind != PN_FIGURE_STATEMENT_VERB)
            continue;

        if (statement->verb == PN_FIGURE_VERB_REPEAT)
        {
            /* One level, deliberately (86.2): a grid is one loop and
             * the floor/mod arithmetic its index affords, and refusing
             * nesting is what lets the index be a single fixed name. */
            if (depth > 0)
            {
                report_at (errors, statement->source, 0,
                           "repeat cannot be nested inside another repeat");
                ok = FALSE;
            }
            else
            {
                open = statement;
            }
            depth++;
        }
        else if (statement->verb == PN_FIGURE_VERB_END)
        {
            if (depth == 0)
            {
                report_at (errors, statement->source, 0,
                           "end without a repeat");
                ok = FALSE;
                continue;
            }
            if (--depth == 0)
                open = NULL;
        }
    }

    if (depth > 0 && open != NULL)
    {
        report_at (errors, open->source, 0, "repeat without an end");
        ok = FALSE;
    }

    return ok;
}

/* ================================================================== */
/*  Literals                                                          */
/* ================================================================== */

/* A word table shared by the dash styles and the two alignment axes:
 * each is a short closed vocabulary, and a message that names the whole
 * vocabulary is worth more than one that only says the word was
 * wrong. */
typedef struct
{
    const gchar *word;
    gint         value;
} WordEntry;

static const WordEntry dash_words[] =
{
    { "solid",   PN_FIGURE_DASH_SOLID   },
    { "dot",     PN_FIGURE_DASH_DOT     },
    { "dash",    PN_FIGURE_DASH_DASH    },
    { "dashdot", PN_FIGURE_DASH_DASHDOT },
};

/* "center" is here beside "centre" for the same reason pn-color.c
 * carries both "grey" and "gray": the word is unavoidable and the
 * spelling is not worth an error message. */
static const WordEntry halign_words[] =
{
    { "left",   PN_FIGURE_HALIGN_LEFT   },
    { "centre", PN_FIGURE_HALIGN_CENTRE },
    { "center", PN_FIGURE_HALIGN_CENTRE },
    { "right",  PN_FIGURE_HALIGN_RIGHT  },
};

static const WordEntry valign_words[] =
{
    { "top",      PN_FIGURE_VALIGN_TOP      },
    { "middle",   PN_FIGURE_VALIGN_MIDDLE   },
    { "baseline", PN_FIGURE_VALIGN_BASELINE },
    { "bottom",   PN_FIGURE_VALIGN_BOTTOM   },
};

/* Looks argument @n up in @words, storing what it names in its .word.
 * @vocabulary is the message to give when it is not there — spelled
 * out rather than generated, because the two centre spellings would
 * make a generated list read oddly. */
static gboolean
parse_word (
        const PnFigureStatement *statement,
        guint                    n,
        const WordEntry         *words,
        gsize                    n_words,
        const gchar             *vocabulary,
        GPtrArray               *errors)
{
    PnFigureArg *arg = g_ptr_array_index (statement->args, n);
    gsize        i;

    for (i = 0; i < n_words; i++)
        if (g_strcmp0 (words[i].word, arg->text) == 0)
        {
            arg->word = words[i].value;
            return TRUE;
        }

    report_at (errors, statement->source, arg->offset, "expected %s",
               vocabulary);
    return FALSE;
}

/* Validates a `text` format and counts its conversions.  Only "%%" and
 * the numeric conversions are allowed through, with optional flags,
 * width and precision — no "*", which would eat an argument, and no
 * length modifier, which would change the argument's type (80.7c). */
static gboolean
parse_format (
        const PnFigureStatement *statement,
        GPtrArray               *errors)
{
    PnFigureArg *arg    = g_ptr_array_index (statement->args, 2);
    const gchar *format = arg->text;
    guint        wanted = statement->args->len - 3;
    guint        found  = 0;
    gsize        i;

    for (i = 0; format[i] != '\0'; i++)
    {
        gsize start;

        if (format[i] != '%')
            continue;

        start = i++;

        if (format[i] == '%')
            continue;

        while (format[i] == '-' || format[i] == '+' || format[i] == ' '
               || format[i] == '#' || format[i] == '0')
            i++;
        while (g_ascii_isdigit (format[i]))
            i++;
        if (format[i] == '.')
        {
            i++;
            while (g_ascii_isdigit (format[i]))
                i++;
        }

        /* The NUL test comes first on purpose: strchr() finds the
         * terminator of its own search string and would say yes. */
        if (format[i] == '\0' || strchr ("feEgGF", format[i]) == NULL)
        {
            /* Show it from the "%" to whatever went wrong, which is
             * what the person has to go and look at. */
            gsize end = format[i] == '\0' ? i : i + 1;

            report_at (errors, statement->source, arg->offset,
                       "unsupported conversion \"%.*s\"",
                       (int) (end - start), format + start);
            return FALSE;
        }

        found++;
    }

    if (found != wanted)
    {
        report_at (errors, statement->source, arg->offset,
                   "format needs %u value%s, not %u",
                   found, found == 1 ? "" : "s", wanted);
        return FALSE;
    }

    arg->word = (gint) found;
    return TRUE;
}

/* Gives one statement's literals their meaning. */
static gboolean
parse_statement_literals (
        PnFigureStatement *statement,
        GPtrArray         *errors)
{
    switch (statement->verb)
    {
    case PN_FIGURE_VERB_COLOR:
    case PN_FIGURE_VERB_FILL:
    {
        PnFigureArg *arg;

        /* Only the one-argument spelling carries a literal; the other
         * is three or four expressions evaluated per frame. */
        if (statement->args->len != 1)
            return TRUE;

        arg = g_ptr_array_index (statement->args, 0);
        if (pn_color_parse (&arg->color, arg->text))
            return TRUE;

        report_at (errors, statement->source, arg->offset,
                   "unknown colour \"%s\"", arg->text);
        return FALSE;
    }

    case PN_FIGURE_VERB_DASH:
        return parse_word (statement, 0, dash_words,
                           G_N_ELEMENTS (dash_words),
                           "\"solid\", \"dot\", \"dash\" or \"dashdot\"",
                           errors);

    case PN_FIGURE_VERB_ALIGN:
        if (!parse_word (statement, 0, halign_words,
                         G_N_ELEMENTS (halign_words),
                         "\"left\", \"centre\" or \"right\"", errors))
            return FALSE;
        if (statement->args->len < 2)
            return TRUE;
        return parse_word (statement, 1, valign_words,
                           G_N_ELEMENTS (valign_words),
                           "\"top\", \"middle\", \"baseline\" or \"bottom\"",
                           errors);

    case PN_FIGURE_VERB_TEXT:
        return parse_format (statement, errors);

    default:
        return TRUE;
    }
}

gboolean
pn_figure_parse_literals (
        GPtrArray *statements,
        GPtrArray *errors)
{
    gboolean ok = TRUE;
    guint    i  = 0;

    g_return_val_if_fail (statements != NULL, FALSE);

    while (i < statements->len)
    {
        PnFigureStatement *statement = g_ptr_array_index (statements, i);

        if (statement->kind == PN_FIGURE_STATEMENT_ASSIGNMENT
            || parse_statement_literals (statement, errors))
        {
            i++;
            continue;
        }

        g_ptr_array_remove_index (statements, i);
        ok = FALSE;
    }

    return ok;
}

/* ================================================================== */
/*  Expressions                                                       */
/* ================================================================== */

/* The language's own constants (`pi`, `e`), which are not names a
 * program expects from outside: they fold like numbers and never reach
 * the collected list.  The table itself moved into the calculator
 * language where it belongs (TODO #81.6, which was #80.3d's own
 * prediction); PnVarStore now resolves them as a fallback, so this file
 * no longer binds them — it only needs to RECOGNISE them, so that a
 * folded `pi` is not collected as a free name and, more sharply, so
 * that bind_frame() below does not zero-fill one.  A zero-fill would
 * win: a binding shadows the fallback. */
static gboolean
is_figure_constant (
        const gchar *name)
{
    return pn_expr_constant_lookup (name, NULL);
}

/* TRUE when @node reads nothing that can change between frames, so its
 * value can be settled now and never looked at again.  An assignment
 * never folds: it has to run each frame to bind its name. */
static gboolean
is_foldable (
        const PnExprNode *node)
{
    if (node == NULL)
        return TRUE;

    switch (node->type)
    {
    case PN_EXPR_NODE_VARIABLE:
        return is_figure_constant (node->name);

    case PN_EXPR_NODE_ASSIGN:
        return FALSE;

    default:
        return is_foldable (node->left) && is_foldable (node->right);
    }
}

/* Adds every variable name @node READS to @names.  A function's name is
 * not a variable, and neither is an assignment's target — only what the
 * assignment's value reads. */
static void
collect_names (
        const PnExprNode *node,
        GHashTable       *names)
{
    if (node == NULL)
        return;

    if (node->type == PN_EXPR_NODE_VARIABLE)
    {
        if (!is_figure_constant (node->name))
            g_hash_table_add (names, g_strdup (node->name));
        return;
    }

    collect_names (node->left, names);
    collect_names (node->right, names);
}

/* A store for folding: empty, because the constants a foldable
 * expression can need are the only thing it reads and PnVarStore
 * resolves those itself now.  Kept as a named function anyway — the
 * call sites say what the store is FOR, and the day folding needs a
 * binding it has one place to appear. */
static PnVarStore *
fold_store_new (void)
{
    return pn_var_store_new ();
}

/* Splits the calculator's " at position N" tail off @message, so the
 * position can go into the column where it belongs instead of sitting
 * in the text next to a different one.  Returns the 1-based position,
 * or 0 when the message does not carry one — in which case the caller
 * falls back on the fragment's own position, which is never wrong,
 * only less precise. */
static gint
take_position (
        gchar *message)
{
    static const gchar tail[] = " at position ";
    gchar             *at     = g_strrstr (message, tail);
    const gchar       *digits;
    gint               position;
    gchar             *end;

    if (at == NULL)
        return 0;

    digits   = at + strlen (tail);
    position = (gint) g_ascii_strtoll (digits, &end, 10);

    if (end == digits || *end != '\0' || position < 1)
        return 0;

    *at = '\0';
    return position;
}

/* Parses @text with @parser, reporting a failure at @base plus wherever
 * in @text the parser stopped. */
static PnExprNode *
parse_fragment (
        PnExprParser            *parser,
        const gchar             *text,
        const PnFigureStatement *statement,
        gsize                    base,
        GPtrArray               *errors)
{
    GError     *error = NULL;
    PnExprNode *ast   = pn_expr_parser_parse (parser, text, &error);
    gint        position;

    if (ast != NULL)
        return ast;

    position = take_position (error->message);
    report_at (errors, statement->source,
               base + (position > 0 ? (gsize) position - 1 : 0),
               "%s", error->message);
    g_error_free (error);
    return NULL;
}

/* Parses one statement's expressions, folding what cannot change. */
static gboolean
parse_statement_expressions (
        PnFigureStatement *statement,
        PnExprParser      *parser,
        PnVarStore        *folder,
        GPtrArray         *errors)
{
    guint i;

    if (statement->kind == PN_FIGURE_STATEMENT_ASSIGNMENT)
    {
        /* The whole line, which is what already returns an ASSIGN node
         * the evaluator knows how to bind (80.2). */
        statement->ast = parse_fragment (parser, statement->source->text,
                                         statement, 0, errors);
        return statement->ast != NULL;
    }

    for (i = 0; i < statement->args->len; i++)
    {
        PnFigureArg *arg = g_ptr_array_index (statement->args, i);
        GError      *error = NULL;
        gdouble      value;

        if (arg->kind != PN_FIGURE_ARG_EXPRESSION)
            continue;

        arg->ast = parse_fragment (parser, arg->text, statement,
                                   arg->offset, errors);
        if (arg->ast == NULL)
            return FALSE;

        if (!is_foldable (arg->ast))
            continue;

        if (!pn_var_store_evaluate (folder, arg->ast, &value, &error))
        {
            /* Nothing a frame could cure: the only way a constant
             * expression fails is a function that does not exist. */
            report_at (errors, statement->source, arg->offset,
                       "%s", error->message);
            g_error_free (error);
            return FALSE;
        }

        /* A non-finite constant is a VALUE, not a program error
         * (80.10b): the resolver skips the statement, and says so. */
        pn_expr_node_free (arg->ast);
        arg->ast    = NULL;
        arg->value  = value;
        arg->folded = TRUE;
    }

    return TRUE;
}

gboolean
pn_figure_parse_expressions (
        GPtrArray *statements,
        GPtrArray *errors)
{
    PnExprParser *parser;
    PnVarStore   *folder;
    gboolean      ok = TRUE;
    guint         i  = 0;

    g_return_val_if_fail (statements != NULL, FALSE);

    parser = pn_expr_parser_new ();
    folder = fold_store_new ();

    while (i < statements->len)
    {
        PnFigureStatement *statement = g_ptr_array_index (statements, i);

        if (parse_statement_expressions (statement, parser, folder, errors))
        {
            i++;
            continue;
        }

        g_ptr_array_remove_index (statements, i);
        ok = FALSE;
    }

    g_object_unref (folder);
    g_object_unref (parser);
    return ok;
}

/* g_ptr_array_sort() hands the comparator the ELEMENT SLOTS, not the
 * elements, and the project's minimum GLib (2.40) has no
 * g_ptr_array_sort_values(). */
static gint
name_sort_cmp (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

GPtrArray *
pn_figure_free_names (
        GPtrArray *statements)
{
    GHashTable     *seen;
    GPtrArray      *names;
    GHashTableIter  iter;
    gpointer        key;
    guint           i;

    names = g_ptr_array_new_with_free_func (g_free);
    g_return_val_if_fail (statements != NULL, names);

    seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    for (i = 0; i < statements->len; i++)
    {
        const PnFigureStatement *statement = g_ptr_array_index (statements, i);
        guint                    n;

        collect_names (statement->ast, seen);

        for (n = 0; n < statement->args->len; n++)
        {
            const PnFigureArg *arg = g_ptr_array_index (statement->args, n);

            collect_names (arg->ast, seen);
        }
    }

    g_hash_table_iter_init (&iter, seen);
    while (g_hash_table_iter_next (&iter, &key, NULL))
        g_ptr_array_add (names, g_strdup (key));

    g_ptr_array_sort (names, name_sort_cmp);
    g_hash_table_destroy (seen);
    return names;
}

/* ================================================================== */
/*  Reporting                                                         */
/* ================================================================== */

gchar *
pn_figure_errors_to_string (
        GPtrArray *errors)
{
    const PnFigureError *first = NULL;
    guint                i;

    if (errors == NULL || errors->len == 0)
        return NULL;

    /* Earliest in the PROGRAM, which is not the order the stages found
     * them in: an unterminated string on line 5 is found before a bad
     * verb on line 2, and a person reads top to bottom. */
    for (i = 0; i < errors->len; i++)
    {
        const PnFigureError *error = g_ptr_array_index (errors, i);

        if (first == NULL
            || error->line < first->line
            || (error->line == first->line && error->column < first->column))
            first = error;
    }

    if (errors->len == 1)
        return g_strdup_printf ("line %d, column %d: %s",
                                first->line, first->column, first->message);

    return g_strdup_printf ("line %d, column %d: %s\n"
                            "%u errors, first on line %d",
                            first->line, first->column, first->message,
                            errors->len, first->line);
}

/* ================================================================== */
/*  The binding snapshot                                              */
/* ================================================================== */

struct _PnFigureSnapshot
{
    GHashTable *values; /* gchar * -> PnExprValue *, both owned */
};

static void
snapshot_value_free (
        gpointer data)
{
    PnExprValue *value = data;

    pn_expr_value_clear (value);
    g_free (value);
}

PnFigureSnapshot *
pn_figure_snapshot_new (void)
{
    PnFigureSnapshot *self = g_new0 (PnFigureSnapshot, 1);

    self->values = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, snapshot_value_free);
    return self;
}

void
pn_figure_snapshot_free (
        PnFigureSnapshot *self)
{
    if (self == NULL)
        return;

    g_hash_table_destroy (self->values);
    g_free (self);
}

void
pn_figure_snapshot_clear (
        PnFigureSnapshot *self)
{
    g_return_if_fail (self != NULL);

    g_hash_table_remove_all (self->values);
}

void
pn_figure_snapshot_set (
        PnFigureSnapshot *self,
        const gchar      *name,
        gdouble           value)
{
    PnExprValue *slot;

    g_return_if_fail (self != NULL);
    g_return_if_fail (name != NULL);

    slot         = g_new0 (PnExprValue, 1);
    slot->scalar = value;
    g_hash_table_insert (self->values, g_strdup (name), slot);
}

void
pn_figure_snapshot_set_vector (
        PnFigureSnapshot *self,
        const gchar      *name,
        PnVector         *vec)
{
    PnExprValue *slot;

    g_return_if_fail (self != NULL);
    g_return_if_fail (name != NULL);
    g_return_if_fail (vec != NULL);

    slot      = g_new0 (PnExprValue, 1);
    slot->vec = g_object_ref (vec);
    g_hash_table_insert (self->values, g_strdup (name), slot);
}

/* The shortest input, not the longest, although vector OP vector takes
 * the LONGER length with the tail passing through verbatim (80.3f).
 * That tail is arithmetic's way of saying something rather than
 * failing; a figure would say it as a picture in which, from some frame
 * on, a lever is still swinging on data it no longer has.  Every frame
 * of the shortest film is drawn from real values of every input.  The
 * film therefore does not match the arithmetic exactly, and cannot:
 * `a * b` of a 2-vector and a 3-vector is two frames here, and the
 * third element of the product is never shown. */
guint
pn_figure_frame_count (
        GPtrArray              *free_names,
        const PnFigureSnapshot *snapshot,
        guint                   frames)
{
    guint count = frames;
    guint n;

    for (n = 0; snapshot != NULL && free_names != NULL
                && n < free_names->len; n++)
    {
        const PnExprValue *value;

        value = g_hash_table_lookup (snapshot->values,
                                     g_ptr_array_index (free_names, n));
        if (value == NULL || value->vec == NULL)
            continue;

        if (count == 0 || pn_vector_get_len (value->vec) < count)
            count = pn_vector_get_len (value->vec);

        if (count == 0)
            return 0;
    }

    return count == 0 ? 1 : count;
}

/* Puts one snapshot entry into the store the frame will run against. */
static void
snapshot_apply_one (
        gpointer key,
        gpointer data,
        gpointer user_data)
{
    const gchar *name  = key;
    PnExprValue *value = data;
    PnVarStore  *store = user_data;

    if (value->vec != NULL)
        pn_var_store_set_vector (store, name, value->vec);
    else
        pn_var_store_set (store, name, value->scalar);
}

/* The per-frame cycle of 80.2 rule 13, refined by 80.8(e): clear, then
 * the latched inputs, then zero for whatever the program reads and
 * nothing has supplied.  The constants no longer need a step of their
 * own — PnVarStore resolves them beneath every binding (#81.6) — but
 * they still need EXCLUDING from the zero-fill, which is the one place
 * 80.3(c)'s precedence still has to be spelled out here.  That zero-fill
 * step is not tidiness — an unbound name FAILS an evaluation in
 * PnVarStore rather than reading as 0, so without it an unwired figure
 * would draw nothing at all (80.2 rule 12).
 *
 * The order is 80.3(c)'s, and the reason it is written down there is
 * that getting it wrong makes `pi` silently 0: a figure that still
 * draws, just wrongly.  Assignments bind themselves as they execute,
 * which is why they come last and are not this function's business. */
/* TRUE when @name is in the sorted, NULL-safe @free_names. */
static gboolean
names_contain (
        GPtrArray   *free_names,
        const gchar *name)
{
    guint n;

    for (n = 0; free_names != NULL && n < free_names->len; n++)
        if (g_strcmp0 (g_ptr_array_index (free_names, n), name) == 0)
            return TRUE;

    return FALSE;
}

/* Binds one animation variable, unless the program does not read it --
 * the free-name check 80.17(e) promised, which spares a still program
 * two vectors it would never look at -- or an input already supplies
 * it: an input is a wire somebody named, and wins as it does over the
 * zero-fill (80.3c).  A film of one frame binds a plain 0.  A film of
 * none binds nothing, and the zero-fill makes it 0: the empty vector
 * that made it so is reported where it lands (82.1e).
 *
 * Returns %TRUE when it bound @name, so the zero-fill leaves it be. */
static gboolean
bind_animation (
        PnVarStore             *store,
        const PnFigureSnapshot *snapshot,
        GPtrArray              *free_names,
        const gchar            *name,
        guint                   count,
        gdouble                 step)
{
    gdouble  *values;
    PnVector *vec;
    guint     k;

    if (!names_contain (free_names, name))
        return FALSE;
    if (snapshot != NULL && g_hash_table_contains (snapshot->values, name))
        return FALSE;
    if (count == 0)
        return FALSE;

    if (count == 1)
    {
        pn_var_store_set (store, name, 0.0);
        return TRUE;
    }

    values = g_new (gdouble, count);
    for (k = 0; k < count; k++)
        values[k] = (gdouble) k * step;

    vec = pn_vector_new_take (values, count);
    pn_var_store_set_vector (store, name, vec);
    g_object_unref (vec);
    return TRUE;
}

static void
bind_frame (
        PnVarStore             *store,
        const PnFigureSnapshot *snapshot,
        GPtrArray              *free_names,
        const PnFigureFilm     *film)
{
    gboolean bound_frame;
    gboolean bound_time;
    guint    count;
    guint    n;

    pn_var_store_clear (store);

    if (snapshot != NULL)
        g_hash_table_foreach (snapshot->values, snapshot_apply_one, store);

    /* After the inputs, before the zero-fill: 80.3(c)'s order. */
    count = pn_figure_frame_count (free_names, snapshot,
                                   film != NULL ? film->frames : 0);
    bound_frame = bind_animation (store, snapshot, free_names,
                                  PN_FIGURE_FRAME_NAME, count, 1.0);
    if (count > 1)
    {
        /* 82.5: a loop's frame after the last is t = 1 = t = 0, so it
         * steps by 1/N and never shows the seam twice; once and
         * ping-pong step by 1/(N-1) and end exactly on 1. */
        gboolean loop = film == NULL || film->mode == PN_FIGURE_PLAY_LOOP;

        bound_time = bind_animation (store, snapshot, free_names,
                                     PN_FIGURE_TIME_NAME, count,
                                     1.0 / (gdouble) (loop ? count
                                                           : count - 1));
    }
    else
        bound_time = bind_animation (store, snapshot, free_names,
                                     PN_FIGURE_TIME_NAME, count, 0.0);

    for (n = 0; free_names != NULL && n < free_names->len; n++)
    {
        const gchar *name = g_ptr_array_index (free_names, n);

        if (is_figure_constant (name))
            continue;
        if (snapshot != NULL
            && g_hash_table_contains (snapshot->values, name))
            continue;
        if ((bound_frame && strcmp (name, PN_FIGURE_FRAME_NAME) == 0)
            || (bound_time && strcmp (name, PN_FIGURE_TIME_NAME) == 0))
            continue;

        pn_var_store_set (store, name, 0.0);
    }
}

/* ================================================================== */
/*  The view transform                                                */
/* ================================================================== */

/* The default window (80.4c): fixed, round and documented, never fitted
 * to the drawing — an auto window breathes as the values change, which
 * turns every animation into an accidental zoom. */
#define FIGURE_VIEW_XMIN 0.0
#define FIGURE_VIEW_YMIN 0.0
#define FIGURE_VIEW_XMAX 100.0
#define FIGURE_VIEW_YMAX 100.0

/* A stroked line never thinner than this many device pixels, so a fine
 * line does not vanish at rest (80.4b).  `width 0` is the deliberate
 * escape and stays a hairline. */
#define FIGURE_MIN_WIDTH 0.75

/* The window fitted into a device rectangle: what 80.4 works out, kept
 * as the numbers the mapping actually uses.  @sx and @sy are SIGNED, so
 * reversed bounds are nothing special, and @s is the length scale that
 * widths, radii and font sizes are multiplied by. */
typedef struct
{
    gdouble xmin, ymin, xmax, ymax; /* the window, user units          */
    gdouble ox, oy;                 /* device corner of the fitted box */
    gdouble boxw, boxh;             /* its size, device units          */
    gdouble sx, sy;                 /* device units per user unit      */
    gdouble s;                      /* the length scale                */
} View;

/* The device rectangle a frame is being resolved into. */
typedef struct
{
    gdouble  x, y, w, h;
    gboolean stretch;
} Frame;

/* Fits the window into @frame.  Returns %FALSE for a DEGENERATE window
 * — zero extent, NaN, infinite — which is a runtime value and not a
 * program error: a knob winding through zero must not latch the node
 * red, so the caller skips the statement and the previous window stands
 * (80.4d). */
static gboolean
view_set (
        View        *self,
        const Frame *frame,
        gdouble      xmin,
        gdouble      ymin,
        gdouble      xmax,
        gdouble      ymax)
{
    gdouble ex = xmax - xmin;
    gdouble ey = ymax - ymin;
    gdouble kx, ky;

    if (!isfinite (xmin) || !isfinite (ymin)
        || !isfinite (ex) || !isfinite (ey)
        || ex == 0.0 || ey == 0.0)
        return FALSE;

    if (frame->stretch)
    {
        kx = frame->w / fabs (ex);
        ky = frame->h / fabs (ey);
    }
    else
    {
        kx = ky = MIN (frame->w / fabs (ex), frame->h / fabs (ey));
    }

    self->xmin = xmin;
    self->ymin = ymin;
    self->xmax = xmax;
    self->ymax = ymax;

    /* The flip lives here and not in the cairo matrix (80.4a): a
     * negative-y CTM mirrors every glyph the `text` verb draws. */
    self->sx   = ex > 0.0 ?  kx : -kx;
    self->sy   = ey > 0.0 ? -ky :  ky;
    self->boxw = kx * fabs (ex);
    self->boxh = ky * fabs (ey);
    self->ox   = frame->x + (frame->w - self->boxw) / 2.0;
    self->oy   = frame->y + (frame->h - self->boxh) / 2.0;
    self->s    = MIN (kx, ky);
    return TRUE;
}

static gdouble
view_map_x (
        const View *self,
        gdouble     u)
{
    return self->ox + (u - self->xmin) * self->sx;
}

static gdouble
view_map_y (
        const View *self,
        gdouble     v)
{
    return self->oy + self->boxh + (v - self->ymin) * self->sy;
}

/* A user angle in DEVICE degrees, plus which way the sweep goes.
 *
 * Our y flip means a counter-clockwise user sweep is a clockwise device
 * one, which is 80.6(d)'s gotcha; a reversed bound flips it back, and
 * reversing both flips the angles instead of the direction.  The map is
 * always device = @out_k * user + @out_phi, linear on purpose: going
 * through atan2() would wrap 360 degrees to 0 and turn a full circle
 * into nothing. */
static void
view_angle_map (
        const View *self,
        gdouble    *out_k,
        gdouble    *out_phi)
{
    *out_k   = ((self->sx > 0.0) == (self->sy < 0.0)) ? -1.0 : 1.0;
    *out_phi = self->sx < 0.0 ? 180.0 : 0.0;
}

/* ================================================================== */
/*  The pen state machine                                             */
/* ================================================================== */

/* The whole state vector of 80.5, in USER units — which is the point:
 * `width 2` means two user units whatever the view is, so a `view` that
 * changes the scale changes what the same pen state comes to in device
 * units, and the resolver re-emits it.
 *
 * It is reset to these defaults at the start of every frame (80.5g);
 * persisting it would make frame N depend on frame N-1, which is the
 * bug class a film has no way to debug. */
typedef struct
{
    PnColor        stroke;
    PnColor        fill;
    gboolean       filling;
    gdouble        width;      /* user units, 0 = a device hairline   */
    PnFigureDash   dash;
    gdouble        dash_scale;
    gdouble        font;       /* user units                          */
    PnFigureHAlign halign;
    PnFigureVAlign valign;
    gdouble        px, py;     /* the pen, user units                 */
    View           view;
} Pen;

/* The dash patterns of 80.5(d), in user units before the scale. */
static const struct
{
    gint    n;
    gdouble on_off[4];
}
dash_patterns[] =
{
    { 0, { 0.0, 0.0, 0.0, 0.0 } }, /* solid   */
    { 2, { 0.5, 1.5, 0.0, 0.0 } }, /* dot     */
    { 2, { 3.0, 2.0, 0.0, 0.0 } }, /* dash    */
    { 4, { 3.0, 2.0, 0.5, 2.0 } }, /* dashdot */
};

/* The default font size, in user units.  Nothing decides this but
 * taste: five units in the default hundred-unit window is a label about
 * a twentieth of the plate high, which is what a plate's labels are. */
#define FIGURE_DEFAULT_FONT 5.0

static void
pen_init (
        Pen         *self,
        const Frame *frame)
{
    static const PnColor black = { 0.0, 0.0, 0.0, 1.0 };

    self->stroke     = black;
    self->fill       = black;
    self->filling    = FALSE;
    self->width      = 1.0;
    self->dash       = PN_FIGURE_DASH_SOLID;
    self->dash_scale = 1.0;
    self->font       = FIGURE_DEFAULT_FONT;
    self->halign     = PN_FIGURE_HALIGN_CENTRE; /* 80.7g */
    self->valign     = PN_FIGURE_VALIGN_MIDDLE;
    self->px         = 0.0;                     /* 80.6a */
    self->py         = 0.0;

    view_set (&self->view, frame, FIGURE_VIEW_XMIN, FIGURE_VIEW_YMIN,
              FIGURE_VIEW_XMAX, FIGURE_VIEW_YMAX);
}

/* ================================================================== */
/*  The display list                                                  */
/* ================================================================== */

static void
figure_op_free (
        gpointer data)
{
    PnFigureOp *self = data;

    if (self == NULL)
        return;

    if (self->points != NULL)
        g_array_unref (self->points);
    g_free (self->text);
    g_free (self);
}

/* An empty display list with the right element free function — a frame
 * about to be filled, or one that drew nothing at all. */
static GPtrArray *
figure_ops_new (void)
{
    return g_ptr_array_new_with_free_func (figure_op_free);
}

static PnFigureOp *
op_add (
        GPtrArray      *ops,
        PnFigureOpKind  kind,
        gint            line)
{
    PnFigureOp *op = g_new0 (PnFigureOp, 1);

    op->kind = kind;
    op->line = line;
    g_ptr_array_add (ops, op);
    return op;
}

/* The three state operations whose device numbers depend on the scale,
 * which is why each is a function: a `view` statement emits them again.
 */
static void
emit_width (
        GPtrArray *ops,
        const Pen *pen,
        gint       line)
{
    PnFigureOp *op = op_add (ops, PN_FIGURE_OP_WIDTH, line);

    /* `width 0` is the PostScript hairline and the one escape from
     * 80.4(b)'s rule that widths scale with the view (80.5c). */
    op->value = pen->width == 0.0
                ? 0.0
                : MAX (pen->width * pen->view.s, FIGURE_MIN_WIDTH);
}

static void
emit_dash (
        GPtrArray *ops,
        const Pen *pen,
        gint       line)
{
    PnFigureOp *op = op_add (ops, PN_FIGURE_OP_DASH, line);
    gint        i;

    op->n_dashes = dash_patterns[pen->dash].n;
    for (i = 0; i < op->n_dashes; i++)
        op->dashes[i] = dash_patterns[pen->dash].on_off[i]
                        * pen->dash_scale * pen->view.s;
}

static void
emit_font (
        GPtrArray *ops,
        const Pen *pen,
        gint       line)
{
    PnFigureOp *op = op_add (ops, PN_FIGURE_OP_FONT, line);

    op->value = pen->font * pen->view.s;
}

/* The whole pen state, at the top of every frame.  It is not padding:
 * it is 80.5(g)'s reset made visible, and it is what lets the painter
 * hold no defaults of its own — the list describes the frame from
 * nothing, so a figure cannot inherit a colour or a width from the
 * frame before it even by accident. */
static void
emit_pen (
        GPtrArray *ops,
        const Pen *pen,
        gint       line)
{
    PnFigureOp *op;

    op_add (ops, PN_FIGURE_OP_COLOR, line)->color = pen->stroke;
    op_add (ops, PN_FIGURE_OP_NOFILL, line);
    emit_width (ops, pen, line);
    emit_dash (ops, pen, line);
    emit_font (ops, pen, line);

    op         = op_add (ops, PN_FIGURE_OP_ALIGN, line);
    op->halign = pen->halign;
    op->valign = pen->valign;
}

static void
emit_view (
        GPtrArray *ops,
        const Pen *pen,
        gint       line)
{
    PnFigureOp *op = op_add (ops, PN_FIGURE_OP_VIEW, line);

    op->window[0] = pen->view.xmin;
    op->window[1] = pen->view.ymin;
    op->window[2] = pen->view.xmax;
    op->window[3] = pen->view.ymax;
    op->x         = pen->view.ox;
    op->y         = pen->view.oy;
    op->w         = pen->view.boxw;
    op->h         = pen->view.boxh;
    op->scale     = pen->view.s;
    op->scale_x   = fabs (pen->view.sx) / pen->view.s;
    op->scale_y   = fabs (pen->view.sy) / pen->view.s;
}

/* A point list in device units, from @n user pairs. */
static GArray *
device_points (
        const View    *view,
        const gdouble *values,
        guint          n)
{
    GArray *points = g_array_sized_new (FALSE, FALSE, sizeof (gdouble), n);
    guint   i;

    for (i = 0; i + 1 < n; i += 2)
    {
        gdouble xy[2];

        xy[0] = view_map_x (view, values[i]);
        xy[1] = view_map_y (view, values[i + 1]);
        g_array_append_vals (points, xy, 2);
    }

    return points;
}

static void
emit_segment (
        GPtrArray  *ops,
        const View *view,
        gint        line,
        gdouble     x1,
        gdouble     y1,
        gdouble     x2,
        gdouble     y2)
{
    PnFigureOp *op     = op_add (ops, PN_FIGURE_OP_LINE, line);
    gdouble     ends[4];

    ends[0] = x1;
    ends[1] = y1;
    ends[2] = x2;
    ends[3] = y2;
    op->points = device_points (view, ends, 4);
}

/* ================================================================== */
/*  The resolver                                                      */
/* ================================================================== */

/* The `text` verb's format, filled in.  It was validated conversion by
 * conversion at parse time (80.7c), so nothing here has to defend
 * itself — but every number still goes through g_ascii_formatd(), or a
 * machine with a comma decimal separator would draw "1,5" and fail
 * every test in 80.12. */
static gchar *
format_text (
        const gchar   *format,
        const gdouble *values,
        guint          n)
{
    GString *out  = g_string_new (NULL);
    guint    used = 0;
    gsize    i;

    for (i = 0; format[i] != '\0'; i++)
    {
        gchar spec[64];
        gchar buf[512];
        gsize start;
        gsize len;

        if (format[i] != '%')
        {
            g_string_append_c (out, format[i]);
            continue;
        }

        start = i++;

        if (format[i] == '%')
        {
            g_string_append_c (out, '%');
            continue;
        }

        while (format[i] != '\0' && strchr ("-+ #0", format[i]) != NULL)
            i++;
        while (g_ascii_isdigit (format[i]))
            i++;
        if (format[i] == '.')
        {
            i++;
            while (g_ascii_isdigit (format[i]))
                i++;
        }

        len = i - start + 1;
        if (len >= sizeof spec)
        {
            /* A width with fifty digits in it: nothing can be made of
             * it, and truncating the spec would make it a lie. */
            used++;
            continue;
        }

        memcpy (spec, format + start, len);
        spec[len] = '\0';

        if (g_ascii_formatd (buf, sizeof buf, spec,
                             used < n ? values[used] : 0.0) != NULL)
            g_string_append (out, buf);
        used++;
    }

    return g_string_free (out, FALSE);
}

/* Evaluates every expression argument into @values, which the caller
 * sized to the argument count.  A string argument leaves its slot at 0.
 *
 * A vector argument is the film (80.16): it contributes its element
 * @index, which is how frame i of a film is drawn without running the
 * program per frame -- the store is elementwise, so the whole film was
 * already computed, and this is the one place that picks a frame out of
 * it (82.2).
 *
 * The one failure here is 80.10's class (c): an evaluation that could
 * not be done at all, or a vector with no element @index -- an empty
 * one, or a caller asking for a frame past the count
 * pn_figure_frame_count() gave it.  Winding a knob cures neither, so
 * both stop the frame. */
static gboolean
eval_args (
        const PnFigureStatement *statement,
        PnVarStore              *store,
        guint                    index,
        gdouble                 *values,
        GPtrArray               *errors)
{
    guint i;

    for (i = 0; i < statement->args->len; i++)
    {
        PnFigureArg *arg   = g_ptr_array_index (statement->args, i);
        PnExprValue  value = { NULL, 0.0 };
        GError      *error = NULL;

        values[i] = 0.0;

        if (arg->kind != PN_FIGURE_ARG_EXPRESSION)
            continue;

        if (arg->folded)
        {
            values[i] = arg->value;
            continue;
        }

        if (!pn_var_store_evaluate_value (store, arg->ast, &value, &error))
        {
            report_at (errors, statement->source, arg->offset,
                       "%s", error->message);
            g_error_free (error);
            return FALSE;
        }

        if (value.vec != NULL)
        {
            gsize len = pn_vector_get_len (value.vec);

            if (index >= len)
            {
                pn_expr_value_clear (&value);
                if (len == 0)
                    report_at (errors, statement->source, arg->offset,
                               "empty vector argument; nothing to draw");
                else
                    report_at (errors, statement->source, arg->offset,
                               "frame %u is past the end of a %"
                               G_GSIZE_FORMAT "-element vector",
                               index, len);
                return FALSE;
            }

            values[i] = pn_vector_get_data (value.vec)[index];
            pn_expr_value_clear (&value);
            continue;
        }

        values[i] = value.scalar;
    }

    return TRUE;
}

/* TRUE when every expression argument came out a finite number.  A NaN
 * or an infinity is a VALUE, not a bug (80.10b), so the caller skips
 * the statement and says so rather than failing the frame. */
static gboolean
args_are_finite (
        const PnFigureStatement *statement,
        const gdouble           *values)
{
    guint i;

    for (i = 0; i < statement->args->len; i++)
    {
        const PnFigureArg *arg = g_ptr_array_index (statement->args, i);

        if (arg->kind == PN_FIGURE_ARG_EXPRESSION && !isfinite (values[i]))
            return FALSE;
    }

    return TRUE;
}

static void
emit_skip (
        GPtrArray               *ops,
        const PnFigureStatement *statement,
        const gchar             *reason)
{
    gint        line = 0;
    PnFigureOp *op;

    pn_figure_line_locate (statement->source, 0, &line, NULL);
    op       = op_add (ops, PN_FIGURE_OP_SKIP, line);
    op->text = g_strdup (reason);
}

/* The colour of a `color` or `fill` statement in either of its two
 * spellings (80.5): the one quoted literal the front end already
 * parsed, or three-to-four expressions just evaluated. */
static PnColor
statement_colour (
        const PnFigureStatement *statement,
        const gdouble           *values)
{
    PnColor            color;
    const PnFigureArg *arg;

    if (statement->args->len == 1)
    {
        arg = g_ptr_array_index (statement->args, 0);
        return arg->color;
    }

    color.red   = values[0];
    color.green = values[1];
    color.blue  = values[2];
    color.alpha = statement->args->len > 3 ? values[3] : 1.0;
    return color;
}

/* Runs one statement.  Returns %FALSE only for 80.10's class (c), the
 * error that empties the whole figure; a skipped statement is a %TRUE
 * that drew nothing. */
static gboolean
resolve_statement (
        PnFigureStatement *statement,
        PnVarStore        *store,
        guint              index,
        Pen               *pen,
        const Frame       *frame,
        GPtrArray         *ops,
        GPtrArray         *errors)
{
    guint      n = statement->args->len;
    gdouble   *values;
    gint       line = 0;
    gboolean   ok   = TRUE;

    pn_figure_line_locate (statement->source, 0, &line, NULL);

    if (statement->kind == PN_FIGURE_STATEMENT_ASSIGNMENT)
    {
        PnExprValue value = { NULL, 0.0 };
        GError     *error = NULL;

        /* An assignment leaves no operation: it binds a name, and its
         * effect is already in the numbers of what follows.  A vector
         * binding is fine here and only becomes an error where it
         * reaches an argument. */
        if (!pn_var_store_evaluate_value (store, statement->ast,
                                          &value, &error))
        {
            report_at (errors, statement->source, 0, "%s", error->message);
            g_error_free (error);
            return FALSE;
        }

        pn_expr_value_clear (&value);
        return TRUE;
    }

    values = g_new0 (gdouble, n + 1);

    if (!eval_args (statement, store, index, values, errors))
    {
        g_free (values);
        return FALSE;
    }

    if (!args_are_finite (statement, values))
    {
        emit_skip (ops, statement, "non-finite");
        g_free (values);
        return TRUE;
    }

    switch (statement->verb)
    {
    case PN_FIGURE_VERB_VIEW:
    {
        View    view;
        gdouble was = pen->view.s;

        if (!view_set (&view, frame, values[0], values[1],
                       values[2], values[3]))
        {
            emit_skip (ops, statement, "degenerate");
            break;
        }

        pen->view = view;
        emit_view (ops, pen, line);

        /* The pen state is in user units, so a new scale changes what
         * it comes to in device units.  Re-emitting is what keeps the
         * painter free of the conversion — and it shows in the dump,
         * which is the honest place for it. */
        if (pen->view.s != was)
        {
            emit_width (ops, pen, line);
            emit_dash (ops, pen, line);
            emit_font (ops, pen, line);
        }
        break;
    }

    case PN_FIGURE_VERB_COLOR:
        pen->stroke = statement_colour (statement, values);
        op_add (ops, PN_FIGURE_OP_COLOR, line)->color = pen->stroke;
        break;

    case PN_FIGURE_VERB_FILL:
        pen->fill    = statement_colour (statement, values);
        pen->filling = TRUE;
        op_add (ops, PN_FIGURE_OP_FILL, line)->color = pen->fill;
        break;

    case PN_FIGURE_VERB_NOFILL:
        pen->filling = FALSE;
        op_add (ops, PN_FIGURE_OP_NOFILL, line);
        break;

    case PN_FIGURE_VERB_WIDTH:
        if (values[0] < 0.0)
        {
            emit_skip (ops, statement, "degenerate");
            break;
        }
        pen->width = values[0];
        emit_width (ops, pen, line);
        break;

    case PN_FIGURE_VERB_DASH:
    {
        const PnFigureArg *arg   = g_ptr_array_index (statement->args, 0);
        gdouble            scale = n > 1 ? values[1] : 1.0;

        if (scale <= 0.0)
        {
            emit_skip (ops, statement, "degenerate");
            break;
        }

        pen->dash       = (PnFigureDash) arg->word;
        pen->dash_scale = scale;
        emit_dash (ops, pen, line);
        break;
    }

    case PN_FIGURE_VERB_FONT:
        if (!(values[0] > 0.0))
        {
            emit_skip (ops, statement, "degenerate");
            break;
        }
        pen->font = values[0];
        emit_font (ops, pen, line);
        break;

    case PN_FIGURE_VERB_ALIGN:
    {
        const PnFigureArg *arg = g_ptr_array_index (statement->args, 0);
        PnFigureOp        *op;

        pen->halign = (PnFigureHAlign) arg->word;

        /* Omitting the vertical word leaves that axis alone, which is
         * what pen state means: `align "left"` moves one thing. */
        if (n > 1)
        {
            arg         = g_ptr_array_index (statement->args, 1);
            pen->valign = (PnFigureVAlign) arg->word;
        }

        op         = op_add (ops, PN_FIGURE_OP_ALIGN, line);
        op->halign = pen->halign;
        op->valign = pen->valign;
        break;
    }

    case PN_FIGURE_VERB_MOVE:
    case PN_FIGURE_VERB_RMOVE:
    {
        PnFigureOp *op;

        if (statement->verb == PN_FIGURE_VERB_MOVE)
        {
            pen->px = values[0];
            pen->py = values[1];
        }
        else
        {
            pen->px += values[0];
            pen->py += values[1];
        }

        op    = op_add (ops, PN_FIGURE_OP_MOVE, line);
        op->x = view_map_x (&pen->view, pen->px);
        op->y = view_map_y (&pen->view, pen->py);
        break;
    }

    case PN_FIGURE_VERB_LINETO:
    case PN_FIGURE_VERB_RLINE:
    case PN_FIGURE_VERB_LINE:
    {
        gdouble x1 = pen->px;
        gdouble y1 = pen->py;
        gdouble x2, y2;

        if (statement->verb == PN_FIGURE_VERB_LINE)
        {
            x1 = values[0];
            y1 = values[1];
            x2 = values[2];
            y2 = values[3];
        }
        else if (statement->verb == PN_FIGURE_VERB_LINETO)
        {
            x2 = values[0];
            y2 = values[1];
        }
        else
        {
            x2 = pen->px + values[0];
            y2 = pen->py + values[1];
        }

        emit_segment (ops, &pen->view, line, x1, y1, x2, y2);

        /* A segment leaves the pen at its far end, so a `line` then
         * `rline` chain works (80.6a). */
        pen->px = x2;
        pen->py = y2;
        break;
    }

    case PN_FIGURE_VERB_POINT:
    {
        PnFigureOp *op = op_add (ops, PN_FIGURE_OP_POINT, line);

        op->x = view_map_x (&pen->view, values[0]);
        op->y = view_map_y (&pen->view, values[1]);

        /* A disc of radius = the current line width, in the stroke
         * colour, so `width` sizes the dots and no new state appears
         * (80.6h).  A hairline still has to be visible as a dot. */
        op->r = pen->width == 0.0
                ? FIGURE_MIN_WIDTH
                : MAX (pen->width * pen->view.s, FIGURE_MIN_WIDTH);
        break;
    }

    case PN_FIGURE_VERB_CIRCLE:
    case PN_FIGURE_VERB_ARC:
    {
        PnFigureOp *op;
        gdouble     k, phi;

        if (!(values[2] > 0.0))
        {
            emit_skip (ops, statement, "degenerate");
            break;
        }

        op = op_add (ops, statement->verb == PN_FIGURE_VERB_ARC
                          ? PN_FIGURE_OP_ARC : PN_FIGURE_OP_CIRCLE, line);
        op->x = view_map_x (&pen->view, values[0]);
        op->y = view_map_y (&pen->view, values[1]);
        op->r = values[2] * pen->view.s;

        if (statement->verb != PN_FIGURE_VERB_ARC)
            break;

        view_angle_map (&pen->view, &k, &phi);
        op->a0 = k * values[3] + phi;
        op->a1 = k * values[4] + phi;

        /* Which way to travel between the two DEVICE angles, which is
         * simply which of them is larger — cairo_arc() runs up and
         * cairo_arc_negative() runs down, and each wraps by 2*pi until
         * its end lies the right side of its start.  Reading the
         * direction off the VIEW instead (k < 0) is wrong for a sweep
         * the user wrote BACKWARDS: 80.6(d)'s own example,
         * `arc 0,0,10,90,0`, is a clockwise quarter and would come out
         * as the three quarters going the other way round. */
        op->negative = op->a1 < op->a0;
        break;
    }

    case PN_FIGURE_VERB_RECT:
    {
        PnFigureOp *op = op_add (ops, PN_FIGURE_OP_RECT, line);
        gdouble     x1 = view_map_x (&pen->view, values[0]);
        gdouble     x2 = view_map_x (&pen->view, values[0] + values[2]);
        gdouble     y1 = view_map_y (&pen->view, values[1]);
        gdouble     y2 = view_map_y (&pen->view, values[1] + values[3]);

        /* `rect` takes the LOWER-LEFT corner because y points up
         * (80.6f); which device corner that is depends on the view, and
         * a negative width extends the other way, so normalise. */
        op->x = MIN (x1, x2);
        op->y = MIN (y1, y2);
        op->w = fabs (x2 - x1);
        op->h = fabs (y2 - y1);
        break;
    }

    case PN_FIGURE_VERB_POLY:
    case PN_FIGURE_VERB_PATH:
        op_add (ops, statement->verb == PN_FIGURE_VERB_POLY
                     ? PN_FIGURE_OP_POLY : PN_FIGURE_OP_PATH, line)->points
            = device_points (&pen->view, values, n);
        break;

    case PN_FIGURE_VERB_TEXT:
    {
        const PnFigureArg *arg = g_ptr_array_index (statement->args, 2);
        PnFigureOp        *op  = op_add (ops, PN_FIGURE_OP_TEXT, line);

        op->x      = view_map_x (&pen->view, values[0]);
        op->y      = view_map_y (&pen->view, values[1]);
        op->halign = pen->halign;
        op->valign = pen->valign;
        op->text   = format_text (arg->text, values + 3, n - 3);
        break;
    }

    case PN_FIGURE_VERB_REPEAT:
    case PN_FIGURE_VERB_END:
        /* Control flow is not ink (#86.7), and the walk in
         * pn_figure_resolve() has already dealt with the pair: what
         * reaches here is a block the front end never checked, which
         * is nothing to draw and nothing to complain about either. */
        break;

    default:
        /* PN_FIGURE_VERB_NONE cannot get here: the verb table removed
         * every statement it could not name. */
        g_warn_if_reached ();
        ok = FALSE;
        break;
    }

    g_free (values);
    return ok;
}

/* The index of the `end` that closes the `repeat` at @start, or the
 * statement count when the program has none.  Nesting is a parse error
 * (#86.2), so the first `end` is always the right one and the scan is
 * a single pass. */
static guint
block_end_index (
        GPtrArray *statements,
        guint      start)
{
    guint i;

    for (i = start + 1; i < statements->len; i++)
    {
        const PnFigureStatement *statement = g_ptr_array_index (statements, i);

        if (statement->kind == PN_FIGURE_STATEMENT_VERB
            && statement->verb == PN_FIGURE_VERB_END)
            return i;
    }

    return statements->len;
}

/* How many times the block at @statement runs.  A count is a VALUE and
 * not a program (#86.5): anything unusable skips the block whole, with
 * the marker that says why, and leaves the rest of the figure to draw.
 *
 * Returns %FALSE only for 80.10's class (c) — a vector count, or an
 * evaluation that could not be done — which empties the figure like any
 * other type error. */
static gboolean
repeat_count (
        PnFigureStatement *statement,
        PnVarStore        *store,
        guint              index,
        GPtrArray         *ops,
        GPtrArray         *errors,
        guint             *out_n)
{
    gdouble values[2] = { 0.0, 0.0 };
    gdouble count;

    *out_n = 0;

    if (!eval_args (statement, store, index, values, errors))
        return FALSE;

    count = trunc (values[0]);

    if (!isfinite (values[0]))
        emit_skip (ops, statement, "non-finite");
    else if (count < 1.0)
        emit_skip (ops, statement, "degenerate");
    else if (count > (gdouble) PN_FIGURE_MAX_REPEAT)
        emit_skip (ops, statement, "too-many");
    else
        *out_n = (guint) count;

    return TRUE;
}

GType
pn_figure_play_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_FIGURE_PLAY_ONCE,
              "PN_FIGURE_PLAY_ONCE",      "once"      },
            { PN_FIGURE_PLAY_LOOP,
              "PN_FIGURE_PLAY_LOOP",      "loop"      },
            { PN_FIGURE_PLAY_PING_PONG,
              "PN_FIGURE_PLAY_PING_PONG", "ping-pong" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnFigurePlayMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

gboolean
pn_figure_step_frame (
        PnFigurePlayMode  mode,
        guint             count,
        guint            *frame,
        gint             *direction)
{
    g_return_val_if_fail (frame != NULL, FALSE);
    g_return_val_if_fail (direction != NULL, FALSE);

    if (count <= 1)
    {
        *frame = 0;
        return FALSE;
    }

    if (*frame >= count)
        *frame = count - 1;

    switch (mode)
    {
    case PN_FIGURE_PLAY_ONCE:
        if (*frame + 1 < count)
            *frame += 1;
        return *frame + 1 < count;

    case PN_FIGURE_PLAY_PING_PONG:
        /* Bounce off both ends without showing an end frame twice:
         * 0 1 2 1 0 1 2, never 0 1 2 2 1. */
        if (*direction < 0)
        {
            if (*frame == 0)
            {
                *direction = 1;
                *frame     = 1;
            }
            else
                *frame -= 1;
        }
        else
        {
            if (*frame + 1 >= count)
            {
                *direction = -1;
                *frame     = count - 2;
            }
            else
                *frame += 1;
        }
        return TRUE;

    case PN_FIGURE_PLAY_LOOP:
    default:
        *frame = (*frame + 1) % count;
        return TRUE;
    }
}

GPtrArray *
pn_figure_resolve (
        GPtrArray              *statements,
        GPtrArray              *free_names,
        const PnFigureSnapshot *snapshot,
        const PnFigureFilm     *film,
        gdouble                 x,
        gdouble                 y,
        gdouble                 w,
        gdouble                 h,
        gboolean                stretch,
        gchar                 **out_error)
{
    GPtrArray  *ops = figure_ops_new ();
    GPtrArray  *errors;
    PnVarStore *store;
    Frame       frame;
    Pen         pen;
    gboolean    failed = FALSE;
    guint       index;
    guint       i;

    if (out_error != NULL)
        *out_error = NULL;

    g_return_val_if_fail (statements != NULL, ops);

    /* A client area with no room in it maps nothing, and dividing by
     * its extent would hand every coordinate an infinity. */
    if (!(w > 0.0) || !(h > 0.0))
        return ops;

    frame.x       = x;
    frame.y       = y;
    frame.w       = w;
    frame.h       = h;
    frame.stretch = stretch;

    /* A store per frame IS 80.2 rule 13's clear: last frame's
     * assignments cannot leak into this one if they were never here. */
    store = pn_var_store_new ();
    bind_frame (store, snapshot, free_names, film);
    index = film != NULL ? film->index : 0;

    pen_init (&pen, &frame);
    emit_view (ops, &pen, 0);
    emit_pen (ops, &pen, 0);

    errors = pn_figure_errors_new ();

    i = 0;
    while (i < statements->len && !failed)
    {
        PnFigureStatement *statement = g_ptr_array_index (statements, i);
        guint              end;
        guint              pass;
        guint              k;

        if (statement->kind != PN_FIGURE_STATEMENT_VERB
            || statement->verb != PN_FIGURE_VERB_REPEAT)
        {
            if (!resolve_statement (statement, store, index, &pen, &frame,
                                    ops, errors))
                failed = TRUE;
            i++;
            continue;
        }

        /* A block is shorthand for writing its statements out n times
         * (#86.6): the same pen, the same store, no scope of any kind
         * — only `i` changes, and it changes because the loop binds it
         * before each pass (#86.4). */
        end = block_end_index (statements, i);

        if (!repeat_count (statement, store, index, ops, errors, &pass))
        {
            failed = TRUE;
            break;
        }

        for (k = 0; k < pass && !failed; k++)
        {
            guint body;

            /* The index is an ordinary binding, which is why it beats
             * rule 12's zero-fill without anything being told about
             * it, and why it is rebound rather than saved (#86.4). */
            pn_var_store_set (store, PN_FIGURE_INDEX_NAME, (gdouble) k);

            for (body = i + 1; body < end; body++)
                if (!resolve_statement (g_ptr_array_index (statements, body),
                                        store, index, &pen, &frame,
                                        ops, errors))
                {
                    failed = TRUE;
                    break;
                }
        }

        i = end + 1;
    }

    /* Evaluate fully, then paint (80.10d): "nothing is drawn" is only
     * honest if the whole list is resolved before a single stroke goes
     * down, so a failure takes the list with it. */
    if (failed || errors->len > 0)
    {
        if (out_error != NULL)
            *out_error = pn_figure_errors_to_string (errors);

        g_ptr_array_set_size (ops, 0);
    }

    g_ptr_array_unref (errors);
    g_object_unref (store);
    return ops;
}

/* ================================================================== */
/*  The dump                                                          */
/* ================================================================== */

/* Every number in the dump goes through g_ascii_formatd(), or a machine
 * with a comma decimal separator fails every test in 80.12. */
static void
append_number (
        GString     *out,
        gdouble      value,
        const gchar *format)
{
    gchar        buf[G_ASCII_DTOSTR_BUF_SIZE];
    const gchar *text = g_ascii_formatd (buf, sizeof buf, format, value);

    if (text == NULL)
        return;

    /* Negative zero, and a coordinate that lands a hair under zero
     * after the arithmetic, both print as "-0.00" -- which is the same
     * place as "0.00", and only one of them can be an expected
     * string. */
    if (text[0] == '-' && strspn (text + 1, "0.") == strlen (text + 1))
        text++;

    g_string_append (out, text);
}

/* Device numbers at two decimals, so the 80.4 transform is under test
 * rather than trusted. */
static void
append_device (
        GString *out,
        gdouble  value)
{
    g_string_append_c (out, ' ');
    append_number (out, value, "%.2f");
}

/* A window is what the program TYPED, in user units, so it is printed
 * the way it was written rather than padded out to two decimals. */
static void
append_user (
        GString *out,
        gdouble  value)
{
    g_string_append_c (out, ' ');
    append_number (out, value, "%g");
}

static void
append_quoted (
        GString     *out,
        const gchar *text)
{
    const gchar *p;

    g_string_append_c (out, '"');
    for (p = text; *p != '\0'; p++)
    {
        switch (*p)
        {
        case '"':  g_string_append (out, "\\\""); break;
        case '\\': g_string_append (out, "\\\\"); break;
        case '\n': g_string_append (out, "\\n");  break;
        default:   g_string_append_c (out, *p);   break;
        }
    }
    g_string_append_c (out, '"');
}

static const gchar *
halign_word (
        PnFigureHAlign align)
{
    switch (align)
    {
    case PN_FIGURE_HALIGN_LEFT:  return "left";
    case PN_FIGURE_HALIGN_RIGHT: return "right";
    default:                     return "centre";
    }
}

static const gchar *
valign_word (
        PnFigureVAlign align)
{
    switch (align)
    {
    case PN_FIGURE_VALIGN_TOP:      return "top";
    case PN_FIGURE_VALIGN_BASELINE: return "baseline";
    case PN_FIGURE_VALIGN_BOTTOM:   return "bottom";
    default:                        return "middle";
    }
}

static void
append_points (
        GString      *out,
        const GArray *points)
{
    guint i;

    for (i = 0; points != NULL && i < points->len; i++)
        append_device (out, g_array_index (points, gdouble, i));
}

gchar *
pn_figure_display_to_string (
        GPtrArray *ops)
{
    GString *out = g_string_new (NULL);
    guint    i;

    for (i = 0; ops != NULL && i < ops->len; i++)
    {
        const PnFigureOp *op = g_ptr_array_index (ops, i);
        gint              n;

        switch (op->kind)
        {
        case PN_FIGURE_OP_VIEW:
            g_string_append (out, "# view");
            append_user (out, op->window[0]);
            append_user (out, op->window[1]);
            append_user (out, op->window[2]);
            append_user (out, op->window[3]);
            g_string_append (out, " scale");
            append_device (out, op->scale);
            g_string_append (out, " rect");
            append_device (out, op->x);
            append_device (out, op->y);
            append_device (out, op->w);
            append_device (out, op->h);

            /* The per-axis scales are both 1 unless `stretch` is on,
             * and a line that says so on every figure would be noise. */
            if (op->scale_x != 1.0 || op->scale_y != 1.0)
            {
                g_string_append (out, " stretch");
                append_device (out, op->scale_x);
                append_device (out, op->scale_y);
            }
            break;

        case PN_FIGURE_OP_COLOR:
        case PN_FIGURE_OP_FILL:
        {
            gchar *text = pn_color_to_string (&op->color);

            g_string_append (out, op->kind == PN_FIGURE_OP_FILL
                                  ? "fill " : "color ");
            g_string_append (out, text);
            g_free (text);
            break;
        }

        case PN_FIGURE_OP_NOFILL:
            g_string_append (out, "nofill");
            break;

        case PN_FIGURE_OP_WIDTH:
            g_string_append (out, "width");
            append_device (out, op->value);
            break;

        case PN_FIGURE_OP_DASH:
            g_string_append (out, "dash");
            if (op->n_dashes == 0)
                g_string_append (out, " solid");
            for (n = 0; n < op->n_dashes; n++)
                append_device (out, op->dashes[n]);
            break;

        case PN_FIGURE_OP_FONT:
            g_string_append (out, "font");
            append_device (out, op->value);
            break;

        case PN_FIGURE_OP_ALIGN:
            g_string_append_printf (out, "align %s %s",
                                    halign_word (op->halign),
                                    valign_word (op->valign));
            break;

        case PN_FIGURE_OP_MOVE:
            g_string_append (out, "move");
            append_device (out, op->x);
            append_device (out, op->y);
            break;

        case PN_FIGURE_OP_LINE:
            g_string_append (out, "line");
            append_points (out, op->points);
            break;

        case PN_FIGURE_OP_POINT:
            g_string_append (out, "point");
            append_device (out, op->x);
            append_device (out, op->y);
            append_device (out, op->r);
            break;

        case PN_FIGURE_OP_CIRCLE:
            g_string_append (out, "circle");
            append_device (out, op->x);
            append_device (out, op->y);
            append_device (out, op->r);
            break;

        case PN_FIGURE_OP_ARC:
            g_string_append (out, "arc");
            append_device (out, op->x);
            append_device (out, op->y);
            append_device (out, op->r);
            append_device (out, op->a0);
            append_device (out, op->a1);

            /* Which of cairo_arc() and cairo_arc_negative() the painter
             * calls, in those words: "clockwise" would have to say
             * clockwise IN WHICH SPACE, and that is 80.6(d)'s whole
             * trap. */
            g_string_append (out, op->negative ? " negative" : " positive");
            break;

        case PN_FIGURE_OP_RECT:
            g_string_append (out, "rect");
            append_device (out, op->x);
            append_device (out, op->y);
            append_device (out, op->w);
            append_device (out, op->h);
            break;

        case PN_FIGURE_OP_POLY:
        case PN_FIGURE_OP_PATH:
            g_string_append (out, op->kind == PN_FIGURE_OP_POLY
                                  ? "poly" : "path");
            append_points (out, op->points);
            break;

        case PN_FIGURE_OP_TEXT:
            g_string_append (out, "text");
            append_device (out, op->x);
            append_device (out, op->y);
            g_string_append_printf (out, " %s %s ",
                                    halign_word (op->halign),
                                    valign_word (op->valign));
            append_quoted (out, op->text != NULL ? op->text : "");
            break;

        case PN_FIGURE_OP_SKIP:
            g_string_append_printf (out, "# skip %d %s", op->line,
                                    op->text != NULL ? op->text : "");
            break;

        default:
            g_warn_if_reached ();
            break;
        }

        g_string_append_c (out, '\n');
    }

    return g_string_free (out, FALSE);
}

/* ================================================================== */
/*  The node                                                          */
/*                                                                    */
/*  The GObject half: properties, ports, receive() and the seam the   */
/*  painter reads.  Everything above this line is the language; this  */
/*  part only decides WHEN it runs.                                   */
/* ================================================================== */

/* Repaint no faster than this, however fast the source feeds us
 * (80.8h) — the same floor, and the same schedule_repaint() idiom,
 * PnPlot and PnOscilloscope already keep. */
#define PN_FIGURE_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

#define PN_FIGURE_MIN_INPUTS 1
#define PN_FIGURE_MAX_INPUTS 8
#define PN_FIGURE_DEF_INPUTS 1

/* What a node dragged in from the palette draws before it is wired to
 * anything (80.11g): three lines that show the shape of the language
 * and render immediately, because every free name zero-fills (80.2
 * rule 12).  `value1` is the default display name of input 1 — rename
 * the port and the program names it by the new name. */
#define PN_FIGURE_DEF_PROGRAM                  \
    "view 0, 0, 100, 100\n"                    \
    "circle 50, 50, 40\n"                      \
    "text 50, 50, \"%.1f\", value1"

struct _PnFigure
{
    PnNode parent_instance;

    /* Properties. */
    gchar    *program;
    gint      n_inputs;
    PnColor   background_color;
    gchar    *font_family;
    gboolean  stretch;

    /* The front end's verdict on @program, rebuilt on every set.  The
     * statements point into @lines, so the two live and die together. */
    GPtrArray *lines;          /* #PnFigureLine                        */
    GPtrArray *statements;     /* #PnFigureStatement                   */
    GPtrArray *names;          /* utf8: what the program reads         */
    gchar     *program_error;  /* the parse report, or %NULL           */

    /* The latched inputs, kept between frames (80.8e) because a figure
     * repaints long after the message that last changed it. */
    PnFigureSnapshot *snapshot;

    /* The frame of the film being shown (82.2), moved by the timer
     * (82.3); pn_figure_get_frame() keeps it inside the film. */
    guint frame;
    gint  direction;           /* +1 / -1, for ping-pong               */

    /* How the film plays (82.4). */
    PnFigurePlayMode play_mode;
    guint            fps;
    guint            frames;   /* explicit count, 0 = from the data    */

    guint anim_id;             /* the film timer, 0 when stopped       */

    /* The last pn_figure_render()'s verdict: a runtime TYPE problem,
     * or %NULL.  A runtime VALUE problem is not here — it left a skip
     * marker in the list and is deliberately not an error (80.10b). */
    gchar *runtime_error;

    /* What the `error` property currently reads, so a set that changes
     * nothing does not notify. */
    gchar *error;

    /* Repaint throttle — see schedule_repaint(). */
    gint64 last_repaint_us;
    guint  pending_repaint_id;
};

G_DEFINE_TYPE (PnFigure, pn_figure, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PROGRAM,
    PROP_INPUTS,
    PROP_BACKGROUND_COLOR,
    PROP_FONT_FAMILY,
    PROP_STRETCH,
    PROP_FPS,
    PROP_PLAY_MODE,
    PROP_FRAMES,
    PROP_ERROR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Repaint throttle (mirrors PnPlot / PnOscilloscope)                 */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnFigure *self = user_data;

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (
        PnFigure *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_FIGURE_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_FIGURE_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Error state                                                        */
/* ------------------------------------------------------------------ */

/* Recompute what the `error` property says, and with it whether the
 * node paints red.  A PROGRAM error outranks a runtime one: it is the
 * reason nothing was resolved in the first place.  has-error is set for
 * both classes that reach here and never for a skipped statement, so a
 * knob winding through zero does not teach the user to ignore the red
 * (80.10a, 80.10c versus 80.10b). */
static void
figure_refresh_error (
        PnFigure *self)
{
    const gchar *text = self->program_error != NULL ? self->program_error
                      : self->runtime_error != NULL ? self->runtime_error
                      : "";

    if (g_strcmp0 (self->error, text) == 0)
        return;

    g_free (self->error);
    self->error = g_strdup (text);

    pn_node_set_has_error (PN_NODE (self), *text != '\0');
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ERROR]);
}

/* ------------------------------------------------------------------ */
/*  The front end, once per `program` set                              */
/* ------------------------------------------------------------------ */

/* Re-run every stage of the front end over @self->program.  Done here,
 * on the property, rather than per frame: the dialog applies edits as
 * they are typed (80.11d), and a figure that animates will resolve the
 * same parse dozens of times a second (80.3a). */
static void
figure_recompile (
        PnFigure *self)
{
    GPtrArray *errors = pn_figure_errors_new ();

    g_clear_pointer (&self->names,      g_ptr_array_unref);
    g_clear_pointer (&self->statements, g_ptr_array_unref);
    g_clear_pointer (&self->lines,      g_ptr_array_unref);
    g_clear_pointer (&self->program_error, g_free);

    self->lines      = pn_figure_scan  (self->program, errors);
    self->statements = pn_figure_split (self->lines, errors);
    pn_figure_check_verbs       (self->statements, errors);
    pn_figure_check_blocks      (self->statements, errors);
    pn_figure_parse_literals    (self->statements, errors);
    pn_figure_parse_expressions (self->statements, errors);
    self->names      = pn_figure_free_names (self->statements);

    self->program_error = pn_figure_errors_to_string (errors);
    g_ptr_array_unref (errors);

    /* A new program makes the old frame's verdict meaningless. */
    g_clear_pointer (&self->runtime_error, g_free);
    figure_refresh_error (self);
}

/* ------------------------------------------------------------------ */
/*  The film timer (82.3)                                              */
/* ------------------------------------------------------------------ */

/* Whether anything is connected to repaint-needed -- the worksheet, the
 * layout editor, the panel engine.  Nothing means headless: no painter,
 * so nobody would ever see the frames the timer turns (80.17c).  A
 * blocked handler does not count; it would not see them either. */
static gboolean
figure_is_watched (
        PnFigure *self)
{
    static guint signal_id = 0;

    if (signal_id == 0)
        signal_id = g_signal_lookup ("repaint-needed", PN_TYPE_NODE);

    return g_signal_has_handler_pending (self, signal_id, 0, FALSE);
}

static void
figure_stop_film (
        PnFigure *self)
{
    if (self->anim_id != 0)
    {
        g_source_remove (self->anim_id);
        self->anim_id = 0;
    }
}

static gboolean
on_film_tick (
        gpointer user_data)
{
    PnFigure *self = user_data;
    gboolean  more;

    /* Re-checked every tick rather than only at the start, because the
     * listener can go -- a node removed from its worksheet keeps
     * living in the undo history, and must stop ticking there. */
    if (!figure_is_watched (self))
    {
        self->anim_id = 0;
        return G_SOURCE_REMOVE;
    }

    more = pn_figure_step_frame (self->play_mode,
                                 pn_figure_get_frame_count (self),
                                 &self->frame, &self->direction);

    /* Straight through, not schedule_repaint(): its 100 ms throttle
     * would cap every film at 10 fps. */
    pn_node_request_repaint (PN_NODE (self));

    if (!more)
    {
        self->anim_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Start the timer if there is a film, it has somewhere to go, and
 * somebody is watching.  Cheap enough to call from every place a film
 * can begin -- a message, a program edit, a paint. */
static void
figure_ensure_film (
        PnFigure *self)
{
    guint count;
    guint fps;

    if (self->anim_id != 0 || self->program_error != NULL)
        return;

    count = pn_figure_get_frame_count (self);
    if (count <= 1)
        return;

    /* A film played once and finished stays finished: a repaint must
     * not start a timer that would only find it at the end again. */
    if (self->play_mode == PN_FIGURE_PLAY_ONCE
        && pn_figure_get_frame (self) + 1 >= count)
        return;

    if (!figure_is_watched (self))
        return;

    fps           = CLAMP (self->fps, 1, PN_FIGURE_MAX_FPS);
    self->anim_id = g_timeout_add (1000 / fps, on_film_tick, self);
}

/* The timer again from where the film stands, for a change of pace or
 * of mode -- the frame is kept, only the ticking is redone. */
static void
figure_retime_film (
        PnFigure *self)
{
    figure_stop_film (self);
    figure_ensure_film (self);
}

/* A new film starts at the beginning (80.17b). */
static void
figure_restart_film (
        PnFigure *self)
{
    figure_stop_film (self);
    self->frame     = 0;
    self->direction = 1;
    figure_ensure_film (self);
}

/* After new data or a new program: a film of a DIFFERENT LENGTH is a
 * new film and starts at the beginning (80.17b); one of the same length
 * is the same film with new values, and carries on from the frame it
 * was on.  That is what lets a knob reshape a swinging pendulum without
 * snapping it back to frame 0 on every step (82.3d, amended). */
static void
figure_continue_film (
        PnFigure *self,
        guint     old_count)
{
    if (pn_figure_get_frame_count (self) != old_count)
    {
        figure_restart_film (self);
        return;
    }

    figure_ensure_film (self);
}

gboolean
pn_figure_is_playing (
        PnFigure *self)
{
    g_return_val_if_fail (PN_IS_FIGURE (self), FALSE);
    return self->anim_id != 0;
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

guint
pn_figure_get_frame_count (
        PnFigure *self)
{
    g_return_val_if_fail (PN_IS_FIGURE (self), 1);

    return pn_figure_frame_count (self->names, self->snapshot,
                                  self->frames);
}

guint
pn_figure_get_frame (
        PnFigure *self)
{
    guint count;

    g_return_val_if_fail (PN_IS_FIGURE (self), 0);

    count = pn_figure_get_frame_count (self);
    return count == 0 ? 0 : MIN (self->frame, count - 1);
}

/* pn_figure_render() of a given frame, which is what lets the dump ask
 * for any frame of the film without moving the one on screen. */
static GPtrArray *
figure_render_frame (
        PnFigure *self,
        guint     frame,
        gdouble   x,
        gdouble   y,
        gdouble   w,
        gdouble   h)
{
    GPtrArray    *ops;
    PnFigureFilm  film;
    gchar        *error = NULL;

    /* A program error draws nothing at all (80.10a) — not the good
     * statements either, because half a figure is a worse lie than
     * none. */
    if (self->program_error != NULL)
        return figure_ops_new ();

    /* An empty vector is not special-cased here: the resolver reaches
     * it at an argument and names the line and column, which is more
     * than a frame count of 0 could say (82.1e). */
    film.index  = frame;
    film.frames = self->frames;
    film.mode   = self->play_mode;

    ops = pn_figure_resolve (self->statements, self->names, self->snapshot,
                             &film, x, y, w, h, self->stretch, &error);

    g_free (self->runtime_error);
    self->runtime_error = error;   /* transferred; %NULL when all is well */
    figure_refresh_error (self);

    return ops;
}

GPtrArray *
pn_figure_render (
        PnFigure *self,
        gdouble   x,
        gdouble   y,
        gdouble   w,
        gdouble   h)
{
    GPtrArray *ops;

    g_return_val_if_fail (PN_IS_FIGURE (self), figure_ops_new ());

    ops = figure_render_frame (self, pn_figure_get_frame (self),
                               x, y, w, h);

    /* The film may have been waiting for a watcher: loaded, or
     * receiving, before any worksheet was listening. */
    figure_ensure_film (self);
    return ops;
}

gchar *
pn_figure_dump (
        PnFigure *self,
        guint     frame,
        gdouble   x,
        gdouble   y,
        gdouble   w,
        gdouble   h)
{
    GPtrArray *ops;
    gchar     *text;

    g_return_val_if_fail (PN_IS_FIGURE (self), g_strdup (""));

    ops  = figure_render_frame (self, frame, x, y, w, h);
    text = pn_figure_display_to_string (ops);

    g_ptr_array_unref (ops);
    return text;
}

const gchar *
pn_figure_get_error (
        PnFigure *self)
{
    g_return_val_if_fail (PN_IS_FIGURE (self), "");
    return self->error != NULL ? self->error : "";
}

void
pn_figure_get_background_color (
        PnFigure *self,
        PnColor  *out)
{
    g_return_if_fail (PN_IS_FIGURE (self));
    g_return_if_fail (out != NULL);

    *out = self->background_color;
}

const gchar *
pn_figure_get_font_family (
        PnFigure *self)
{
    g_return_val_if_fail (PN_IS_FIGURE (self), "");
    return self->font_family != NULL ? self->font_family : "";
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/* One binding from pn_expr_bind_collated() into the snapshot the next
 * frame will read. */
static void
figure_bind_into_snapshot (
        const gchar *name,
        gdouble      scalar,
        PnVector    *vec,
        gpointer     user_data)
{
    PnFigureSnapshot *snapshot = user_data;

    if (vec != NULL)
        pn_figure_snapshot_set_vector (snapshot, name, vec);
    else
        pn_figure_snapshot_set (snapshot, name, scalar);
}

static void
pn_figure_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnFigure  *self = PN_FIGURE (node);
    GPtrArray *ops;
    guint      old_count = pn_figure_get_frame_count (self);

    /* Re-latch: clear and refill, which is 80.8(e)'s answer to the fact
     * that a figure repaints long after the message.  The core has
     * already collated the other inputs' last values into the bag, so
     * one pass over it holds every input at once. */
    pn_figure_snapshot_clear (self->snapshot);
    pn_expr_bind_collated (node, message, figure_bind_into_snapshot,
                           self->snapshot);

    /* Before the render below, so the error state is judged on the
     * frame that will actually be shown. */
    figure_continue_film (self, old_count);

    /* Resolve once at the at-rest client rectangle, purely so the
     * `error` property is right straight away (80.10f) — a headless
     * worksheet and the D-Bus automation have no painter to do it for
     * them.  The list itself is thrown away; the painter resolves for
     * whatever rectangle it is actually given, which differs the moment
     * the node is lifted into the zoom overlay. */
    ops = pn_figure_render (self, 0.0, 0.0,
                            PN_FIGURE_WIDTH, PN_FIGURE_CLIENT_HEIGHT);
    g_ptr_array_unref (ops);

    /* A sink (80.8g): nothing is emitted onward. */
    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_figure_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    if (out_width  != NULL) *out_width  = PN_FIGURE_WIDTH;
    /* A figure with 2+ inputs stacks one named row per input under the
     * header; the drawing goes below those rows, not over them, so the
     * footprint grows by the port section and the client area keeps its
     * fixed height. */
    if (out_height != NULL)
        *out_height = PN_FIGURE_TOTAL_HEIGHT
                    + pn_node_get_port_section_height (node);
}

static double
pn_figure_get_header_height (
        PnNode *node)
{
    (void) node;
    return PN_FIGURE_HEADER_HEIGHT;
}

/* No get_client_area override: the figure's body IS the rectangle under
 * the header and its input rows, which is exactly what PnNode's geometric default reports
 * and exactly what the worksheet hands paint_plot. */

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_figure_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFigure *self = PN_FIGURE (object);

    switch (prop_id)
    {
    case PROP_PROGRAM:
        g_value_set_string (value, self->program);
        break;
    case PROP_INPUTS:
        g_value_set_int (value, self->n_inputs);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_FONT_FAMILY:
        g_value_set_string (value, self->font_family);
        break;
    case PROP_STRETCH:
        g_value_set_boolean (value, self->stretch);
        break;
    case PROP_FPS:
        g_value_set_int (value, (gint) self->fps);
        break;
    case PROP_PLAY_MODE:
        g_value_set_enum (value, self->play_mode);
        break;
    case PROP_FRAMES:
        g_value_set_int (value, (gint) self->frames);
        break;
    case PROP_ERROR:
        g_value_set_string (value, pn_figure_get_error (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_figure_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFigure *self = PN_FIGURE (object);

    switch (prop_id)
    {
    case PROP_PROGRAM:
        {
            const gchar *s = g_value_get_string (value);

            if (g_strcmp0 (self->program, s) != 0)
            {
                guint old_count = pn_figure_get_frame_count (self);

                g_free (self->program);
                self->program = g_strdup (s != NULL ? s : "");
                figure_recompile (self);
                /* An edit keeps a swinging figure swinging, unless it
                 * changed how long the film is. */
                figure_continue_film (self, old_count);
                g_object_notify_by_pspec (object, props[PROP_PROGRAM]);
                /* Straight through, not throttled: this is somebody
                 * typing, and the whole point of the code editor is
                 * that the card follows the keystrokes (80.11d). */
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_INPUTS:
        {
            gint n = g_value_get_int (value);

            if (n != self->n_inputs)
            {
                self->n_inputs = n;
                /* Resize the live ports; the core grows or shrinks its
                 * per-input latches to match. */
                pn_node_set_n_inputs (PN_NODE (self), n);
                g_object_notify_by_pspec (object, props[PROP_INPUTS]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BACKGROUND_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);

            if (c != NULL && !pn_color_equal (c, &self->background_color))
            {
                self->background_color = *c;
                g_object_notify_by_pspec (object,
                                          props[PROP_BACKGROUND_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_FONT_FAMILY:
        {
            const gchar *s = g_value_get_string (value);

            if (g_strcmp0 (self->font_family, s) != 0)
            {
                g_free (self->font_family);
                self->font_family = g_strdup (s != NULL ? s : "");
                g_object_notify_by_pspec (object, props[PROP_FONT_FAMILY]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_STRETCH:
        {
            gboolean v = g_value_get_boolean (value);

            if (self->stretch != v)
            {
                self->stretch = v;
                g_object_notify_by_pspec (object, props[PROP_STRETCH]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_FPS:
        {
            guint v = (guint) g_value_get_int (value);

            if (self->fps != v)
            {
                self->fps = v;
                g_object_notify_by_pspec (object, props[PROP_FPS]);
                /* Same film, new pace: retime without rewinding. */
                figure_retime_film (self);
            }
        }
        break;
    case PROP_PLAY_MODE:
        {
            PnFigurePlayMode v = g_value_get_enum (value);

            if (self->play_mode != v)
            {
                self->play_mode = v;
                self->direction = 1;
                g_object_notify_by_pspec (object, props[PROP_PLAY_MODE]);
                /* Keep the frame; a ONCE film that had finished starts
                 * moving again if the new mode has somewhere to go.
                 * Repaint as well: the mode decides where `t` ends
                 * (82.5), so the frame on screen may have moved. */
                figure_retime_film (self);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_FRAMES:
        {
            guint v = (guint) g_value_get_int (value);

            if (self->frames != v)
            {
                self->frames = v;
                g_object_notify_by_pspec (object, props[PROP_FRAMES]);
                /* A different length is a different film. */
                figure_restart_film (self);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_figure_dispose (
        GObject *object)
{
    /* The timer holds a plain pointer, so it must go before the object
     * does (80.17b). */
    figure_stop_film (PN_FIGURE (object));

    G_OBJECT_CLASS (pn_figure_parent_class)->dispose (object);
}

static void
pn_figure_finalize (
        GObject *object)
{
    PnFigure *self = PN_FIGURE (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    g_clear_pointer (&self->names,         g_ptr_array_unref);
    g_clear_pointer (&self->statements,    g_ptr_array_unref);
    g_clear_pointer (&self->lines,         g_ptr_array_unref);
    g_clear_pointer (&self->program_error, g_free);
    g_clear_pointer (&self->runtime_error, g_free);
    g_clear_pointer (&self->error,         g_free);
    g_clear_pointer (&self->program,       g_free);
    g_clear_pointer (&self->font_family,   g_free);
    g_clear_pointer (&self->snapshot,      pn_figure_snapshot_free);

    G_OBJECT_CLASS (pn_figure_parent_class)->finalize (object);
}

static void
pn_figure_class_init (
        PnFigureClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_figure_get_property;
    object_class->set_property = pn_figure_set_property;
    object_class->dispose      = pn_figure_dispose;
    object_class->finalize     = pn_figure_finalize;

    node_class->receive           = pn_figure_receive;
    node_class->get_size          = pn_figure_get_size;
    node_class->get_header_height = pn_figure_get_header_height;
    /* The cairo painter (paint_plot) is installed onto this class by the
     * gui tier — pn_figure_gui_install() in pn-figure-gui.c — so the
     * headless core carries no cairo and no Pango. */

    node_class->class_name    = "Figure";
    node_class->icon          = "\xef\x81\x80";  /* fa-pencil U+F040 */
    node_class->color         = (PnColor){ 0.55, 0.36, 0.66, 1.0 };
    node_class->category      = "Sinks";
    node_class->has_input     = TRUE;
    node_class->has_output    = FALSE;

    /* The figure preserves its own aspect and letterboxes what is left
     * over, so a stretched overlay would only grow the bars (80.4g). */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    props[PROP_PROGRAM] = g_param_spec_string (
            "program", "Program",
            "The drawing program, one verb per line.  Y POINTS UP: "
            "`view xmin, ymin, xmax, ymax` declares the user-unit window "
            "(0, 0, 100, 100 by default), which is fitted into the card "
            "preserving aspect and centred.  Geometry: move, rmove, "
            "lineto, rline, line, point, circle, arc, rect, poly, path, "
            "text.  Pen state, which persists until changed: color, fill, "
            "nofill, width, dash, font, align.  `repeat n` ... `end` draws "
            "the lines between them n times with `i` counting 0, 1, 2 …, "
            "which is how a grid or a row of ticks is written; blocks do "
            "not nest.  Every coordinate and every "
            "length is in user units and scales with the drawing.  "
            "Arguments are expressions in the calculator language, so "
            "`circle 0, 0, 10 * sin(t)` works; a quoted argument is a "
            "literal.  `name = expr` on a line of its own binds a variable "
            "for the lines below it.  Each input's last data.value is "
            "bound under that input's name (value1 … valueN by default, or "
            "whatever the inputs are renamed to); a name the program reads "
            "and nothing supplies is 0, so a figure draws even unwired.  "
            "`#` starts a comment; a line ending in a comma continues onto "
            "the next.  An input that is a vector makes the figure a film: "
            "frame k draws element k of every vector, and `frame` (0, 1, "
            "2 …) and `t` (0 towards 1 over the film) animate the same "
            "way, so a figure with nothing wired plays when the Animation "
            "tab gives it a frame count.",
            PN_FIGURE_DEF_PROGRAM,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_PROGRAM]);

    props[PROP_INPUTS] = g_param_spec_int (
            "inputs", "Inputs",
            "How many inputs the node has.  Each input's last data.value "
            "is remembered and bound in the program under that input's "
            "name (value1 … valueN by default, or whatever the inputs are "
            "renamed to), so every input is available on every repaint, "
            "not only the one that just fired.",
            PN_FIGURE_MIN_INPUTS, PN_FIGURE_MAX_INPUTS,
            PN_FIGURE_DEF_INPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the whole card area behind the drawing, "
            "including the letterbox bars the fitted window leaves over",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FONT_FAMILY] = g_param_spec_string (
            "font-family", "Font family",
            "Family every `text` in this figure is drawn in, so one "
            "drawing stays typographically consistent without repeating "
            "the family on every label.  Empty means the theme default.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_STRETCH] = g_param_spec_boolean (
            "stretch", "Stretch to fit",
            "Distort the drawing to fill the whole card instead of "
            "preserving the window's aspect ratio and centring it.  Off by "
            "default: a figure with a stretched circle in it is rarely "
            "what anyone meant.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FPS] = g_param_spec_int (
            "fps", "Frames per second",
            "How fast a film plays.  A figure becomes a film when an "
            "input it reads is a vector: frame i draws element i of it.",
            1, PN_FIGURE_MAX_FPS, PN_FIGURE_DEFAULT_FPS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* LOOP by default (82.4): a figure is a readout on a card, and a
     * film that plays once and freezes is over before anybody looks. */
    props[PROP_PLAY_MODE] = g_param_spec_enum (
            "play-mode", "Play mode",
            "What a film does at its last frame: once stops there and "
            "holds it, loop starts again from the first, ping-pong plays "
            "back to the first and turns again.",
            PN_TYPE_FIGURE_PLAY_MODE,
            PN_FIGURE_PLAY_LOOP,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FRAMES] = g_param_spec_int (
            "frames", "Frames",
            "How many frames the film has.  0 takes it from the data: "
            "the shortest vector input the program reads.  A number here "
            "makes a film with no vector input at all, and with one it "
            "can shorten the film but never stretch it past the data.",
            0, PN_FIGURE_MAX_FRAMES, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Read-only on purpose (80.10f): the client area is this node's only
     * error channel, and a readable-but-not-writable property is one
     * pn-flow.c leaves out of the saved worksheet. */
    props[PROP_ERROR] = g_param_spec_string (
            "error", "Error",
            "What is wrong with the program, or empty when nothing is.  "
            "The same text the card shows in place of the figure.",
            "",
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema (80.11): the program on a tab of its
     * own, full width, in the GtkSourceView code editor — which is what
     * shows the line numbers an error message cites.  "sh" is close
     * enough to colour `#` comments, numbers and quoted strings; a
     * figure.lang that would colour the verbs too is noted, not built
     * (80.11c).  The error text sits under the editor, where the person
     * who typed the mistake is looking (80.11e). */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Figure");

        pn_settings_schema_row       (schema, "program", PN_EDITOR_CODE);
        pn_settings_schema_row_flags (schema, "program",
                                      PN_ROW_FLAG_FULL_WIDTH);
        pn_settings_schema_code_language (schema, "program", "sh");

        pn_settings_schema_row       (schema, "error", PN_EDITOR_LABEL);
        pn_settings_schema_row_flags (schema, "error",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_settings_schema_tab (schema, "Appearance");
        pn_settings_schema_row (schema, "background-color", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "font-family",      PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "stretch",          PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Animation");
        pn_settings_schema_row (schema, "play-mode", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "fps",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "frames",    PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_figure_init (
        PnFigure *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  plum = { 0.55, 0.36, 0.66, 1.0 };

    self->n_inputs         = PN_FIGURE_DEF_INPUTS;
    self->background_color = (PnColor){ 1.0, 1.0, 1.0, 1.0 };
    self->font_family      = g_strdup ("");
    self->stretch          = FALSE;
    self->snapshot         = pn_figure_snapshot_new ();
    self->error            = g_strdup ("");
    self->direction        = 1;
    self->play_mode        = PN_FIGURE_PLAY_LOOP;
    self->fps              = PN_FIGURE_DEFAULT_FPS;

    /* Mirror the property default and compile it, so a freshly dropped
     * node draws instead of sitting blank (80.11g, 80.8i). */
    self->program = g_strdup (PN_FIGURE_DEF_PROGRAM);
    figure_recompile (self);

    pn_node_set_class_name (node, "Figure");
    pn_node_set_icon       (node, "\xef\x81\x80");  /* fa-pencil U+F040 */
    pn_node_set_color      (node, &plum);
    pn_node_set_n_inputs   (node, self->n_inputs);  /* 1..8, default 1 */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
    /* Let the node dialog offer a spin for the input count on its own
     * "Inputs" tab, beside the editable per-input names (80.24.2). */
    pn_node_set_input_count_property (node, "inputs");
    /* Latch each input's /data/value and surface it under the input's
     * name on every message, so the program sees every input at once
     * even though only one of them just fired (80.8a). */
    pn_node_set_collate_inputs (node, TRUE);
}

PnFigure *
pn_figure_new (void)
{
    return g_object_new (PN_TYPE_FIGURE, NULL);
}
