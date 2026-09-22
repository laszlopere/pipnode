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

#include "pn-expr-funcs.h"

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
    TOK_PERCENT,   /* %  */
    TOK_AMP,       /* &  */
    TOK_PIPE,      /* |  */
    TOK_CARET,     /* ^  */
    TOK_TILDE,     /* ~  */
    TOK_SHL,       /* << */
    TOK_SHR,       /* >> */
    TOK_LT,        /* <  */
    TOK_GT,        /* >  */
    TOK_LE,        /* <= */
    TOK_GE,        /* >= */
    TOK_EQ,        /* == */
    TOK_NE,        /* != */
    TOK_ASSIGN,    /* =  (statement-level assignment) */
    TOK_NEWLINE,   /* one or more newlines: statement separator */
    TOK_COMMA,     /* ,  (separates a call's arguments) */
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
    case '%': c->tok = TOK_PERCENT; c->p = p + 1; return;
    case '&': c->tok = TOK_AMP;    c->p = p + 1; return;
    case '|': c->tok = TOK_PIPE;   c->p = p + 1; return;
    case '^': c->tok = TOK_CARET;  c->p = p + 1; return;
    case '~': c->tok = TOK_TILDE;  c->p = p + 1; return;
    case ',': c->tok = TOK_COMMA;  c->p = p + 1; return;
    case '(': c->tok = TOK_LPAREN; c->p = p + 1; return;
    case ')': c->tok = TOK_RPAREN; c->p = p + 1; return;

    /* Comparisons: '<' and '>' stand alone or take a trailing '=', and
     * a doubled '<<' / '>>' is a shift; '!=' must be two characters — a lone '!' is an error, since the
     * language has no logical-not.  ('=' is handled separately: '==' is
     * equality, a lone '=' is assignment.) */
    case '<':
        if (p[1] == '=')      { c->tok = TOK_LE;  c->p = p + 2; }
        else if (p[1] == '<') { c->tok = TOK_SHL; c->p = p + 2; }
        else                  { c->tok = TOK_LT;  c->p = p + 1; }
        return;
    case '>':
        if (p[1] == '=')      { c->tok = TOK_GE;  c->p = p + 2; }
        else if (p[1] == '>') { c->tok = TOK_SHR; c->p = p + 2; }
        else                  { c->tok = TOK_GT;  c->p = p + 1; }
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
/*    expression := bitor  (CMP bitor)*       // CMP: < > <= >= == !=   */
/*    bitor      := bitxor ('|' bitxor)*                               */
/*    bitxor     := bitand ('^' bitand)*                               */
/*    bitand     := shift  ('&' shift)*                                */
/*    shift      := additive (('<<' | '>>') additive)*                 */
/*    additive   := term   (('+' | '-') term)*                         */
/*    term       := factor (('*' | '/' | '%') factor)*                 */
/*    factor     := NUMBER                                             */
/*                | IDENT '(' arglist ')'      // function call        */
/*                | IDENT                       // variable            */
/*                | '(' expression ')'                                 */
/*                | ('+' | '-') factor          // unary sign          */
/*                | '~' factor                  // bitwise not         */
/*    arglist    := expression (',' expression)*                       */
/*                                                                     */
/*  Comparisons sit at the lowest precedence level and are left-       */
/*  associative like the arithmetic operators; each yields 1.0/0.0.    */
/*  The bitwise operators rank as in Python, not C: all of them bind   */
/*  tighter than a comparison, so `a & 1 == 1` is `(a & 1) == 1`.      */
/*  A program is one or more newline-separated statements; its value   */
/*  is that of the last statement (see pn-var-store evaluation).       */
/* ------------------------------------------------------------------ */

static PnExprNode *parse_expression (Ctx *c);
static PnExprNode *parse_factor (Ctx *c);

/* Parse the bracketed argument list of a call to @name (the '(' is
 * already consumed) and build the CALL node, consuming the closing ')'.
 * Takes ownership of @name either way.
 *
 * Arguments: the first goes in .left and the rest hang off .right as a
 * chain of PN_EXPR_NODE_ARG (TODO #83.1), so a call of any arity costs
 * no growth in PnExprNode.  That is not a micro-optimisation:
 * pn-expr-parser.h is INSTALLED public API that reaches plugins through
 * pipnode.h, so a bigger struct would be an ABI break of the same class
 * as appending a PnNodeClass vfunc, with every plugin needing a rebuild.
 * An appended enum value is not (81.3 did the same to PnExprParserError).
 * One-argument trees stay bit-for-bit what they were.
 *
 * The COUNT is checked here, at parse time, against the shared arity
 * table — so `atan2(x)` lights the node up as it is typed rather than
 * on the next message (TODO #81.3) — and the table may declare a RANGE,
 * so `log(1, 2, 3)` is told what `log` actually takes (83.2).  A name
 * the table does not know has no arity to check, so it parses and the
 * evaluator reports it as an unknown function; more than
 * #PN_EXPR_MAX_ARITY arguments is a parse error whatever the name,
 * because no function in the language takes that many. */
