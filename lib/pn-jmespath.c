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

#include "pn-jmespath.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/*  Errors                                                             */
/* ================================================================== */

G_DEFINE_QUARK (pn-jmespath-error-quark, pn_jmespath_error)

/* ================================================================== */
/*  Lexer                                                              */
/* ================================================================== */

typedef enum
{
    TOK_EOF,
    TOK_IDENT,        /* unquoted or "quoted" identifier */
    TOK_NUMBER,       /* integer literal (in slice/index)         */
    TOK_STRING,       /* 'single-quoted' raw string literal       */
    TOK_LITERAL,      /* `<json>` literal — pre-parsed to a node  */
    TOK_DOT,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_COLON,
    TOK_STAR,
    TOK_AT,
    TOK_AMP,
    TOK_PIPE,
    TOK_OR,           /* ||  */
    TOK_AND,          /* &&  */
    TOK_NOT,          /* !   (unary)        */
    TOK_EQ,           /* ==  */
    TOK_NE,           /* !=  */
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,
    TOK_FILTER,       /* [?  — opens a filter predicate           */
    TOK_FLATTEN,      /* []  — flatten projection                 */
} TokKind;

typedef struct
{
    TokKind   kind;
    /* For IDENT and STRING: an owned string.                       */
    /* For NUMBER:           int64 in 'num' below.                  */
    /* For LITERAL:          owned JsonNode in 'node'.              */
    gchar    *text;
    gint64    num;
    JsonNode *node;
} Tok;

typedef struct
{
    GArray *toks;        /* array of Tok                           */
    guint   pos;
} Lexer;

static void
tok_clear (Tok *t)
{
    g_clear_pointer (&t->text, g_free);
    g_clear_pointer (&t->node, json_node_unref);
}

static gboolean
is_ident_start (gunichar c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static gboolean
is_ident_cont (gunichar c)
{
    return is_ident_start (c) || (c >= '0' && c <= '9');
}

static gboolean
parse_quoted_identifier (const gchar **pp, gchar **out, GError **error)
{
    /* JMESPath quoted identifiers are JSON strings: "...".  We
     * support the common subset (\\, \", \n, \t, \/, \uXXXX is left
     * as-is — pipnode doesn't need full JSON-string escapes for
     * field names). */
    const gchar *p = *pp + 1;
    GString     *out_str = g_string_new (NULL);

    while (*p != '\0' && *p != '"')
    {
        if (*p == '\\' && p[1] != '\0')
        {
            switch (p[1])
            {
            case '"':  g_string_append_c (out_str, '"');  break;
            case '\\': g_string_append_c (out_str, '\\'); break;
            case '/':  g_string_append_c (out_str, '/');  break;
            case 'n':  g_string_append_c (out_str, '\n'); break;
            case 't':  g_string_append_c (out_str, '\t'); break;
            case 'r':  g_string_append_c (out_str, '\r'); break;
            default:
                g_string_append_c (out_str, p[1]);
                break;
            }
            p += 2;
        }
        else
        {
            g_string_append_c (out_str, *p);
            p++;
        }
    }

    if (*p != '"')
    {
        g_string_free (out_str, TRUE);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "unterminated quoted identifier");
        return FALSE;
    }

    *out = g_string_free (out_str, FALSE);
    *pp  = p + 1;
    return TRUE;
}

static gboolean
parse_single_quoted (const gchar **pp, gchar **out, GError **error)
{
    const gchar *p = *pp + 1;
    GString     *out_str = g_string_new (NULL);

    while (*p != '\0' && *p != '\'')
    {
        if (*p == '\\' && (p[1] == '\'' || p[1] == '\\'))
        {
            g_string_append_c (out_str, p[1]);
            p += 2;
        }
        else
        {
            g_string_append_c (out_str, *p);
            p++;
        }
    }

    if (*p != '\'')
    {
        g_string_free (out_str, TRUE);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "unterminated raw string literal");
        return FALSE;
    }

    *out = g_string_free (out_str, FALSE);
    *pp  = p + 1;
    return TRUE;
}

static gboolean
parse_backquoted (const gchar **pp, JsonNode **out_node, GError **error)
{
    const gchar *p   = *pp + 1;
    GString     *raw = g_string_new (NULL);
    JsonParser  *jp;
    GError      *jerr = NULL;

    while (*p != '\0' && *p != '`')
    {
        if (*p == '\\' && p[1] == '`')
        {
            g_string_append_c (raw, '`');
            p += 2;
        }
        else
        {
            g_string_append_c (raw, *p);
            p++;
        }
    }

    if (*p != '`')
    {
        g_string_free (raw, TRUE);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "unterminated literal");
        return FALSE;
    }

    /* JMESPath allows bareword strings inside backticks (e.g.
     * `foo`).  json-glib's parser is strict, so try once and on
     * failure wrap the body in quotes and retry. */
    jp = json_parser_new ();
    if (!json_parser_load_from_data (jp, raw->str, raw->len, &jerr))
    {
        gchar *quoted;

        g_clear_error (&jerr);
        quoted = g_strdup_printf ("\"%s\"", raw->str);
        if (!json_parser_load_from_data (jp, quoted, -1, &jerr))
        {
            g_free (quoted);
            g_string_free (raw, TRUE);
            g_object_unref (jp);
            g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                         "invalid JSON literal: %s",
                         jerr ? jerr->message : "?");
            g_clear_error (&jerr);
            return FALSE;
        }
        g_free (quoted);
    }

    *out_node = json_node_copy (json_parser_get_root (jp));
    g_object_unref (jp);
    g_string_free (raw, TRUE);
    *pp = p + 1;
    return TRUE;
}

static gboolean
lex (const gchar *expr, Lexer *out, GError **error)
{
    const gchar *p = expr ? expr : "";

    out->toks = g_array_new (FALSE, FALSE, sizeof (Tok));
    g_array_set_clear_func (out->toks, (GDestroyNotify) tok_clear);
    out->pos = 0;

#define EMIT(K)              \
    do {                     \
        Tok t = { 0 };       \
        t.kind = (K);        \
        g_array_append_val (out->toks, t); \
    } while (0)

    while (*p != '\0')
    {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
            continue;
        }

        if (*p == '.') { EMIT (TOK_DOT);     p++; continue; }
        if (*p == '(') { EMIT (TOK_LPAREN);  p++; continue; }
        if (*p == ')') { EMIT (TOK_RPAREN);  p++; continue; }
        if (*p == ',') { EMIT (TOK_COMMA);   p++; continue; }
        if (*p == ':') { EMIT (TOK_COLON);   p++; continue; }
        if (*p == '@') { EMIT (TOK_AT);      p++; continue; }
        if (*p == '*') { EMIT (TOK_STAR);    p++; continue; }
        if (*p == '{') { EMIT (TOK_LBRACE);  p++; continue; }
        if (*p == '}') { EMIT (TOK_RBRACE);  p++; continue; }
        if (*p == ']') { EMIT (TOK_RBRACKET); p++; continue; }

        if (*p == '[')
        {
            if (p[1] == '?')      { EMIT (TOK_FILTER);  p += 2; continue; }
            if (p[1] == ']')      { EMIT (TOK_FLATTEN); p += 2; continue; }
            EMIT (TOK_LBRACKET);
            p++;
            continue;
        }

        if (*p == '|')
        {
            if (p[1] == '|') { EMIT (TOK_OR); p += 2; continue; }
            EMIT (TOK_PIPE); p++; continue;
        }
        if (*p == '&')
        {
            if (p[1] == '&') { EMIT (TOK_AND); p += 2; continue; }
            EMIT (TOK_AMP); p++; continue;
        }
        if (*p == '!')
        {
            if (p[1] == '=') { EMIT (TOK_NE); p += 2; continue; }
            EMIT (TOK_NOT); p++; continue;
        }
        if (*p == '=')
        {
            if (p[1] == '=') { EMIT (TOK_EQ); p += 2; continue; }
            g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                         "unexpected '=' (did you mean '==' ?)");
            return FALSE;
        }
        if (*p == '<')
        {
            if (p[1] == '=') { EMIT (TOK_LTE); p += 2; continue; }
            EMIT (TOK_LT); p++; continue;
        }
        if (*p == '>')
        {
            if (p[1] == '=') { EMIT (TOK_GTE); p += 2; continue; }
            EMIT (TOK_GT); p++; continue;
        }

        if (*p == '"')
        {
            Tok    t   = { 0 };
            gchar *str = NULL;
            if (!parse_quoted_identifier (&p, &str, error))
                return FALSE;
            t.kind = TOK_IDENT;
            t.text = str;
            g_array_append_val (out->toks, t);
            continue;
        }

        if (*p == '\'')
        {
            Tok    t   = { 0 };
            gchar *str = NULL;
            if (!parse_single_quoted (&p, &str, error))
                return FALSE;
            t.kind = TOK_STRING;
            t.text = str;
            g_array_append_val (out->toks, t);
            continue;
        }

        if (*p == '`')
        {
            Tok       t  = { 0 };
            JsonNode *jn = NULL;
            if (!parse_backquoted (&p, &jn, error))
                return FALSE;
            t.kind = TOK_LITERAL;
            t.node = jn;
            g_array_append_val (out->toks, t);
            continue;
        }

        if (*p == '-' || (*p >= '0' && *p <= '9'))
        {
            const gchar *start = p;
            Tok          t     = { 0 };
            gchar       *end   = NULL;
            gint64       n;

            if (*p == '-')
                p++;
            while (*p >= '0' && *p <= '9')
                p++;

            n = g_ascii_strtoll (start, &end, 10);
            if (end != p)
            {
                g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                             "invalid number");
                return FALSE;
            }
            t.kind = TOK_NUMBER;
            t.num  = n;
            g_array_append_val (out->toks, t);
            continue;
        }

        if (is_ident_start ((guchar) *p))
        {
            const gchar *start = p;
            Tok          t     = { 0 };
            while (is_ident_cont ((guchar) *p))
                p++;
            t.kind = TOK_IDENT;
            t.text = g_strndup (start, p - start);
            g_array_append_val (out->toks, t);
            continue;
        }

        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "unexpected character '%c'", *p);
        return FALSE;
    }

    EMIT (TOK_EOF);
