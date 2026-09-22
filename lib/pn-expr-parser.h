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

#ifndef PN_EXPR_PARSER_H
#define PN_EXPR_PARSER_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnExprNode                                                         */
/*                                                                     */
/*  Abstract-syntax-tree node for the little algebraic language the    */
/*  parser accepts: numbers, variables, the arithmetic and comparison  */
/*  binary operators, unary minus, function calls of one or more       */
/*  arguments, variable assignment, and newline-separated statement     */
/*  sequences.                                                         */
/*  The tree is a plain tagged-union struct rather than a GObject — it  */
/*  is a short-lived value the parser produces and the evaluator        */
/*  consumes, and a struct keeps allocation and recursion cheap.  Free  */
/*  a whole tree with pn_expr_node_free().                              */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_EXPR_NODE_NUMBER,   /* literal:   uses .number                  */
    PN_EXPR_NODE_VARIABLE, /* identifier: uses .name                   */
    PN_EXPR_NODE_UNARY,    /* unary op:  .op ('-' or '~'), .left = operand */
    PN_EXPR_NODE_BINARY,   /* binary op: .op, .left/.right (see below)  */
    PN_EXPR_NODE_CALL,     /* function:  .name, .left = arg 1,
                            *            .right = ARG chain or NULL   */
    PN_EXPR_NODE_ASSIGN,   /* name = expr: .name target, .left = value */
    PN_EXPR_NODE_SEQ,      /* stmt list:  .left = stmt, .right = rest   */
    PN_EXPR_NODE_ARG,      /* call arg 2..N: .left = this argument,
                            *   .right = the next ARG or NULL.  APPENDED
                            *   (TODO #83.1): a value, not a layout, so
                            *   no plugin ABI bump — see below.        */
} PnExprNodeType;

typedef struct _PnExprNode PnExprNode;

struct _PnExprNode
{
    PnExprNodeType  type;
    gdouble         number; /* NUMBER */
    gchar          *name;   /* VARIABLE / CALL / ASSIGN target name */
    gchar           op;     /* UNARY '-' / '~'; BINARY op code (below)   */
    PnExprNode     *left;   /* binary lhs / unary / call arg 1 / ARG's
                             *   own argument / assign value / sequence
                             *   statement                              */
    PnExprNode     *right;  /* binary rhs / call's ARG chain (NULL for a
                             *   one-argument call) / next ARG / rest of
                             *   a sequence                             */

    /* A CALL of any arity: argument one is .left and arguments two
     * onwards hang off .right as a right-leaning chain of
     * PN_EXPR_NODE_ARG, exactly the way PN_EXPR_NODE_SEQ chains
     * statements — the precedent is in the same enum (TODO #83.1).
     * That is UNIFORM: even a two-argument call chains, rather than
     * arity 2 being special and 3+ chained, because the irregularity is
     * what would be got wrong later (83.1b).  sizeof (PnExprNode) does
     * not change and no field changes meaning, so this stays what
     * #81.2 bought: pn-expr-parser.h is INSTALLED public API reaching
     * plugins through pipnode.h, and neither an appended enum value nor
     * a chain is an ABI break.  The AST is short-lived and never
     * serialised, so nothing on disk needed migrating either. */

    /* Binary operator codes carried in .op.  Arithmetic operators use
     * their own character; the multi-character comparisons get a single
     * stand-in letter so .op stays a plain gchar:
     *     '+' '-' '*' '/'   arithmetic
     *     '%'               floored modulo (result takes the divisor's sign)
     *     '&' '|' '^'       bitwise and, or, xor
     *     'l' 'r'           shift left (<<), shift right (>>)
     *     '<' '>'           less-than, greater-than
     *     'L' 'G'           less-or-equal (<=), greater-or-equal (>=)
     *     '=' '!'           equal (==), not-equal (!=)
     * Every comparison evaluates to 1.0 (true) or 0.0 (false). */
};

/**
 * pn_expr_node_free:
 * @node: (nullable) (transfer full): root of an AST, or %NULL
 *
 * Recursively frees @node and every child, including the owned name
 * strings.  Safe to call with %NULL.
 */
void pn_expr_node_free (PnExprNode *node);

/* ------------------------------------------------------------------ */
/*  Errors                                                             */
/* ------------------------------------------------------------------ */

#define PN_EXPR_PARSER_ERROR (pn_expr_parser_error_quark ())

GQuark pn_expr_parser_error_quark (void);

typedef enum
{
    PN_EXPR_PARSER_ERROR_SYNTAX,            /* malformed token / number   */
    PN_EXPR_PARSER_ERROR_UNEXPECTED_TOKEN,  /* token in the wrong place   */
    PN_EXPR_PARSER_ERROR_UNEXPECTED_EOF,    /* ran out of input mid-parse */
    PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT,    /* call has the wrong arity   */
} PnExprParserError;

/* ------------------------------------------------------------------ */
/*  PnExprParser                                                       */
/*                                                                     */
/*  Parses a program of one or more newline-separated statements held  */
/*  in a string — e.g. "(12.3 * value1) + (1 / 2)", "sin(value) + 1",  */
/*  or a multi-line "a = value * 2\nb = a + 1\nb" — into a #PnExprNode  */
/*  tree.  The parser carries no per-parse state of its own, so a       */
/*  single instance can be reused for any number of parses.            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_EXPR_PARSER (pn_expr_parser_get_type ())

G_DECLARE_FINAL_TYPE (PnExprParser, pn_expr_parser, PN, EXPR_PARSER, GObject)

PnExprParser *pn_expr_parser_new (void);

/**
 * pn_expr_parser_parse:
 * @self:  the parser
 * @text:  expression source to parse
 * @error: (out) (optional): set on failure
 *
 * Parses @text into a freshly-allocated AST.  Supported grammar:
 * numbers (`12`, `12.3`), variables (`value`, `value1`), the arithmetic
 * operators `+ - * / %`, the bitwise operators `<< >> & ^ |` and unary
 * `~`, the comparison operators `< > <= >= == !=` (which yield 1.0 or 0.0
 * and bind looser than everything else), parentheses, unary minus, and
 * function calls (`sin(x)`, `atan2(y, x)`, `clamp(x, lo, hi)`, …) whose
 * comma-separated arguments number whatever the function declares, up to
 * four — lib/pn-expr-funcs.c holds the whole list and is the only place
 * a name is added.  A call
 * on a KNOWN function with the wrong number of arguments is a parse
 * error (#PN_EXPR_PARSER_ERROR_ARGUMENT_COUNT) — "log takes 1 or 2
 * arguments, got 3 at position 5", since an arity may be a RANGE
 * (TODO #83.2) — so a typo lights the
 * node up as it is typed rather than at the next message; a name the
 * language does not know parses and fails at evaluation instead, since
 * there is no arity to check it against.
 * Precedence, tightest first: unary, `* / %`, `+ -`, `<< >>`, `&`, `^`,
 * `|`, comparisons — Python's order, so `a & 1 == 1` is `(a & 1) == 1`.
 *
 * @text may hold several statements separated by newlines; blank lines
 * are ignored.  A statement is either an assignment `name = expr` (which
 * binds `name` for later statements) or a bare expression.  The value of
 * the whole program is the value of its last statement.
 *
 * Returns: (transfer full) (nullable): the AST root, freed by the
 *   caller with pn_expr_node_free(); %NULL with @error set on a parse
 *   failure.
 */
PnExprNode *pn_expr_parser_parse (PnExprParser *self,
                                  const gchar  *text,
                                  GError      **error);

G_END_DECLS

#endif /* PN_EXPR_PARSER_H */
