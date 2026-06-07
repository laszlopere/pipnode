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

#include "pn-expr-parser.h"

#include <stdarg.h>

struct _PnExprParser
{
    GObject parent_instance;
};

G_DEFINE_TYPE (PnExprParser, pn_expr_parser, G_TYPE_OBJECT)

G_DEFINE_QUARK (pn-expr-parser-error, pn_expr_parser_error)

/* ------------------------------------------------------------------ */
/*  AST construction / teardown                                        */
/* ------------------------------------------------------------------ */

static PnExprNode *
node_new (PnExprNodeType type)
{
    PnExprNode *n = g_new0 (PnExprNode, 1);
    n->type = type;
    return n;
}

void
pn_expr_node_free (PnExprNode *node)
{
    if (node == NULL)
        return;

    pn_expr_node_free (node->left);
    pn_expr_node_free (node->right);
    g_free (node->name);
    g_free (node);
}

/* ------------------------------------------------------------------ */
/*  Lexer                                                              */
/*                                                                     */
/*  A single-token-lookahead scanner.  parse_*() always works against  */
/*  the "current" token in the context; lex_advance() moves to the     */
/*  next one.  Identifiers are reported as a (start, len) slice into    */
/*  the source so the parser only allocates a copy for the ones that    */
/*  actually become AST nodes.                                         */
/* ------------------------------------------------------------------ */

typedef enum
{
    TOK_END,
    TOK_NUMBER,
    TOK_IDENT,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_LT,        /* <  */
    TOK_GT,        /* >  */
    TOK_LE,        /* <= */
    TOK_GE,        /* >= */
    TOK_EQ,        /* == */
    TOK_NE,        /* != */
    TOK_ASSIGN,    /* =  (statement-level assignment) */
    TOK_NEWLINE,   /* one or more newlines: statement separator */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_ERROR,
} TokenType;

/* Cap on parser recursion.  Every level of parenthesis / function-argument
 * nesting, and every unary sign, descends through parse_factor (see the
 * grammar below); without a limit a pathological input like ((((…)))) or a
 * long unary-minus chain overflows the native C stack and crashes the
 * process.  256 is far beyond any hand-written expression yet leaves the
 * real stack comfortably untouched.  The evaluator (pn-var-store.c) recurses
 * to the same depth, but the tree it walks can be no deeper than this, so
 * bounding the parser bounds it too. */
#define PN_EXPR_MAX_DEPTH 256

typedef struct
{
    const gchar *input;       /* whole source, for column reporting */
    const gchar *p;           /* scan cursor */

    TokenType    tok;         /* current token */
    gdouble      number;      /* TOK_NUMBER value */
    const gchar *ident_start; /* TOK_IDENT slice */
    gsize        ident_len;
    const gchar *tok_start;   /* where the current token began */

    GError     **error;       /* borrowed; may be NULL */
    gboolean     failed;      /* an error has been recorded */
    gint         depth;       /* current parse_factor recursion depth */
} Ctx;

G_GNUC_PRINTF (3, 4)
static void
ctx_set_error (Ctx               *c,
               PnExprParserError  code,
               const gchar       *fmt,
               ...)
{
    va_list  ap;
    gchar   *msg;

    /* Keep the first error: a failure deep in the recursion is more
     * specific than the "expected …" the unwinding parents would add. */
    if (c->failed)
        return;
    c->failed = TRUE;

    va_start (ap, fmt);
    msg = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    g_set_error_literal (c->error, PN_EXPR_PARSER_ERROR, code, msg);
    g_free (msg);
}

/** 1-based column of the current token, for error messages. */
static gint
ctx_column (Ctx *c)
{
    return (gint) (c->tok_start - c->input) + 1;
}