#undef EMIT
    return TRUE;
}

static void
lex_clear (Lexer *l)
{
    g_clear_pointer (&l->toks, g_array_unref);
}

static Tok *
peek (Lexer *l)
{
    return &g_array_index (l->toks, Tok, l->pos);
}

static Tok *
peek_at (Lexer *l, guint offset)
{
    if (l->pos + offset >= l->toks->len)
        return &g_array_index (l->toks, Tok, l->toks->len - 1);
    return &g_array_index (l->toks, Tok, l->pos + offset);
}

static Tok *
advance (Lexer *l)
{
    Tok *t = peek (l);
    if (t->kind != TOK_EOF)
        l->pos++;
    return t;
}

static gboolean
expect (Lexer *l, TokKind k, GError **error)
{
    if (peek (l)->kind != k)
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "expected token %d, got %d", (int) k,
                     (int) peek (l)->kind);
        return FALSE;
    }
    advance (l);
    return TRUE;
}

/* ================================================================== */
/*  AST                                                                */
/* ================================================================== */

typedef enum
{
    AST_FIELD,         /* identifier (also used by multi-hash key)    */
    AST_SUBEXPR,       /* left . right                                */
    AST_INDEX,         /* base [N]                                    */
    AST_ARRAY_PROJ,    /* base [*] child                              */
    AST_OBJECT_PROJ,   /* base .* child  (or `*` at primary)          */
    AST_SLICE_PROJ,    /* base [start:stop:step] child                */
    AST_FLATTEN_PROJ,  /* base []  child                              */
    AST_FILTER_PROJ,   /* base [? predicate ] child                   */
    AST_MULTI_LIST,    /* [a, b, c]                                   */
    AST_MULTI_HASH,    /* {k: a, k2: b}                               */
    AST_FUNCTION,      /* f(arg, ...)                                 */
    AST_PIPE,          /* a | b                                       */
    AST_OR,            /* a || b                                      */
    AST_AND,           /* a && b                                      */
    AST_NOT,           /* ! a                                         */
    AST_COMPARE,       /* a OP b                                      */
    AST_LITERAL,       /* `<json>` / 'string' / number                */
    AST_CURRENT,       /* @                                           */
    AST_EXPR_REF,      /* &expr                                       */
    AST_IDENTITY,      /* synthetic — yields the input unchanged      */
} AstKind;

typedef struct Ast Ast;

struct Ast
{
    AstKind   kind;
    gchar    *name;       /* AST_FIELD / AST_FUNCTION                   */
    Ast      *left;       /* binary lhs / projection base               */
    Ast      *right;      /* binary rhs                                 */
    Ast      *child;      /* projection's tail expression               */
    Ast      *predicate;  /* AST_FILTER_PROJ                            */
    Ast      *expr;       /* AST_NOT, AST_EXPR_REF                      */
    GPtrArray *args;      /* AST_FUNCTION args / AST_MULTI_LIST exprs   */
    GPtrArray *keys;      /* AST_MULTI_HASH keys (gchar*, owned)        */
    gint64    idx;        /* AST_INDEX                                  */
    gint64    s_start;
    gint64    s_stop;
    gint64    s_step;
    gboolean  has_start;
    gboolean  has_stop;
    gboolean  has_step;
    int       op;         /* AST_COMPARE: TOK_EQ.. TOK_GTE              */
    JsonNode *node;       /* AST_LITERAL                                */
};

static void
ast_free (Ast *a)
{
    if (a == NULL)
        return;
    g_free (a->name);
    ast_free (a->left);
    ast_free (a->right);
    ast_free (a->child);
    ast_free (a->predicate);
    ast_free (a->expr);
    if (a->args)
        g_ptr_array_unref (a->args);
    if (a->keys)
        g_ptr_array_unref (a->keys);
    if (a->node)
        json_node_unref (a->node);
    g_free (a);
}

static Ast *
ast_new (AstKind k)
{
    Ast *a = g_new0 (Ast, 1);
    a->kind = k;
    return a;
}

/* ================================================================== */
/*  Parser                                                             */
/* ================================================================== */

static Ast *parse_expression (Lexer *l, GError **error);
static Ast *parse_or          (Lexer *l, GError **error);
static Ast *parse_and         (Lexer *l, GError **error);
static Ast *parse_not         (Lexer *l, GError **error);
static Ast *parse_compare     (Lexer *l, GError **error);
static Ast *parse_chain       (Lexer *l, GError **error);
static Ast *parse_postfix     (Lexer *l, Ast *base, GError **error);
static Ast *parse_primary     (Lexer *l, GError **error);
static Ast *parse_dot_rhs     (Lexer *l, Ast *base, GError **error);
static Ast *parse_multi_list  (Lexer *l, GError **error);
static Ast *parse_multi_hash  (Lexer *l, GError **error);

static Ast *
parse_expression (Lexer *l, GError **error)
{
    Ast *left = parse_or (l, error);
    if (left == NULL)
        return NULL;

    while (peek (l)->kind == TOK_PIPE)
    {
        Ast *node;
        advance (l);
        Ast *right = parse_or (l, error);
        if (right == NULL) { ast_free (left); return NULL; }
        node = ast_new (AST_PIPE);
        node->left = left;
        node->right = right;
        left = node;
    }
    return left;
}

static Ast *
parse_or (Lexer *l, GError **error)
{
    Ast *left = parse_and (l, error);
    if (left == NULL)
        return NULL;
    while (peek (l)->kind == TOK_OR)
    {
        Ast *node;
        advance (l);
        Ast *right = parse_and (l, error);
        if (right == NULL) { ast_free (left); return NULL; }
        node = ast_new (AST_OR);
        node->left = left;
        node->right = right;
        left = node;
    }
    return left;
}

static Ast *
parse_and (Lexer *l, GError **error)
{
    Ast *left = parse_not (l, error);
    if (left == NULL)
        return NULL;
    while (peek (l)->kind == TOK_AND)
    {
        Ast *node;
        advance (l);
        Ast *right = parse_not (l, error);
        if (right == NULL) { ast_free (left); return NULL; }
        node = ast_new (AST_AND);
        node->left = left;
        node->right = right;
        left = node;
    }
    return left;
}

static Ast *
parse_not (Lexer *l, GError **error)
{
    if (peek (l)->kind == TOK_NOT)
    {
        Ast *n;
        advance (l);
        Ast *e = parse_not (l, error);
        if (e == NULL) return NULL;
        n = ast_new (AST_NOT);
        n->expr = e;
        return n;
    }
    return parse_compare (l, error);
}

static Ast *
parse_compare (Lexer *l, GError **error)
{
    Ast *left = parse_chain (l, error);
    TokKind k;
    Ast *cmp, *right;

    if (left == NULL)
        return NULL;

    k = peek (l)->kind;
    if (k != TOK_EQ && k != TOK_NE && k != TOK_LT &&
        k != TOK_LTE && k != TOK_GT && k != TOK_GTE)
        return left;

    advance (l);
    right = parse_chain (l, error);
    if (right == NULL) { ast_free (left); return NULL; }

    cmp = ast_new (AST_COMPARE);
    cmp->op = (int) k;
    cmp->left = left;
    cmp->right = right;
    return cmp;
}

static Ast *
parse_chain (Lexer *l, GError **error)
{
    Ast *base = parse_primary (l, error);
    if (base == NULL)
        return NULL;
    return parse_postfix (l, base, error);
}

/** Consume the bracket body that starts after `[`.  The caller has
 *  already consumed `[`; this routine consumes through `]` and
 *  returns an AST node combining @base with the bracket form.       */
static Ast *
parse_lbracket_postfix (Lexer *l, Ast *base, GError **error)
{
    Tok    *t0 = peek (l);

    /* [*] — array projection */
    if (t0->kind == TOK_STAR && peek_at (l, 1)->kind == TOK_RBRACKET)
    {
        Ast *child;
        Ast *proj;
        advance (l);                /* * */
        advance (l);                /* ] */
        child = parse_postfix (l, ast_new (AST_IDENTITY), error);
        if (child == NULL) { ast_free (base); return NULL; }
        proj = ast_new (AST_ARRAY_PROJ);
        proj->left  = base;
        proj->child = child;
        return proj;
    }

    /* slice — [start:stop:step], any of which may be omitted.
     * Disambiguates from a single [N] index because a slice always
     * contains a colon. */
    {
        guint    save = l->pos;
        gboolean is_slice = FALSE;
        guint    i;

        for (i = 0; ; i++)
        {
            Tok *t = peek_at (l, i);
            if (t->kind == TOK_RBRACKET) break;
            if (t->kind == TOK_COLON) { is_slice = TRUE; break; }
            if (t->kind == TOK_NUMBER) continue;
            break;
        }
        (void) save;

        if (is_slice)
        {
            Ast     *proj;
            Ast     *child;
            gint64   parts[3] = { 0, 0, 1 };
            gboolean has[3]   = { FALSE, FALSE, FALSE };
            gint     slot     = 0;

            for (;;)
            {
                Tok *t = peek (l);
                if (t->kind == TOK_NUMBER)
                {
                    if (slot > 2) break;
                    parts[slot] = t->num;
                    has[slot]   = TRUE;
                    advance (l);
                }
                else if (t->kind == TOK_COLON)
                {
                    advance (l);
                    slot++;
                    if (slot > 2)
                    {
                        g_set_error (error, PN_JMESPATH_ERROR,
                                     PN_JMESPATH_ERROR_PARSE,
                                     "too many colons in slice");
                        ast_free (base);
                        return NULL;
                    }
                }
                else if (t->kind == TOK_RBRACKET)
                {
                    break;
                }
                else
                {
                    g_set_error (error, PN_JMESPATH_ERROR,
                                 PN_JMESPATH_ERROR_PARSE,
                                 "unexpected token in slice");
                    ast_free (base);
                    return NULL;
                }
            }
            if (!expect (l, TOK_RBRACKET, error)) { ast_free (base); return NULL; }

            child = parse_postfix (l, ast_new (AST_IDENTITY), error);
            if (child == NULL) { ast_free (base); return NULL; }

            proj = ast_new (AST_SLICE_PROJ);
            proj->left      = base;
            proj->s_start   = parts[0];
            proj->s_stop    = parts[1];
            proj->s_step    = has[2] ? parts[2] : 1;
            proj->has_start = has[0];
            proj->has_stop  = has[1];
            proj->has_step  = has[2];
            proj->child     = child;
            return proj;
        }
    }

    /* [N] — single index */
    if (t0->kind == TOK_NUMBER && peek_at (l, 1)->kind == TOK_RBRACKET)
    {
        Ast *idx;
        gint64 n = t0->num;
        advance (l);            /* number */
        advance (l);            /* ] */
        idx = ast_new (AST_INDEX);
        idx->left = base;
        idx->idx  = n;
        return idx;
    }

    g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                 "unexpected token after '['");
    ast_free (base);
    return NULL;
}