static PnExprNode *parse_call_args (Ctx *c, gchar *name, gint column);

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

    case TOK_TILDE:
        {
            PnExprNode *operand, *n;
            lex_advance (c);
            operand = parse_factor (c);
            if (operand == NULL)
                return NULL;
            n = node_new (PN_EXPR_NODE_UNARY);
            n->op   = '~';
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
            gchar *name   = g_strndup (c->ident_start, c->ident_len);
            gint   column = ctx_column (c);   /* the name, for error messages */
            lex_advance (c);

            if (c->tok == TOK_LPAREN)
            {
                /* Function call: name '(' expression (',' expression)* ')'.
                 * parse_call_args() owns the whole bracketed part and the
                 * arity check; it consumes the ')' on success. */
                lex_advance (c);
                return parse_call_args (c, name, column);
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
parse_call_args (Ctx   *c,
                 gchar *name,
                 gint   column)
{
    const PnExprFunc *fn     = pn_expr_func_lookup (name);
    PnExprNode       *args[PN_EXPR_MAX_ARITY] = { NULL, };
    gint              n_args = 0;
    PnExprNode       *call;
    PnExprNode       *chain  = NULL;
    gint              i;

    for (;;)
    {
        PnExprNode *arg = parse_expression (c);

        if (arg == NULL)
            goto fail;

        if (n_args == PN_EXPR_MAX_ARITY)
        {
            pn_expr_node_free (arg);
            ctx_set_error (c, PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT,
                           "too many arguments to '%s' at position %d: "
                           "no function takes more than %d",
                           name, column, PN_EXPR_MAX_ARITY);
            goto fail;
        }
        args[n_args++] = arg;

        if (c->tok != TOK_COMMA)
            break;
        lex_advance (c);
    }

    if (c->tok != TOK_RPAREN)
    {
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,
                       "expected ')' after argument to '%s' "
                       "at position %d", name, ctx_column (c));
        goto fail;
    }

    /* An unknown name has no declared arity; let the evaluator be the one
     * that says so, which is where it said so before this entry.  A
     * known one may declare a RANGE (TODO #83.2), so the message comes
     * from the table rather than from a count — "log takes 1 or 2
     * arguments, got 3". */
    if (fn != NULL && (n_args < fn->min_arity || n_args > fn->max_arity))
    {
        gchar *phrase = pn_expr_func_arity_phrase (fn);
        ctx_set_error (c, PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT,
                       "%s takes %s, got %d at position %d",
                       name, phrase, n_args, column);
        g_free (phrase);
        goto fail;
    }

    lex_advance (c);                /* consume the ')' */

    /* Arguments 2..N become a right-leaning chain of ARG nodes hanging
     * off the call's .right (TODO #83.1).  Built back to front so each
     * node already has its successor; a two-argument call chains too,
     * because arity 2 being special is the irregularity 83.1(b) refused
     * to leave behind. */
    for (i = n_args - 1; i >= 1; i--)
    {
        PnExprNode *arg = node_new (PN_EXPR_NODE_ARG);
        arg->left  = args[i];
        arg->right = chain;
        chain = arg;
    }

    call = node_new (PN_EXPR_NODE_CALL);
    call->name  = name;             /* transfer ownership */
    call->left  = args[0];
    call->right = chain;            /* NULL for a one-argument call */
    return call;

fail:
    for (i = 0; i < n_args; i++)
        pn_expr_node_free (args[i]);
    g_free (name);
    return NULL;
}

static PnExprNode *
parse_term (Ctx *c)
{
    PnExprNode *left = parse_factor (c);
    if (left == NULL)
        return NULL;

    while (c->tok == TOK_STAR || c->tok == TOK_SLASH || c->tok == TOK_PERCENT)
    {
        gchar       op = (c->tok == TOK_STAR)  ? '*'
                       : (c->tok == TOK_SLASH) ? '/' : '%';
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

/** Map a bitwise-level token to its operator code (see the header), or
 *  '\0' if @tok is not one. */
static gchar
bitwise_op (TokenType tok)
{
    switch (tok)
    {
    case TOK_SHL:   return 'l';
    case TOK_SHR:   return 'r';
    case TOK_AMP:   return '&';
    case TOK_CARET: return '^';
    case TOK_PIPE:  return '|';
    default:        return '\0';
    }
}

/** One left-associative binary level: operands come from @next, and the
 *  level consumes every token whose bitwise_op() is @op_a or @op_b (pass
 *  the same code twice for a single-operator level). */
static PnExprNode *
parse_bitwise_level (Ctx         *c,
                     PnExprNode *(*next) (Ctx *),
                     gchar        op_a,
                     gchar        op_b)
{
    PnExprNode *left = next (c);
    gchar       op;

    if (left == NULL)
        return NULL;

    while ((op = bitwise_op (c->tok)) != '\0' && (op == op_a || op == op_b))
    {
        PnExprNode *right, *n;

        lex_advance (c);
        right = next (c);
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
parse_shift (Ctx *c)
{
    return parse_bitwise_level (c, parse_additive, 'l', 'r');
}

static PnExprNode *
parse_bitand (Ctx *c)
{
    return parse_bitwise_level (c, parse_shift, '&', '&');
}

static PnExprNode *
parse_bitxor (Ctx *c)
{
    return parse_bitwise_level (c, parse_bitand, '^', '^');
}

static PnExprNode *
parse_bitor (Ctx *c)
{
    return parse_bitwise_level (c, parse_bitxor, '|', '|');
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
    PnExprNode *left = parse_bitor (c);
    if (left == NULL)
        return NULL;

    for (gchar op; (op = comparison_op (c->tok)) != '\0'; )
    {
        PnExprNode *right, *n;

        lex_advance (c);
        right = parse_bitor (c);
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