static void
lex_advance (Ctx *c)
{
    const gchar *p = c->p;

    /* Spaces and tabs are insignificant; newlines are not — they
     * separate statements, so they get their own token. */
    while (*p == ' ' || *p == '\t')
        p++;

    c->tok_start = p;

    /* A run of newlines (with any spaces/tabs between them) collapses to
     * a single separator token, so blank lines never produce empty
     * statements. */
    if (*p == '\n' || *p == '\r')
    {
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
            p++;
        c->tok = TOK_NEWLINE;
        c->p   = p;
        return;
    }

    if (*p == '\0')
    {
        c->tok = TOK_END;
        c->p   = p;
        return;
    }

    /* Number: a digit, or a dot immediately followed by a digit. */
    if (g_ascii_isdigit (*p) || (*p == '.' && g_ascii_isdigit (p[1])))
    {
        gchar *end = NULL;
        c->number = g_ascii_strtod (p, &end);
        if (end == p)
        {
            ctx_set_error (c, PN_EXPR_PARSER_ERROR_SYNTAX,
                           "malformed number at position %d",
                           (gint) (p - c->input) + 1);
            c->tok = TOK_ERROR;
            return;
        }
        c->p   = end;
        c->tok = TOK_NUMBER;
        return;
    }

    /* Identifier: [A-Za-z_][A-Za-z0-9_]* */
    if (g_ascii_isalpha (*p) || *p == '_')
    {
        const gchar *s = p;
        while (g_ascii_isalnum (*p) || *p == '_')
            p++;
        c->ident_start = s;
        c->ident_len   = (gsize) (p - s);
        c->p           = p;
        c->tok         = TOK_IDENT;
        return;
    }

    switch (*p)
    {
    case '+': c->tok = TOK_PLUS;   c->p = p + 1; return;
    case '-': c->tok = TOK_MINUS;  c->p = p + 1; return;
    case '*': c->tok = TOK_STAR;   c->p = p + 1; return;
    case '/': c->tok = TOK_SLASH;  c->p = p + 1; return;
    case '(': c->tok = TOK_LPAREN; c->p = p + 1; return;
    case ')': c->tok = TOK_RPAREN; c->p = p + 1; return;

    /* Comparisons: '<' and '>' stand alone or take a trailing '=';
     * '!=' must be two characters — a lone '!' is an error, since the
     * language has no logical-not.  ('=' is handled separately: '==' is
     * equality, a lone '=' is assignment.) */
    case '<':
        if (p[1] == '=') { c->tok = TOK_LE; c->p = p + 2; }
        else             { c->tok = TOK_LT; c->p = p + 1; }
        return;
    case '>':
        if (p[1] == '=') { c->tok = TOK_GE; c->p = p + 2; }
        else             { c->tok = TOK_GT; c->p = p + 1; }
        return;
    case '=':
        /* "==" is equality; a lone "=" is statement-level assignment. */
        if (p[1] == '=') { c->tok = TOK_EQ;     c->p = p + 2; }
        else             { c->tok = TOK_ASSIGN; c->p = p + 1; }
        return;
    case '!':
        if (p[1] == '=') { c->tok = TOK_NE; c->p = p + 2; return; }
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_SYNTAX,
                       "expected '!=' but found a single '!' at position %d",
                       (gint) (p - c->input) + 1);
        c->tok = TOK_ERROR;
        return;

    default:
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_SYNTAX,
                       "unexpected character '%c' at position %d",
                       *p, (gint) (p - c->input) + 1);
        c->tok = TOK_ERROR;
        return;
    }
}

/** Return the token *after* the current one without disturbing @c.  The
 *  scanner state is all pointers, so a struct copy is a cheap snapshot;
 *  the copy gets no error sink since a peek never reports failures.
 *  Used to tell an assignment (`IDENT =`) from an expression that merely
 *  begins with an identifier. */
static TokenType
lex_peek (Ctx *c)
{
    Ctx tmp = *c;
    tmp.error  = NULL;
    tmp.failed = FALSE;
    lex_advance (&tmp);
    return tmp.tok;
}

/* ------------------------------------------------------------------ */
/*  Recursive-descent parser                                          */
/*                                                                     */
/*    program    := NEWLINE* statement (NEWLINE statement)* NEWLINE*    */
/*    statement  := IDENT '=' expression          // assignment        */
/*                | expression                                         */
/*    expression := additive (CMP additive)*    // CMP: < > <= >= == != */
/*    additive   := term   (('+' | '-') term)*                         */
/*    term       := factor (('*' | '/') factor)*                       */
/*    factor     := NUMBER                                             */
/*                | IDENT '(' expression ')'   // function call        */
/*                | IDENT                       // variable            */
/*                | '(' expression ')'                                 */
/*                | ('+' | '-') factor          // unary sign          */
/*                                                                     */
/*  Comparisons sit at the lowest precedence level and are left-       */
/*  associative like the arithmetic operators; each yields 1.0/0.0.    */
/*  A program is one or more newline-separated statements; its value   */
/*  is that of the last statement (see pn-var-store evaluation).       */
/* ------------------------------------------------------------------ */

static PnExprNode *parse_expression (Ctx *c);
static PnExprNode *parse_factor (Ctx *c);