static Ast *
parse_postfix (Lexer *l, Ast *base, GError **error)
{
    for (;;)
    {
        Tok *t = peek (l);

        if (t->kind == TOK_DOT)
        {
            advance (l);
            base = parse_dot_rhs (l, base, error);
            if (base == NULL) return NULL;
            continue;
        }
        if (t->kind == TOK_LBRACKET)
        {
            advance (l);
            base = parse_lbracket_postfix (l, base, error);
            if (base == NULL) return NULL;
            continue;
        }
        if (t->kind == TOK_FLATTEN)
        {
            Ast *proj;
            Ast *child;
            advance (l);
            child = parse_postfix (l, ast_new (AST_IDENTITY), error);
            if (child == NULL) { ast_free (base); return NULL; }
            proj = ast_new (AST_FLATTEN_PROJ);
            proj->left  = base;
            proj->child = child;
            base = proj;
            continue;
        }
        if (t->kind == TOK_FILTER)
        {
            Ast *proj;
            Ast *pred;
            Ast *child;
            advance (l);
            pred = parse_expression (l, error);
            if (pred == NULL) { ast_free (base); return NULL; }
            if (!expect (l, TOK_RBRACKET, error))
            { ast_free (pred); ast_free (base); return NULL; }
            child = parse_postfix (l, ast_new (AST_IDENTITY), error);
            if (child == NULL) { ast_free (pred); ast_free (base); return NULL; }
            proj = ast_new (AST_FILTER_PROJ);
            proj->left      = base;
            proj->predicate = pred;
            proj->child     = child;
            base = proj;
            continue;
        }
        break;
    }
    return base;
}

static Ast *
parse_dot_rhs (Lexer *l, Ast *base, GError **error)
{
    Tok *t = peek (l);

    if (t->kind == TOK_STAR)
    {
        Ast *proj;
        Ast *child;
        advance (l);
        child = parse_postfix (l, ast_new (AST_IDENTITY), error);
        if (child == NULL) { ast_free (base); return NULL; }
        proj = ast_new (AST_OBJECT_PROJ);
        proj->left  = base;
        proj->child = child;
        return proj;
    }
    if (t->kind == TOK_LBRACKET)
    {
        Ast *list;
        Ast *sub;
        list = parse_multi_list (l, error);   /* consumes [ ... ] */
        if (list == NULL) { ast_free (base); return NULL; }
        sub = ast_new (AST_SUBEXPR);
        sub->left = base;
        sub->right = list;
        return sub;
    }
    if (t->kind == TOK_LBRACE)
    {
        Ast *hash;
        Ast *sub;
        hash = parse_multi_hash (l, error);
        if (hash == NULL) { ast_free (base); return NULL; }
        sub = ast_new (AST_SUBEXPR);
        sub->left = base;
        sub->right = hash;
        return sub;
    }
    if (t->kind == TOK_IDENT)
    {
        gchar *name = g_strdup (t->text);
        advance (l);
        if (peek (l)->kind == TOK_LPAREN)
        {
            /* dotted function call (e.g. inside multi-hash); rare,
             * but parse_primary handles it at entry, so here we
             * treat it as a free-standing function call appended
             * via subexpression. */
            Ast *fn = ast_new (AST_FUNCTION);
            Ast *sub;
            fn->name = name;
            fn->args = g_ptr_array_new_with_free_func (
                    (GDestroyNotify) ast_free);
            advance (l);  /* ( */
            if (peek (l)->kind != TOK_RPAREN)
            {
                for (;;)
                {
                    Ast *arg = parse_expression (l, error);
                    if (arg == NULL) { ast_free (fn); ast_free (base); return NULL; }
                    g_ptr_array_add (fn->args, arg);
                    if (peek (l)->kind == TOK_COMMA) { advance (l); continue; }
                    break;
                }
            }
            if (!expect (l, TOK_RPAREN, error))
            { ast_free (fn); ast_free (base); return NULL; }
            sub = ast_new (AST_SUBEXPR);
            sub->left = base;
            sub->right = fn;
            return sub;
        }
        else
        {
            Ast *field = ast_new (AST_FIELD);
            Ast *sub   = ast_new (AST_SUBEXPR);
            field->name = name;
            sub->left  = base;
            sub->right = field;
            return sub;
        }
    }

    g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                 "unexpected token after '.'");
    ast_free (base);
    return NULL;
}

static Ast *
parse_multi_list (Lexer *l, GError **error)
{
    Ast *list;
    if (!expect (l, TOK_LBRACKET, error)) return NULL;
    list = ast_new (AST_MULTI_LIST);
    list->args = g_ptr_array_new_with_free_func ((GDestroyNotify) ast_free);
    if (peek (l)->kind != TOK_RBRACKET)
    {
        for (;;)
        {
            Ast *e = parse_expression (l, error);
            if (e == NULL) { ast_free (list); return NULL; }
            g_ptr_array_add (list->args, e);
            if (peek (l)->kind == TOK_COMMA) { advance (l); continue; }
            break;
        }
    }
    if (!expect (l, TOK_RBRACKET, error)) { ast_free (list); return NULL; }
    return list;
}

static Ast *
parse_multi_hash (Lexer *l, GError **error)
{
    Ast *hash;
    if (!expect (l, TOK_LBRACE, error)) return NULL;
    hash = ast_new (AST_MULTI_HASH);
    hash->args = g_ptr_array_new_with_free_func ((GDestroyNotify) ast_free);
    hash->keys = g_ptr_array_new_with_free_func (g_free);
    if (peek (l)->kind != TOK_RBRACE)
    {
        for (;;)
        {
            Tok *kt = peek (l);
            Ast *e;
            if (kt->kind != TOK_IDENT && kt->kind != TOK_STRING)
            {
                g_set_error (error, PN_JMESPATH_ERROR,
                             PN_JMESPATH_ERROR_PARSE,
                             "multi-hash key must be identifier or string");
                ast_free (hash);
                return NULL;
            }
            g_ptr_array_add (hash->keys, g_strdup (kt->text));
            advance (l);
            if (!expect (l, TOK_COLON, error)) { ast_free (hash); return NULL; }
            e = parse_expression (l, error);
            if (e == NULL) { ast_free (hash); return NULL; }
            g_ptr_array_add (hash->args, e);
            if (peek (l)->kind == TOK_COMMA) { advance (l); continue; }
            break;
        }
    }
    if (!expect (l, TOK_RBRACE, error)) { ast_free (hash); return NULL; }
    return hash;
}

static Ast *
parse_primary (Lexer *l, GError **error)
{
    Tok *t = peek (l);

    if (t->kind == TOK_IDENT)
    {
        gchar *name = g_strdup (t->text);
        advance (l);
        if (peek (l)->kind == TOK_LPAREN)
        {
            Ast *fn = ast_new (AST_FUNCTION);
            fn->name = name;
            fn->args = g_ptr_array_new_with_free_func (
                    (GDestroyNotify) ast_free);
            advance (l);  /* ( */
            if (peek (l)->kind != TOK_RPAREN)
            {
                for (;;)
                {
                    Ast *arg = parse_expression (l, error);
                    if (arg == NULL) { ast_free (fn); return NULL; }
                    g_ptr_array_add (fn->args, arg);
                    if (peek (l)->kind == TOK_COMMA) { advance (l); continue; }
                    break;
                }
            }
            if (!expect (l, TOK_RPAREN, error)) { ast_free (fn); return NULL; }
            return fn;
        }
        else
        {
            Ast *f = ast_new (AST_FIELD);
            f->name = name;
            return f;
        }
    }

    if (t->kind == TOK_AT)
    {
        advance (l);
        return ast_new (AST_CURRENT);
    }

    if (t->kind == TOK_STAR)
    {
        Ast *proj;
        Ast *child;
        advance (l);
        child = parse_postfix (l, ast_new (AST_IDENTITY), error);
        if (child == NULL) return NULL;
        proj = ast_new (AST_OBJECT_PROJ);
        proj->left  = ast_new (AST_CURRENT);
        proj->child = child;
        return proj;
    }

    if (t->kind == TOK_NUMBER)
    {
        Ast *lit = ast_new (AST_LITERAL);
        lit->node = json_node_alloc ();
        json_node_init_int (lit->node, t->num);
        advance (l);
        return lit;
    }

    if (t->kind == TOK_STRING)
    {
        Ast *lit = ast_new (AST_LITERAL);
        lit->node = json_node_alloc ();
        json_node_init_string (lit->node, t->text);
        advance (l);
        return lit;
    }

    if (t->kind == TOK_LITERAL)
    {
        Ast *lit = ast_new (AST_LITERAL);
        lit->node = json_node_copy (t->node);
        advance (l);
        return lit;
    }

    if (t->kind == TOK_AMP)
    {
        Ast *r;
        Ast *e;
        advance (l);
        e = parse_expression (l, error);
        if (e == NULL) return NULL;
        r = ast_new (AST_EXPR_REF);
        r->expr = e;
        return r;
    }

    if (t->kind == TOK_LPAREN)
    {
        Ast *e;
        advance (l);
        e = parse_expression (l, error);
        if (e == NULL) return NULL;
        if (!expect (l, TOK_RPAREN, error)) { ast_free (e); return NULL; }
        return e;
    }

    if (t->kind == TOK_LBRACKET)
    {
        /* Disambiguate primary `[…]`.  When the brackets enclose a
         * number/colon/`*` (e.g. `[0]`, `[1:5]`, `[*]`) the spec
         * treats them as a postfix on the implicit current node —
         * the common `… | [0]` idiom relies on that.  Otherwise the
         * brackets open a multi-select-list. */
        Tok *next = peek_at (l, 1);
        if (next->kind == TOK_NUMBER ||
            next->kind == TOK_COLON  ||
            (next->kind == TOK_STAR &&
             peek_at (l, 2)->kind == TOK_RBRACKET))
        {
            advance (l);          /* consume [ */
            return parse_lbracket_postfix (l, ast_new (AST_CURRENT), error);
        }
        return parse_multi_list (l, error);
    }

    if (t->kind == TOK_LBRACE)
    {
        return parse_multi_hash (l, error);
    }

    if (t->kind == TOK_FLATTEN)
    {
        Ast *proj;
        Ast *child;
        advance (l);
        child = parse_postfix (l, ast_new (AST_IDENTITY), error);
        if (child == NULL) return NULL;
        proj = ast_new (AST_FLATTEN_PROJ);
        proj->left  = ast_new (AST_CURRENT);
        proj->child = child;
        return proj;
    }

    if (t->kind == TOK_FILTER)
    {
        Ast *proj;
        Ast *pred;
        Ast *child;
        advance (l);
        pred = parse_expression (l, error);
        if (pred == NULL) return NULL;
        if (!expect (l, TOK_RBRACKET, error)) { ast_free (pred); return NULL; }
        child = parse_postfix (l, ast_new (AST_IDENTITY), error);
        if (child == NULL) { ast_free (pred); return NULL; }
        proj = ast_new (AST_FILTER_PROJ);
        proj->left      = ast_new (AST_CURRENT);
        proj->predicate = pred;
        proj->child     = child;
        return proj;
    }

    g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                 "unexpected token");
    return NULL;
}

