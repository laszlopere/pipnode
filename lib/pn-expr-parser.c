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
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_ERROR,
} TokenType;

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

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    c->tok_start = p;

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
    default:
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_SYNTAX,
                       "unexpected character '%c' at position %d",
                       *p, (gint) (p - c->input) + 1);
        c->tok = TOK_ERROR;
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  Recursive-descent parser                                          */
/*                                                                     */
/*    expression := term   (('+' | '-') term)*                         */
/*    term       := factor (('*' | '/') factor)*                       */
/*    factor     := NUMBER                                             */
/*                | IDENT '(' expression ')'   // function call        */
/*                | IDENT                       // variable            */
/*                | '(' expression ')'                                 */
/*                | ('+' | '-') factor          // unary sign          */
/* ------------------------------------------------------------------ */

static PnExprNode *parse_expression (Ctx *c);

static PnExprNode *
parse_factor (Ctx *c)
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
parse_expression (Ctx *c)
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

    root = parse_expression (&c);
    if (root == NULL)
        return NULL;

    if (c.tok != TOK_END)
    {
        ctx_set_error (&c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                       "unexpected trailing input at position %d",
                       ctx_column (&c));
        pn_expr_node_free (root);
        return NULL;
    }

    return root;
}