static PnExprNode *
parse_factor_body (Ctx *c)
{
    switch (c->tok)
    {
    case TOK_NUMBER:
        {
            PnExprNode *n = node_new (PN_EXPR_NODE_NUMBER);
            n->number = c->number;
            lex_advance (c);
            return n;
        }

    case TOK_MINUS:
        {
            PnExprNode *operand, *n;
            lex_advance (c);
            operand = parse_factor (c);   /* unary binds tighter than * / */
            if (operand == NULL)
                return NULL;
            n = node_new (PN_EXPR_NODE_UNARY);
            n->op   = '-';
            n->left = operand;
            return n;
        }

    case TOK_PLUS:
        /* Unary plus is a no-op; accept and parse the operand. */
        lex_advance (c);
        return parse_factor (c);

    case TOK_LPAREN:
        {
            PnExprNode *inner;
            lex_advance (c);
            inner = parse_expression (c);
            if (inner == NULL)
                return NULL;
            if (c->tok != TOK_RPAREN)
            {
                ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                               "expected ')' at position %d", ctx_column (c));
                pn_expr_node_free (inner);
                return NULL;
            }
            lex_advance (c);
            return inner;
        }

    case TOK_IDENT:
        {
            gchar *name = g_strndup (c->ident_start, c->ident_len);
            lex_advance (c);

            if (c->tok == TOK_LPAREN)
            {
                /* Function call: name '(' expression ')'. */
                PnExprNode *arg, *n;
                lex_advance (c);
                arg = parse_expression (c);
                if (arg == NULL)
                {
                    g_free (name);
                    return NULL;
                }
                if (c->tok != TOK_RPAREN)
                {
                    ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                                   "expected ')' after argument to '%s' "
                                   "at position %d", name, ctx_column (c));
                    pn_expr_node_free (arg);
                    g_free (name);
                    return NULL;
                }
                lex_advance (c);
                n = node_new (PN_EXPR_NODE_CALL);
                n->name = name;     /* transfer ownership */
                n->left = arg;
                return n;
            }
            else
            {
                PnExprNode *n = node_new (PN_EXPR_NODE_VARIABLE);
                n->name = name;     /* transfer ownership */
                return n;
            }
        }

    case TOK_END:
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF,
                       "unexpected end of expression");
        return NULL;

    case TOK_ERROR:
        return NULL;            /* lexer already recorded the error */

    default:
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                       "unexpected token at position %d", ctx_column (c));
        return NULL;
    }
}

/** Recursion-guarded wrapper around parse_factor_body.  All recursion in
 *  the grammar — parentheses, function arguments, unary signs — passes
 *  through here, so a single depth check on entry caps the whole tree (and
 *  hence the native stack) without sprinkling counters across every rule. */
static PnExprNode *
parse_factor (Ctx *c)
{
    PnExprNode *n;

    if (++c->depth > PN_EXPR_MAX_DEPTH)
    {
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_SYNTAX,
                       "expression nesting too deep (limit %d levels)",
                       PN_EXPR_MAX_DEPTH);
        c->depth--;
        return NULL;
    }

    n = parse_factor_body (c);
    c->depth--;
    return n;
}

static PnExprNode *
parse_term (Ctx *c)
{
    PnExprNode *left = parse_factor (c);
    if (left == NULL)
        return NULL;

    while (c->tok == TOK_STAR || c->tok == TOK_SLASH)
    {
        gchar       op = (c->tok == TOK_STAR) ? '*' : '/';
        PnExprNode *right, *n;

        lex_advance (c);
        right = parse_factor (c);
        if (right == NULL)
        {
            pn_expr_node_free (left);
            return NULL;
        }

        n = node_new (PN_EXPR_NODE_BINARY);
        n->op    = op;
        n->left  = left;
        n->right = right;
        left = n;
    }

    return left;
}

static PnExprNode *
parse_additive (Ctx *c)
{
    PnExprNode *left = parse_term (c);
    if (left == NULL)
        return NULL;

    while (c->tok == TOK_PLUS || c->tok == TOK_MINUS)
    {
        gchar       op = (c->tok == TOK_PLUS) ? '+' : '-';
        PnExprNode *right, *n;

        lex_advance (c);
        right = parse_term (c);
        if (right == NULL)
        {
            pn_expr_node_free (left);
            return NULL;
        }

        n = node_new (PN_EXPR_NODE_BINARY);
        n->op    = op;
        n->left  = left;
        n->right = right;
        left = n;
    }

    return left;
}

/** Map a comparison token to the single-character operator code carried
 *  in PnExprNode.op (see the header), or '\0' if @tok is not one. */
static gchar
comparison_op (TokenType tok)
{
    switch (tok)
    {
    case TOK_LT: return '<';
    case TOK_GT: return '>';
    case TOK_LE: return 'L';
    case TOK_GE: return 'G';
    case TOK_EQ: return '=';
    case TOK_NE: return '!';
    default:     return '\0';
    }
}