/* ================================================================== */
/*  JSON helpers                                                       */
/* ================================================================== */

static JsonNode *
json_null_node (void)
{
    JsonNode *n = json_node_alloc ();
    json_node_init_null (n);
    return n;
}

static gboolean
node_is_null (JsonNode *n)
{
    return n == NULL || JSON_NODE_HOLDS_NULL (n);
}

/** JMESPath truthiness: false, null, "", [], {} are false-y.        */
static gboolean
node_truthy (JsonNode *n)
{
    if (n == NULL || JSON_NODE_HOLDS_NULL (n))
        return FALSE;
    if (JSON_NODE_HOLDS_VALUE (n))
    {
        GType vt = json_node_get_value_type (n);
        if (vt == G_TYPE_BOOLEAN)
            return json_node_get_boolean (n);
        if (vt == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (n);
            return s != NULL && *s != '\0';
        }
        return TRUE;
    }
    if (JSON_NODE_HOLDS_ARRAY (n))
        return json_array_get_length (json_node_get_array (n)) > 0;
    if (JSON_NODE_HOLDS_OBJECT (n))
        return json_object_get_size (json_node_get_object (n)) > 0;
    return FALSE;
}

/** Deep structural equality.                                          */
static gboolean
nodes_equal (JsonNode *a, JsonNode *b)
{
    if (a == NULL && b == NULL) return TRUE;
    if (node_is_null (a) && node_is_null (b)) return TRUE;
    if (a == NULL || b == NULL) return FALSE;
    if (json_node_get_node_type (a) != json_node_get_node_type (b))
        return FALSE;

    if (JSON_NODE_HOLDS_VALUE (a))
    {
        GType ta = json_node_get_value_type (a);
        GType tb = json_node_get_value_type (b);
        if (ta == G_TYPE_BOOLEAN && tb == G_TYPE_BOOLEAN)
            return json_node_get_boolean (a) == json_node_get_boolean (b);
        if (ta == G_TYPE_STRING && tb == G_TYPE_STRING)
            return g_strcmp0 (json_node_get_string (a),
                              json_node_get_string (b)) == 0;
        /* Numeric comparison normalises through double so 1 == 1.0. */
        if ((ta == G_TYPE_INT64 || ta == G_TYPE_DOUBLE) &&
            (tb == G_TYPE_INT64 || tb == G_TYPE_DOUBLE))
        {
            gdouble da = (ta == G_TYPE_INT64)
                       ? (gdouble) json_node_get_int (a)
                       : json_node_get_double (a);
            gdouble db = (tb == G_TYPE_INT64)
                       ? (gdouble) json_node_get_int (b)
                       : json_node_get_double (b);
            return da == db;
        }
        return FALSE;
    }
    if (JSON_NODE_HOLDS_ARRAY (a))
    {
        JsonArray *aa = json_node_get_array (a);
        JsonArray *bb = json_node_get_array (b);
        guint i, n = json_array_get_length (aa);
        if (n != json_array_get_length (bb)) return FALSE;
        for (i = 0; i < n; i++)
            if (!nodes_equal (json_array_get_element (aa, i),
                              json_array_get_element (bb, i)))
                return FALSE;
        return TRUE;
    }
    if (JSON_NODE_HOLDS_OBJECT (a))
    {
        JsonObject *oa = json_node_get_object (a);
        JsonObject *ob = json_node_get_object (b);
        GList      *m, *l;
        if (json_object_get_size (oa) != json_object_get_size (ob))
            return FALSE;
        m = json_object_get_members (oa);
        for (l = m; l != NULL; l = l->next)
        {
            const gchar *k = l->data;
            if (!json_object_has_member (ob, k)) { g_list_free (m); return FALSE; }
            if (!nodes_equal (json_object_get_member (oa, k),
                              json_object_get_member (ob, k)))
            { g_list_free (m); return FALSE; }
        }
        g_list_free (m);
        return TRUE;
    }
    return TRUE;
}

static gboolean
nodes_compare_numeric (JsonNode *a, JsonNode *b, gint *out)
{
    GType ta, tb;
    gdouble da, db;
    if (a == NULL || b == NULL || !JSON_NODE_HOLDS_VALUE (a) ||
        !JSON_NODE_HOLDS_VALUE (b))
        return FALSE;
    ta = json_node_get_value_type (a);
    tb = json_node_get_value_type (b);
    if (ta != G_TYPE_INT64 && ta != G_TYPE_DOUBLE) return FALSE;
    if (tb != G_TYPE_INT64 && tb != G_TYPE_DOUBLE) return FALSE;
    da = (ta == G_TYPE_INT64) ? (gdouble) json_node_get_int (a)
                              : json_node_get_double (a);
    db = (tb == G_TYPE_INT64) ? (gdouble) json_node_get_int (b)
                              : json_node_get_double (b);
    *out = (da < db) ? -1 : (da > db) ? 1 : 0;
    return TRUE;
}

/* ================================================================== */
/*  Evaluator                                                          */
/* ================================================================== */

static JsonNode *eval (Ast *a, JsonNode *input, GError **error);

/** Run @child against every element of @arr, dropping null results,
 *  and collect into a fresh array node.                              */
static JsonNode *
project_array (JsonArray *arr, Ast *child, GError **error)
{
    JsonArray *out = json_array_new ();
    guint      i, n = json_array_get_length (arr);

    for (i = 0; i < n; i++)
    {
        JsonNode *el = json_array_get_element (arr, i);
        JsonNode *r;
        if (el == NULL) continue;
        r = eval (child, el, error);
        if (r == NULL)
        {
            json_array_unref (out);
            return NULL;
        }
        if (JSON_NODE_HOLDS_NULL (r))
        {
            json_node_unref (r);
            continue;
        }
        json_array_add_element (out, r);
    }
    {
        JsonNode *node = json_node_alloc ();
        json_node_init_array (node, out);
        json_array_unref (out);
        return node;
    }
}

/** Slice an array per JMESPath rules.  Negative indices count from
 *  the end; @step != 0 is required (caller checks).                 */
static JsonArray *
slice_array (JsonArray *arr,
             gint64    start, gint64 stop, gint64 step,
             gboolean  has_start, gboolean has_stop, gboolean has_step)
{
    gint64    n   = (gint64) json_array_get_length (arr);
    JsonArray *out = json_array_new ();
    gint64    s, e, i;

    (void) has_step;

    if (step == 0)
        step = 1;

    if (step > 0)
    {
        s = has_start ? start : 0;
        e = has_stop  ? stop  : n;
    }
    else
    {
        s = has_start ? start : (n - 1);
        e = has_stop  ? stop  : (-n - 1);
    }

    if (s < 0) s += n;
    if (e < 0 && (has_stop || step > 0)) e += n;

    if (step > 0)
    {
        if (s < 0) s = 0;
        if (e > n) e = n;
        for (i = s; i < e; i += step)
            json_array_add_element (out,
                    json_node_copy (json_array_get_element (arr, (guint) i)));
    }
    else
    {
        if (s >= n) s = n - 1;
        if (e < -1) e = -1;
        for (i = s; i > e; i += step)
        {
            if (i < 0 || i >= n) break;
            json_array_add_element (out,
                    json_node_copy (json_array_get_element (arr, (guint) i)));
        }
    }
    return out;
}

/* Forward decl for function dispatcher.                              */
static JsonNode *call_function (const gchar *name,
                                GPtrArray   *args,
                                JsonNode    *input,
                                GError     **error);

