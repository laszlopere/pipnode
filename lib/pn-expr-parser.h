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
/*  parser accepts: numbers, variables, the four binary operators,     */
/*  unary minus, and single-argument function calls.  The tree is a    */
/*  plain tagged-union struct rather than a GObject — it is a short-   */
/*  lived value the parser produces and the evaluator consumes, and a  */
/*  struct keeps allocation and recursion cheap.  Free a whole tree    */
/*  with pn_expr_node_free().                                          */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_EXPR_NODE_NUMBER,   /* literal:   uses .number                  */
    PN_EXPR_NODE_VARIABLE, /* identifier: uses .name                   */
    PN_EXPR_NODE_UNARY,    /* unary op:  .op ('-'), .left = operand    */
    PN_EXPR_NODE_BINARY,   /* binary op: .op ('+','-','*','/'), .left/.right */
    PN_EXPR_NODE_CALL,     /* function:  .name, .left = argument       */
} PnExprNodeType;

typedef struct _PnExprNode PnExprNode;

struct _PnExprNode
{
    PnExprNodeType  type;
    gdouble         number; /* NUMBER */
    gchar          *name;   /* VARIABLE name / CALL function name */
    gchar           op;     /* UNARY / BINARY operator character */
    PnExprNode     *left;   /* binary lhs / unary operand / call argument */
    PnExprNode     *right;  /* binary rhs */
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
} PnExprParserError;

/* ------------------------------------------------------------------ */
/*  PnExprParser                                                       */
/*                                                                     */
/*  Parses an algebraic expression held in a string — e.g.            */
/*  "(12.3 * value1) + (1 / 2)" or "sin(value) + 1" — into a          */
/*  #PnExprNode tree.  The parser carries no per-parse state of its    */
/*  own, so a single instance can be reused for any number of parses.  */
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
 * numbers (`12`, `12.3`), variables (`value`, `value1`), the binary
 * operators `+ - * /` with the usual precedence, parentheses, unary
 * minus, and single-argument function calls (`sin(x)`, `cos(x)`,
 * `log(x)`, …).
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
