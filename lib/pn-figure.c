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

/* ================================================================== */
/*  The statement splitter                                            */
/* ================================================================== */

static void
pn_figure_arg_free (
        PnFigureArg *self)
{
    if (self == NULL)
        return;

    g_free (self->text);
    g_free (self);
}

void
pn_figure_statement_free (
        PnFigureStatement *self)
{
    if (self == NULL)
        return;

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