static JsonNode *
eval (Ast *a, JsonNode *input, GError **error)
{
    if (a == NULL)
        return json_null_node ();

    switch (a->kind)
    {
    case AST_IDENTITY:
    case AST_CURRENT:
        return input ? json_node_copy (input) : json_null_node ();

    case AST_FIELD:
    {
        if (input == NULL || !JSON_NODE_HOLDS_OBJECT (input))
            return json_null_node ();
        {
            JsonObject *o = json_node_get_object (input);
            if (!json_object_has_member (o, a->name))
                return json_null_node ();
            return json_node_copy (json_object_get_member (o, a->name));
        }
    }

    case AST_SUBEXPR:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonNode *r;
        if (l == NULL) return NULL;
        r = eval (a->right, l, error);
        json_node_unref (l);
        return r;
    }

    case AST_INDEX:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonArray *arr;
        gint64 idx, n;
        JsonNode *el;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_ARRAY (l))
        { json_node_unref (l); return json_null_node (); }
        arr = json_node_get_array (l);
        n   = (gint64) json_array_get_length (arr);
        idx = a->idx;
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n)
        { json_node_unref (l); return json_null_node (); }
        el = json_node_copy (json_array_get_element (arr, (guint) idx));
        json_node_unref (l);
        return el;
    }

    case AST_ARRAY_PROJ:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonNode *r;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_ARRAY (l))
        { json_node_unref (l); return json_null_node (); }
        r = project_array (json_node_get_array (l), a->child, error);
        json_node_unref (l);
        return r;
    }

    case AST_OBJECT_PROJ:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonObject *o;
        JsonArray  *out;
        GList      *members, *it;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_OBJECT (l))
        { json_node_unref (l); return json_null_node (); }
        o = json_node_get_object (l);
        out = json_array_new ();
        members = json_object_get_members (o);
        for (it = members; it != NULL; it = it->next)
        {
            JsonNode *v = json_object_get_member (o, it->data);
            JsonNode *r = eval (a->child, v, error);
            if (r == NULL)
            {
                g_list_free (members);
                json_array_unref (out);
                json_node_unref (l);
                return NULL;
            }
            if (JSON_NODE_HOLDS_NULL (r))
                json_node_unref (r);
            else
                json_array_add_element (out, r);
        }
        g_list_free (members);
        json_node_unref (l);
        {
            JsonNode *node = json_node_alloc ();
            json_node_init_array (node, out);
            json_array_unref (out);
            return node;
        }
    }

    case AST_SLICE_PROJ:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonArray *sliced;
        JsonNode *r;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_ARRAY (l))
        { json_node_unref (l); return json_null_node (); }
        sliced = slice_array (json_node_get_array (l),
                              a->s_start, a->s_stop, a->s_step,
                              a->has_start, a->has_stop, a->has_step);
        r = project_array (sliced, a->child, error);
        json_array_unref (sliced);
        json_node_unref (l);
        return r;
    }

    case AST_FLATTEN_PROJ:
    {
        JsonNode  *l = eval (a->left, input, error);
        JsonArray *arr;
        JsonArray *flat;
        JsonNode  *r;
        guint      i, n;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_ARRAY (l))
        { json_node_unref (l); return json_null_node (); }
        arr  = json_node_get_array (l);
        flat = json_array_new ();
        n    = json_array_get_length (arr);
        for (i = 0; i < n; i++)
        {
            JsonNode *el = json_array_get_element (arr, i);
            if (el != NULL && JSON_NODE_HOLDS_ARRAY (el))
            {
                JsonArray *inner = json_node_get_array (el);
                guint      j, m  = json_array_get_length (inner);
                for (j = 0; j < m; j++)
                    json_array_add_element (flat,
                        json_node_copy (json_array_get_element (inner, j)));
            }
            else if (el != NULL)
            {
                json_array_add_element (flat, json_node_copy (el));
            }
        }
        r = project_array (flat, a->child, error);
        json_array_unref (flat);
        json_node_unref (l);
        return r;
    }

    case AST_FILTER_PROJ:
    {
        JsonNode  *l = eval (a->left, input, error);
        JsonArray *arr;
        JsonArray *out;
        JsonNode  *r;
        guint      i, n;
        if (l == NULL) return NULL;
        if (!JSON_NODE_HOLDS_ARRAY (l))
        { json_node_unref (l); return json_null_node (); }
        arr = json_node_get_array (l);
        out = json_array_new ();
        n   = json_array_get_length (arr);
        for (i = 0; i < n; i++)
        {
            JsonNode *el = json_array_get_element (arr, i);
            JsonNode *p;
            JsonNode *child_r;
            if (el == NULL) continue;
            p = eval (a->predicate, el, error);
            if (p == NULL)
            { json_array_unref (out); json_node_unref (l); return NULL; }
            if (!node_truthy (p))
            { json_node_unref (p); continue; }
            json_node_unref (p);
            child_r = eval (a->child, el, error);
            if (child_r == NULL)
            { json_array_unref (out); json_node_unref (l); return NULL; }
            if (JSON_NODE_HOLDS_NULL (child_r))
                json_node_unref (child_r);
            else
                json_array_add_element (out, child_r);
        }
        json_node_unref (l);
        r = json_node_alloc ();
        json_node_init_array (r, out);
        json_array_unref (out);
        return r;
    }

    case AST_MULTI_LIST:
    {
        JsonArray *out;
        guint      i;
        if (input != NULL && JSON_NODE_HOLDS_NULL (input))
            return json_null_node ();
        out = json_array_new ();
        for (i = 0; i < a->args->len; i++)
        {
            Ast      *e = g_ptr_array_index (a->args, i);
            JsonNode *r = eval (e, input, error);
            if (r == NULL)
            { json_array_unref (out); return NULL; }
            json_array_add_element (out, r);
        }
        {
            JsonNode *node = json_node_alloc ();
            json_node_init_array (node, out);
            json_array_unref (out);
            return node;
        }
    }

    case AST_MULTI_HASH:
    {
        JsonObject *out;
        guint       i;
        if (input != NULL && JSON_NODE_HOLDS_NULL (input))
            return json_null_node ();
        out = json_object_new ();
        for (i = 0; i < a->args->len; i++)
        {
            const gchar *k = g_ptr_array_index (a->keys, i);
            Ast         *e = g_ptr_array_index (a->args, i);
            JsonNode    *r = eval (e, input, error);
            if (r == NULL)
            { json_object_unref (out); return NULL; }
            json_object_set_member (out, k, r);
        }
        {
            JsonNode *node = json_node_alloc ();
            json_node_init_object (node, out);
            json_object_unref (out);
            return node;
        }
    }

    case AST_PIPE:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonNode *r;
        if (l == NULL) return NULL;
        r = eval (a->right, l, error);
        json_node_unref (l);
        return r;
    }

    case AST_OR:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonNode *r;
        if (l == NULL) return NULL;
        if (node_truthy (l)) return l;
        json_node_unref (l);
        r = eval (a->right, input, error);
        return r;
    }

    case AST_AND:
    {
        JsonNode *l = eval (a->left, input, error);
        JsonNode *r;
        if (l == NULL) return NULL;
        if (!node_truthy (l)) return l;
        json_node_unref (l);
        r = eval (a->right, input, error);
        return r;
    }

    case AST_NOT:
    {
        JsonNode *e = eval (a->expr, input, error);
        JsonNode *r;
        if (e == NULL) return NULL;
        r = json_node_alloc ();
        json_node_init_boolean (r, !node_truthy (e));
        json_node_unref (e);
        return r;
    }

    case AST_COMPARE:
    {
        JsonNode *l = eval (a->left,  input, error);
        JsonNode *r;
        JsonNode *out;
        gboolean v = FALSE;
        if (l == NULL) return NULL;
        r = eval (a->right, input, error);
        if (r == NULL) { json_node_unref (l); return NULL; }
        switch (a->op)
        {
        case TOK_EQ: v =  nodes_equal (l, r); break;
        case TOK_NE: v = !nodes_equal (l, r); break;
        case TOK_LT: case TOK_LTE: case TOK_GT: case TOK_GTE:
        {
            gint cmp;
            if (!nodes_compare_numeric (l, r, &cmp))
                v = FALSE;
            else if (a->op == TOK_LT)  v = (cmp <  0);
            else if (a->op == TOK_LTE) v = (cmp <= 0);
            else if (a->op == TOK_GT)  v = (cmp >  0);
            else                       v = (cmp >= 0);
            break;
        }
        }
        json_node_unref (l);
        json_node_unref (r);
        out = json_node_alloc ();
        json_node_init_boolean (out, v);
        return out;
    }

    case AST_LITERAL:
        return json_node_copy (a->node);

    case AST_FUNCTION:
        return call_function (a->name, a->args, input, error);

    case AST_EXPR_REF:
        /* Expression references are only meaningful as direct
         * arguments to higher-order functions, which inspect the
         * AST_EXPR_REF node themselves.  Reaching this branch means
         * an &expr was used in a position that cannot consume it —
         * surface as a type error rather than crash. */
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "expression reference used in non-callable position");
        return NULL;
    }

    return json_null_node ();
}

/* ================================================================== */
/*  Built-in functions                                                 */
/* ================================================================== */

typedef struct
{
    Ast *ref;        /* AST_EXPR_REF node when arg was &expr           */
    JsonNode *value; /* otherwise: evaluated value (owned)             */
} FnArg;

static void
fnargs_free (FnArg *args, guint n)
{
    guint i;
    for (i = 0; i < n; i++)
        if (args[i].value)
            json_node_unref (args[i].value);
    g_free (args);
}

static FnArg *
eval_fn_args (GPtrArray *args, JsonNode *input,
              guint *out_n, GError **error)
{
    guint  i, n = args ? args->len : 0;
    FnArg *out  = g_new0 (FnArg, n);
    *out_n = n;
    for (i = 0; i < n; i++)
    {
        Ast *a = g_ptr_array_index (args, i);
        if (a->kind == AST_EXPR_REF)
        {
            out[i].ref = a;
        }
        else
        {
            out[i].value = eval (a, input, error);
            if (out[i].value == NULL)
            {
                fnargs_free (out, i);
                return NULL;
            }
        }
    }
    return out;
}

static gboolean
fn_arity (const gchar *name, guint got, guint want_min, guint want_max,
          GError **error)
{
    if (got < want_min || got > want_max)
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_ARITY,
                     "%s(): expected %u..%u arg(s), got %u",
                     name, want_min, want_max, got);
        return FALSE;
    }
    return TRUE;
}

static gboolean
expect_value_arg (const gchar *name, FnArg *a, GError **error)
{
    if (a->value == NULL)
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "%s(): expected value, got expression reference",
                     name);
        return FALSE;
    }
    return TRUE;
}