static PnExprNode *
parse_expression (Ctx *c)
{
    PnExprNode *left = parse_additive (c);
    if (left == NULL)
        return NULL;

    for (gchar op; (op = comparison_op (c->tok)) != '\0'; )
    {
        PnExprNode *right, *n;

        lex_advance (c);
        right = parse_additive (c);
        if (right == NULL)
        {
            pn_expr_node_free (left);
            return NULL;
        }

        n = node_new (PN_EXPR_NODE_BINARY);
        n->op    = op;
        n->left  = left;
        n->right = right;
        left = n;
    }

    return left;
}

/** A statement is either `IDENT '=' expression` (an assignment) or a
 *  bare expression.  The two are told apart with a one-token peek: an
 *  identifier immediately followed by '=' starts an assignment; anything
 *  else (incl. `IDENT(`, `IDENT ==`, `IDENT +`) is an expression. */
static PnExprNode *
parse_statement (Ctx *c)
{
    if (c->tok == TOK_IDENT && lex_peek (c) == TOK_ASSIGN)
    {
        gchar      *name = g_strndup (c->ident_start, c->ident_len);
        PnExprNode *value, *n;

        lex_advance (c);    /* consume the identifier */
        lex_advance (c);    /* consume '='            */

        value = parse_expression (c);
        if (value == NULL)
        {
            g_free (name);
            return NULL;
        }

        n = node_new (PN_EXPR_NODE_ASSIGN);
        n->name = name;     /* transfer ownership */
        n->left = value;
        return n;
    }

    return parse_expression (c);
}

/** Parse the whole program: one or more statements separated by newline
 *  tokens, with blank lines (and leading/trailing newlines) ignored.
 *  Folds them into a right-leaning PN_EXPR_NODE_SEQ chain whose value is
 *  the last statement's; a single statement needs no SEQ wrapper. */
static PnExprNode *
parse_program (Ctx *c)
{
    GPtrArray  *stmts;
    PnExprNode *result;
    guint       i;

    /* Skip blank lines before the first statement. */
    while (c->tok == TOK_NEWLINE)
        lex_advance (c);

    if (c->tok == TOK_END)
    {
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF,
                       "unexpected end of expression");
        return NULL;
    }

    stmts = g_ptr_array_new_with_free_func ((GDestroyNotify) pn_expr_node_free);

    for (;;)
    {
        PnExprNode *stmt = parse_statement (c);
        if (stmt == NULL)
        {
            g_ptr_array_unref (stmts);
            return NULL;
        }
        g_ptr_array_add (stmts, stmt);

        /* A separator means another statement may follow; the end of
         * input (directly, or after a trailing newline) finishes. */
        if (c->tok == TOK_NEWLINE)
        {
            lex_advance (c);
            if (c->tok == TOK_END)
                break;
            continue;
        }
        if (c->tok == TOK_END)
            break;

        /* A complete statement followed by anything but a separator. */
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                       "unexpected token at position %d", ctx_column (c));
        g_ptr_array_unref (stmts);
        return NULL;
    }

    result = g_ptr_array_index (stmts, stmts->len - 1);
    for (i = stmts->len - 1; i > 0; i--)
    {
        PnExprNode *seq = node_new (PN_EXPR_NODE_SEQ);
        seq->left  = g_ptr_array_index (stmts, i - 1);
        seq->right = result;
        result = seq;
    }

    g_ptr_array_free (stmts, FALSE);   /* free the array, keep the nodes */
    return result;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_expr_parser_class_init (PnExprParserClass *klass)
{
    (void) klass;
}

static void
pn_expr_parser_init (PnExprParser *self)
{
    (void) self;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnExprParser *
pn_expr_parser_new (void)
{
    return g_object_new (PN_TYPE_EXPR_PARSER, NULL);
}

PnExprNode *
pn_expr_parser_parse (PnExprParser *self,
                      const gchar  *text,
                      GError      **error)
{
    Ctx         c = { 0 };
    PnExprNode *root;

    g_return_val_if_fail (PN_IS_EXPR_PARSER (self), NULL);

    if (text == NULL)
        text = "";

    c.input = text;
    c.p     = text;
    c.error = error;

    lex_advance (&c);           /* prime the first token */
    if (c.tok == TOK_ERROR)
        return NULL;

    /* parse_program consumes the whole input through TOK_END (rejecting
     * any trailing junk itself), so there is no post-check here. */
    root = parse_program (&c);
    if (root == NULL)
        return NULL;

    return root;
}