static gboolean
node_as_double (JsonNode *n, gdouble *out)
{
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n)) return FALSE;
    if (json_node_get_value_type (n) == G_TYPE_INT64)
    { *out = (gdouble) json_node_get_int (n); return TRUE; }
    if (json_node_get_value_type (n) == G_TYPE_DOUBLE)
    { *out = json_node_get_double (n); return TRUE; }
    return FALSE;
}

static JsonNode *
make_double_node (gdouble v)
{
    JsonNode *n = json_node_alloc ();
    /* Keep integer doubles as int for cleaner output. */
    if (v == (gdouble) (gint64) v)
        json_node_init_int (n, (gint64) v);
    else
        json_node_init_double (n, v);
    return n;
}

static gchar *
node_to_text (JsonNode *n)
{
    if (n == NULL || JSON_NODE_HOLDS_NULL (n))
        return g_strdup ("null");
    if (JSON_NODE_HOLDS_VALUE (n))
    {
        GType vt = json_node_get_value_type (n);
        if (vt == G_TYPE_STRING)
            return g_strdup (json_node_get_string (n));
        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (n));
        if (vt == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (n));
        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (n) ? "true" : "false");
    }
    {
        JsonGenerator *g = json_generator_new ();
        gchar         *s;
        json_generator_set_root (g, n);
        s = json_generator_to_data (g, NULL);
        g_object_unref (g);
        return s ? s : g_strdup ("");
    }
}

/** Compare two JsonNodes by an expression reference (@ref) applied
 *  to each, used by sort_by/min_by/max_by.  Returns -1/0/1, or sets
 *  @error and returns G_MININT on failure. */
static gint
cmp_by_ref (JsonNode *a, JsonNode *b, Ast *ref, GError **error)
{
    JsonNode *va = eval (ref->expr, a, error);
    JsonNode *vb;
    gint cmp;
    GType ta, tb;
    if (va == NULL) return G_MININT;
    vb = eval (ref->expr, b, error);
    if (vb == NULL) { json_node_unref (va); return G_MININT; }

    ta = JSON_NODE_HOLDS_VALUE (va) ? json_node_get_value_type (va) : 0;
    tb = JSON_NODE_HOLDS_VALUE (vb) ? json_node_get_value_type (vb) : 0;

    if (ta == G_TYPE_STRING && tb == G_TYPE_STRING)
    {
        cmp = g_strcmp0 (json_node_get_string (va),
                         json_node_get_string (vb));
    }
    else if (nodes_compare_numeric (va, vb, &cmp))
    {
        /* numeric */
    }
    else
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "sort/min/max key must be number or string");
        json_node_unref (va);
        json_node_unref (vb);
        return G_MININT;
    }
    json_node_unref (va);
    json_node_unref (vb);
    return cmp;
}

/* Each function below assumes its caller already validated arity and
 * arg kinds where needed.  All return a freshly-allocated JsonNode. */

static JsonNode *
fn_length (FnArg *a, GError **error)
{
    JsonNode *v = a[0].value;
    JsonNode *r = json_node_alloc ();
    if (v == NULL || JSON_NODE_HOLDS_NULL (v))
    { json_node_init_int (r, 0); return r; }
    if (JSON_NODE_HOLDS_ARRAY (v))
    { json_node_init_int (r, json_array_get_length (json_node_get_array (v))); return r; }
    if (JSON_NODE_HOLDS_OBJECT (v))
    { json_node_init_int (r, json_object_get_size (json_node_get_object (v))); return r; }
    if (JSON_NODE_HOLDS_VALUE (v) &&
        json_node_get_value_type (v) == G_TYPE_STRING)
    {
        const gchar *s = json_node_get_string (v);
        json_node_init_int (r, s ? (gint64) g_utf8_strlen (s, -1) : 0);
        return r;
    }
    json_node_unref (r);
    g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                 "length(): expected string|array|object");
    return NULL;
}

static JsonNode *
fn_type (FnArg *a, GError **error)
{
    JsonNode *v = a[0].value;
    JsonNode *r = json_node_alloc ();
    const gchar *t = "null";
    (void) error;
    if (v != NULL && !JSON_NODE_HOLDS_NULL (v))
    {
        if (JSON_NODE_HOLDS_ARRAY (v))    t = "array";
        else if (JSON_NODE_HOLDS_OBJECT (v)) t = "object";
        else
        {
            GType vt = json_node_get_value_type (v);
            if      (vt == G_TYPE_STRING)  t = "string";
            else if (vt == G_TYPE_INT64 ||
                     vt == G_TYPE_DOUBLE)  t = "number";
            else if (vt == G_TYPE_BOOLEAN) t = "boolean";
        }
    }
    json_node_init_string (r, t);
    return r;
}

static JsonNode *
fn_keys (FnArg *a, GError **error)
{
    JsonNode  *v = a[0].value;
    JsonArray *out;
    GList     *m, *l;
    if (v == NULL || !JSON_NODE_HOLDS_OBJECT (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "keys(): expected object");
        return NULL;
    }
    out = json_array_new ();
    m   = json_object_get_members (json_node_get_object (v));
    for (l = m; l != NULL; l = l->next)
    {
        JsonNode *s = json_node_alloc ();
        json_node_init_string (s, l->data);
        json_array_add_element (out, s);
    }
    g_list_free (m);
    {
        JsonNode *node = json_node_alloc ();
        json_node_init_array (node, out);
        json_array_unref (out);
        return node;
    }
}

static JsonNode *
fn_values (FnArg *a, GError **error)
{
    JsonNode  *v = a[0].value;
    JsonArray *out;
    GList     *m, *l;
    JsonObject *o;
    if (v == NULL || !JSON_NODE_HOLDS_OBJECT (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "values(): expected object");
        return NULL;
    }
    o   = json_node_get_object (v);
    out = json_array_new ();
    m   = json_object_get_members (o);
    for (l = m; l != NULL; l = l->next)
        json_array_add_element (out,
                json_node_copy (json_object_get_member (o, l->data)));
    g_list_free (m);
    {
        JsonNode *node = json_node_alloc ();
        json_node_init_array (node, out);
        json_array_unref (out);
        return node;
    }
}

static JsonNode *
fn_contains (FnArg *a, GError **error)
{
    JsonNode *subj = a[0].value;
    JsonNode *needle = a[1].value;
    JsonNode *r = json_node_alloc ();
    gboolean v = FALSE;
    if (subj == NULL || JSON_NODE_HOLDS_NULL (subj))
    { json_node_init_boolean (r, FALSE); return r; }
    if (JSON_NODE_HOLDS_VALUE (subj) &&
        json_node_get_value_type (subj) == G_TYPE_STRING &&
        needle != NULL && JSON_NODE_HOLDS_VALUE (needle) &&
        json_node_get_value_type (needle) == G_TYPE_STRING)
    {
        v = strstr (json_node_get_string (subj),
                    json_node_get_string (needle)) != NULL;
    }
    else if (JSON_NODE_HOLDS_ARRAY (subj))
    {
        JsonArray *arr = json_node_get_array (subj);
        guint i, n = json_array_get_length (arr);
        for (i = 0; i < n; i++)
            if (nodes_equal (json_array_get_element (arr, i), needle))
            { v = TRUE; break; }
    }
    else
    {
        json_node_unref (r);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "contains(): expected string or array as first arg");
        return NULL;
    }
    json_node_init_boolean (r, v);
    return r;
}

static JsonNode *
fn_starts_with (FnArg *a, GError **error)
{
    JsonNode *r = json_node_alloc ();
    JsonNode *s = a[0].value;
    JsonNode *p = a[1].value;
    if (s == NULL || p == NULL || !JSON_NODE_HOLDS_VALUE (s) ||
        !JSON_NODE_HOLDS_VALUE (p) ||
        json_node_get_value_type (s) != G_TYPE_STRING ||
        json_node_get_value_type (p) != G_TYPE_STRING)
    {
        json_node_unref (r);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "starts_with(): expected (string, string)");
        return NULL;
    }
    json_node_init_boolean (r,
            g_str_has_prefix (json_node_get_string (s),
                              json_node_get_string (p)));
    return r;
}

static JsonNode *
fn_ends_with (FnArg *a, GError **error)
{
    JsonNode *r = json_node_alloc ();
    JsonNode *s = a[0].value;
    JsonNode *p = a[1].value;
    if (s == NULL || p == NULL || !JSON_NODE_HOLDS_VALUE (s) ||
        !JSON_NODE_HOLDS_VALUE (p) ||
        json_node_get_value_type (s) != G_TYPE_STRING ||
        json_node_get_value_type (p) != G_TYPE_STRING)
    {
        json_node_unref (r);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "ends_with(): expected (string, string)");
        return NULL;
    }
    json_node_init_boolean (r,
            g_str_has_suffix (json_node_get_string (s),
                              json_node_get_string (p)));
    return r;
}

static JsonNode *
fn_to_string (FnArg *a, GError **error)
{
    JsonNode *v = a[0].value;
    JsonNode *r = json_node_alloc ();
    gchar    *s;
    (void) error;
    if (v != NULL && JSON_NODE_HOLDS_VALUE (v) &&
        json_node_get_value_type (v) == G_TYPE_STRING)
    {
        json_node_init_string (r, json_node_get_string (v));
        return r;
    }
    s = node_to_text (v);
    json_node_init_string (r, s);
    g_free (s);
    return r;
}

static JsonNode *
fn_to_number (FnArg *a, GError **error)
{
    JsonNode *v = a[0].value;
    JsonNode *r;
    (void) error;
    if (v != NULL && JSON_NODE_HOLDS_VALUE (v))
    {
        GType vt = json_node_get_value_type (v);
        if (vt == G_TYPE_INT64 || vt == G_TYPE_DOUBLE)
            return json_node_copy (v);
        if (vt == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (v);
            gchar *end = NULL;
            gdouble d;
            d = g_ascii_strtod (s, &end);
            if (end != s && *end == '\0')
                return make_double_node (d);
        }
    }
    r = json_node_alloc ();
    json_node_init_null (r);
    return r;
}

static JsonNode *
fn_to_array (FnArg *a, GError **error)
{
    JsonNode  *v = a[0].value;
    JsonArray *out;
    JsonNode  *r;
    (void) error;
    if (v != NULL && JSON_NODE_HOLDS_ARRAY (v))
        return json_node_copy (v);
    out = json_array_new ();
    if (v != NULL)
        json_array_add_element (out, json_node_copy (v));
    r = json_node_alloc ();
    json_node_init_array (r, out);
    json_array_unref (out);
    return r;
}

static JsonNode *
fn_abs (FnArg *a, GError **error)
{
    gdouble d;
    if (!node_as_double (a[0].value, &d))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "abs(): expected number");
        return NULL;
    }
    return make_double_node (fabs (d));
}

static JsonNode *
fn_ceil (FnArg *a, GError **error)
{
    gdouble d;
    if (!node_as_double (a[0].value, &d))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "ceil(): expected number");
        return NULL;
    }
    return make_double_node (ceil (d));
}

static JsonNode *
fn_floor (FnArg *a, GError **error)
{
    gdouble d;
    if (!node_as_double (a[0].value, &d))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "floor(): expected number");
        return NULL;
    }
    return make_double_node (floor (d));
}

static gboolean
collect_numeric_array (JsonNode *v, GArray **out, GError **error,
                       const gchar *name)
{
    JsonArray *arr;
    guint      i, n;
    GArray    *xs;
    if (v == NULL || !JSON_NODE_HOLDS_ARRAY (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "%s(): expected array of numbers", name);
        return FALSE;
    }
    arr = json_node_get_array (v);
    n   = json_array_get_length (arr);
    xs  = g_array_sized_new (FALSE, FALSE, sizeof (gdouble), n);
    for (i = 0; i < n; i++)
    {
        gdouble d;
        if (!node_as_double (json_array_get_element (arr, i), &d))
        {
            g_array_unref (xs);
            g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                         "%s(): non-number element", name);
            return FALSE;
        }
        g_array_append_val (xs, d);
    }
    *out = xs;
    return TRUE;
}

static JsonNode *
fn_sum (FnArg *a, GError **error)
{
    GArray *xs;
    gdouble s = 0;
    guint   i;
    if (!collect_numeric_array (a[0].value, &xs, error, "sum"))
        return NULL;
    for (i = 0; i < xs->len; i++)
        s += g_array_index (xs, gdouble, i);
    g_array_unref (xs);
    return make_double_node (s);
}

static JsonNode *
fn_avg (FnArg *a, GError **error)
{
    GArray *xs;
    gdouble s = 0;
    guint   i;
    JsonNode *r;
    if (!collect_numeric_array (a[0].value, &xs, error, "avg"))
        return NULL;
    if (xs->len == 0) { g_array_unref (xs); return json_null_node (); }
    for (i = 0; i < xs->len; i++)
        s += g_array_index (xs, gdouble, i);
    r = make_double_node (s / xs->len);
    g_array_unref (xs);
    return r;
}

static JsonNode *
fn_min_max (FnArg *a, GError **error, gboolean want_max)
{
    /* Works for all-number or all-string arrays. */
    JsonNode *v = a[0].value;
    JsonArray *arr;
    guint      i, n;
    JsonNode  *best = NULL;
    gboolean   numeric_mode = TRUE;
    if (v == NULL || !JSON_NODE_HOLDS_ARRAY (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "%s(): expected array", want_max ? "max" : "min");
        return NULL;
    }
    arr = json_node_get_array (v);
    n   = json_array_get_length (arr);
    if (n == 0) return json_null_node ();
    {
        JsonNode *first = json_array_get_element (arr, 0);
        if (first != NULL && JSON_NODE_HOLDS_VALUE (first) &&
            json_node_get_value_type (first) == G_TYPE_STRING)
            numeric_mode = FALSE;
    }
    for (i = 0; i < n; i++)
    {
        JsonNode *e = json_array_get_element (arr, i);
        if (best == NULL) { best = e; continue; }
        if (numeric_mode)
        {
            gint cmp;
            if (!nodes_compare_numeric (e, best, &cmp))
            {
                g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                             "%s(): mixed types", want_max ? "max" : "min");
                return NULL;
            }
            if ((want_max && cmp > 0) || (!want_max && cmp < 0))
                best = e;
        }
        else
        {
            const gchar *a_s, *b_s;
            if (!JSON_NODE_HOLDS_VALUE (e) ||
                json_node_get_value_type (e) != G_TYPE_STRING)
            {
                g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                             "%s(): mixed types", want_max ? "max" : "min");
                return NULL;
            }
            a_s = json_node_get_string (e);
            b_s = json_node_get_string (best);
            if ((want_max && g_strcmp0 (a_s, b_s) > 0) ||
                (!want_max && g_strcmp0 (a_s, b_s) < 0))
                best = e;
        }
    }
    return json_node_copy (best);
}

/* Sort helpers that share state via a thread-local-ish structure.
 * Avoids globals while keeping qsort's interface usable. */
typedef struct
{
    Ast    *ref;
    GError *error;
    gint    fail;
} SortCtx;

static int
sort_by_cmp (gconstpointer A, gconstpointer B, gpointer user)
{
    JsonNode **a = (JsonNode **) A;
    JsonNode **b = (JsonNode **) B;
    SortCtx  *ctx = user;
    gint cmp;
    if (ctx->fail) return 0;
    cmp = cmp_by_ref (*a, *b, ctx->ref, &ctx->error);
    if (cmp == G_MININT) { ctx->fail = 1; return 0; }
    return cmp;
}

static int
plain_sort_cmp (gconstpointer A, gconstpointer B, gpointer user)
{
    JsonNode **a = (JsonNode **) A;
    JsonNode **b = (JsonNode **) B;
    GType ta = JSON_NODE_HOLDS_VALUE (*a) ? json_node_get_value_type (*a) : 0;
    GType tb = JSON_NODE_HOLDS_VALUE (*b) ? json_node_get_value_type (*b) : 0;
    (void) user;
    if (ta == G_TYPE_STRING && tb == G_TYPE_STRING)
        return g_strcmp0 (json_node_get_string (*a),
                          json_node_get_string (*b));
    {
        gint cmp = 0;
        nodes_compare_numeric (*a, *b, &cmp);
        return cmp;
    }
}

static JsonNode *
fn_sort (FnArg *a, GError **error)
{
    JsonNode  *v = a[0].value;
    JsonArray *arr;
    guint      i, n;
    GPtrArray *items;
    JsonArray *out;
    JsonNode  *r;
    if (v == NULL || !JSON_NODE_HOLDS_ARRAY (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "sort(): expected array");
        return NULL;
    }
    arr = json_node_get_array (v);
    n   = json_array_get_length (arr);
    items = g_ptr_array_new_full (n, (GDestroyNotify) json_node_unref);
    for (i = 0; i < n; i++)
        g_ptr_array_add (items,
                json_node_copy (json_array_get_element (arr, i)));
    g_ptr_array_sort_with_data (items, plain_sort_cmp, NULL);
    out = json_array_new ();
    for (i = 0; i < items->len; i++)
        json_array_add_element (out,
                json_node_copy (g_ptr_array_index (items, i)));
    g_ptr_array_unref (items);
    r = json_node_alloc ();
    json_node_init_array (r, out);
    json_array_unref (out);
    return r;
}

static JsonNode *
fn_sort_by (FnArg *a, GError **error)
{
    JsonNode  *v = a[0].value;
    Ast       *ref = a[1].ref;
    JsonArray *arr;
    GPtrArray *items;
    JsonArray *out;
    SortCtx    ctx = { 0 };
    guint      i, n;
    JsonNode  *r;
    if (v == NULL || !JSON_NODE_HOLDS_ARRAY (v))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "sort_by(): expected (array, &expr)");
        return NULL;
    }
    if (ref == NULL)
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "sort_by(): second arg must be &expr");
        return NULL;
    }
    arr = json_node_get_array (v);
    n   = json_array_get_length (arr);
    items = g_ptr_array_new_full (n, (GDestroyNotify) json_node_unref);
    for (i = 0; i < n; i++)
        g_ptr_array_add (items,
                json_node_copy (json_array_get_element (arr, i)));
    ctx.ref = ref;
    g_ptr_array_sort_with_data (items, sort_by_cmp, &ctx);
    if (ctx.fail)
    {
        if (error && ctx.error) g_propagate_error (error, ctx.error);
        else g_clear_error (&ctx.error);
        g_ptr_array_unref (items);
        return NULL;
    }
    out = json_array_new ();
    for (i = 0; i < items->len; i++)
        json_array_add_element (out,
                json_node_copy (g_ptr_array_index (items, i)));
    g_ptr_array_unref (items);
    r = json_node_alloc ();
    json_node_init_array (r, out);
    json_array_unref (out);
    return r;
}

static JsonNode *
fn_min_max_by (FnArg *a, gboolean want_max, GError **error)
{
    JsonNode  *v   = a[0].value;
    Ast       *ref = a[1].ref;
    JsonArray *arr;
    guint      i, n;
    JsonNode  *best = NULL;
    if (v == NULL || !JSON_NODE_HOLDS_ARRAY (v) || ref == NULL)
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "%s_by(): expected (array, &expr)",
                     want_max ? "max" : "min");
        return NULL;
    }
    arr = json_node_get_array (v);
    n   = json_array_get_length (arr);
    if (n == 0) return json_null_node ();
    best = json_array_get_element (arr, 0);
    for (i = 1; i < n; i++)
    {
        JsonNode *cand = json_array_get_element (arr, i);
        gint cmp = cmp_by_ref (cand, best, ref, error);
        if (cmp == G_MININT) return NULL;
        if ((want_max && cmp > 0) || (!want_max && cmp < 0))
            best = cand;
    }
    return json_node_copy (best);
}

static JsonNode *
fn_reverse (FnArg *a, GError **error)
{
    JsonNode *v = a[0].value;
    if (v != NULL && JSON_NODE_HOLDS_VALUE (v) &&
        json_node_get_value_type (v) == G_TYPE_STRING)
    {
        gchar    *r = g_utf8_strreverse (json_node_get_string (v), -1);
        JsonNode *n = json_node_alloc ();
        json_node_init_string (n, r);
        g_free (r);
        return n;
    }
    if (v != NULL && JSON_NODE_HOLDS_ARRAY (v))
    {
        JsonArray *arr = json_node_get_array (v);
        JsonArray *out = json_array_new ();
        gint       i, n = (gint) json_array_get_length (arr);
        JsonNode  *r;
        for (i = n - 1; i >= 0; i--)
            json_array_add_element (out,
                    json_node_copy (json_array_get_element (arr, (guint) i)));
        r = json_node_alloc ();
        json_node_init_array (r, out);
        json_array_unref (out);
        return r;
    }
    g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                 "reverse(): expected string or array");
    return NULL;
}

static JsonNode *
fn_not_null (FnArg *args, guint n, GError **error)
{
    guint i;
    (void) error;
    for (i = 0; i < n; i++)
        if (args[i].value != NULL && !JSON_NODE_HOLDS_NULL (args[i].value))
            return json_node_copy (args[i].value);
    return json_null_node ();
}

static JsonNode *
fn_merge (FnArg *args, guint n, GError **error)
{
    JsonObject *out = json_object_new ();
    guint       i;
    JsonNode   *r;
    for (i = 0; i < n; i++)
    {
        JsonNode *v = args[i].value;
        JsonObject *o;
        GList *m, *l;
        if (v == NULL || !JSON_NODE_HOLDS_OBJECT (v))
        {
            json_object_unref (out);
            g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                         "merge(): all args must be objects");
            return NULL;
        }
        o = json_node_get_object (v);
        m = json_object_get_members (o);
        for (l = m; l != NULL; l = l->next)
            json_object_set_member (out, l->data,
                    json_node_copy (json_object_get_member (o, l->data)));
        g_list_free (m);
    }
    r = json_node_alloc ();
    json_node_init_object (r, out);
    json_object_unref (out);
    return r;
}

static JsonNode *
fn_join (FnArg *a, GError **error)
{
    JsonNode  *sep = a[0].value;
    JsonNode  *arr = a[1].value;
    JsonArray *jarr;
    GString   *out;
    guint      i, n;
    JsonNode  *r;
    if (sep == NULL || !JSON_NODE_HOLDS_VALUE (sep) ||
        json_node_get_value_type (sep) != G_TYPE_STRING ||
        arr == NULL || !JSON_NODE_HOLDS_ARRAY (arr))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "join(): expected (string, array of strings)");
        return NULL;
    }
    jarr = json_node_get_array (arr);
    n    = json_array_get_length (jarr);
    out  = g_string_new (NULL);
    for (i = 0; i < n; i++)
    {
        JsonNode *e = json_array_get_element (jarr, i);
        if (e == NULL || !JSON_NODE_HOLDS_VALUE (e) ||
            json_node_get_value_type (e) != G_TYPE_STRING)
        {
            g_string_free (out, TRUE);
            g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                         "join(): array element is not a string");
            return NULL;
        }
        if (i > 0) g_string_append (out, json_node_get_string (sep));
        g_string_append (out, json_node_get_string (e));
    }
    r = json_node_alloc ();
    json_node_init_string (r, out->str);
    g_string_free (out, TRUE);
    return r;
}

static JsonNode *
fn_map (FnArg *a, GError **error)
{
    Ast       *ref = a[0].ref;
    JsonNode  *arr_n = a[1].value;
    JsonArray *arr;
    JsonArray *out;
    guint      i, n;
    JsonNode  *r;
    if (ref == NULL || arr_n == NULL || !JSON_NODE_HOLDS_ARRAY (arr_n))
    {
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_TYPE,
                     "map(): expected (&expr, array)");
        return NULL;
    }
    arr = json_node_get_array (arr_n);
    n   = json_array_get_length (arr);
    out = json_array_new ();
    for (i = 0; i < n; i++)
    {
        JsonNode *m = eval (ref->expr, json_array_get_element (arr, i), error);
        if (m == NULL) { json_array_unref (out); return NULL; }
        json_array_add_element (out, m);
    }
    r = json_node_alloc ();
    json_node_init_array (r, out);
    json_array_unref (out);
    return r;
}

static JsonNode *
call_function (const gchar *name, GPtrArray *args, JsonNode *input,
               GError **error)
{
    FnArg   *fa;
    guint    n;
    JsonNode *r = NULL;

    fa = eval_fn_args (args, input, &n, error);
    if (fa == NULL) return NULL;

#define ARITY(min, max)                                                   \
    do { if (!fn_arity (name, n, (min), (max), error))                    \
            { fnargs_free (fa, n); return NULL; } } while (0)
#define VALUE_AT(i)                                                       \
    do { if (!expect_value_arg (name, &fa[(i)], error))                   \
            { fnargs_free (fa, n); return NULL; } } while (0)

    if      (g_strcmp0 (name, "length") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_length (fa, error); }
    else if (g_strcmp0 (name, "type") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_type (fa, error); }
    else if (g_strcmp0 (name, "keys") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_keys (fa, error); }
    else if (g_strcmp0 (name, "values") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_values (fa, error); }
    else if (g_strcmp0 (name, "contains") == 0)
    { ARITY (2, 2); VALUE_AT (0); VALUE_AT (1); r = fn_contains (fa, error); }
    else if (g_strcmp0 (name, "starts_with") == 0)
    { ARITY (2, 2); VALUE_AT (0); VALUE_AT (1); r = fn_starts_with (fa, error); }
    else if (g_strcmp0 (name, "ends_with") == 0)
    { ARITY (2, 2); VALUE_AT (0); VALUE_AT (1); r = fn_ends_with (fa, error); }
    else if (g_strcmp0 (name, "to_string") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_to_string (fa, error); }
    else if (g_strcmp0 (name, "to_number") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_to_number (fa, error); }
    else if (g_strcmp0 (name, "to_array") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_to_array (fa, error); }
    else if (g_strcmp0 (name, "abs") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_abs (fa, error); }
    else if (g_strcmp0 (name, "ceil") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_ceil (fa, error); }
    else if (g_strcmp0 (name, "floor") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_floor (fa, error); }
    else if (g_strcmp0 (name, "sum") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_sum (fa, error); }
    else if (g_strcmp0 (name, "avg") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_avg (fa, error); }
    else if (g_strcmp0 (name, "max") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_min_max (fa, error, TRUE); }
    else if (g_strcmp0 (name, "min") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_min_max (fa, error, FALSE); }
    else if (g_strcmp0 (name, "max_by") == 0)
    { ARITY (2, 2); VALUE_AT (0); r = fn_min_max_by (fa, TRUE, error); }
    else if (g_strcmp0 (name, "min_by") == 0)
    { ARITY (2, 2); VALUE_AT (0); r = fn_min_max_by (fa, FALSE, error); }
    else if (g_strcmp0 (name, "sort") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_sort (fa, error); }
    else if (g_strcmp0 (name, "sort_by") == 0)
    { ARITY (2, 2); VALUE_AT (0); r = fn_sort_by (fa, error); }
    else if (g_strcmp0 (name, "reverse") == 0)
    { ARITY (1, 1); VALUE_AT (0); r = fn_reverse (fa, error); }
    else if (g_strcmp0 (name, "not_null") == 0)
    { /* arity ≥ 1, all values */ guint i;
        if (n < 1) { g_set_error (error, PN_JMESPATH_ERROR,
                                  PN_JMESPATH_ERROR_ARITY,
                                  "not_null(): expected ≥1 arg");
                     fnargs_free (fa, n); return NULL; }
        for (i = 0; i < n; i++) VALUE_AT (i);
        r = fn_not_null (fa, n, error); }
    else if (g_strcmp0 (name, "merge") == 0)
    { guint i;
        if (n < 1) { g_set_error (error, PN_JMESPATH_ERROR,
                                  PN_JMESPATH_ERROR_ARITY,
                                  "merge(): expected ≥1 arg");
                     fnargs_free (fa, n); return NULL; }
        for (i = 0; i < n; i++) VALUE_AT (i);
        r = fn_merge (fa, n, error); }
    else if (g_strcmp0 (name, "join") == 0)
    { ARITY (2, 2); VALUE_AT (0); VALUE_AT (1); r = fn_join (fa, error); }
    else if (g_strcmp0 (name, "map") == 0)
    { ARITY (2, 2); /* a[0] is &expr, a[1] is value */ VALUE_AT (1);
      r = fn_map (fa, error); }
    else
    {
        g_set_error (error, PN_JMESPATH_ERROR,
                     PN_JMESPATH_ERROR_UNKNOWN_FUNCTION,
                     "unknown function: %s", name);
    }

#undef ARITY
#undef VALUE_AT

    fnargs_free (fa, n);
    return r;
}

/* ================================================================== */
/*  Public entry point                                                 */
/* ================================================================== */

JsonNode *
pn_jmespath_search (const gchar *expression,
                    JsonNode    *input,
                    GError     **error)
{
    Lexer     l = { 0 };
    Ast      *ast;
    JsonNode *result;
    JsonNode *root_input;

    if (expression == NULL || *expression == '\0')
        return json_null_node ();

    if (!lex (expression, &l, error))
    {
        lex_clear (&l);
        return NULL;
    }

    ast = parse_expression (&l, error);
    if (ast == NULL)
    {
        lex_clear (&l);
        return NULL;
    }
    if (peek (&l)->kind != TOK_EOF)
    {
        ast_free (ast);
        lex_clear (&l);
        g_set_error (error, PN_JMESPATH_ERROR, PN_JMESPATH_ERROR_PARSE,
                     "trailing tokens after expression");
        return NULL;
    }

    root_input = (input != NULL) ? input : json_null_node ();
    result = eval (ast, root_input, error);
    if (input == NULL)
        json_node_unref (root_input);

    ast_free (ast);
    lex_clear (&l);

    if (result == NULL && error != NULL && *error == NULL)
        result = json_null_node ();
    return result;
}
