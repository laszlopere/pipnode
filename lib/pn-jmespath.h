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

#ifndef PN_JMESPATH_H
#define PN_JMESPATH_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  pn-jmespath                                                        */
/*                                                                     */
/*  Self-contained evaluator for a useful subset of JMESPath           */
/*  (https://jmespath.org).  No external dependency beyond json-glib;  */
/*  the parser and evaluator operate on #JsonNode trees in-place so    */
/*  callers stay in the same JSON object model the rest of pipnode     */
/*  speaks.                                                            */
/*                                                                     */
/*  Supported language features:                                       */
/*    - identifier paths               a.b.c, "quoted"                 */
/*    - index / slice                  [0], [-1], [start:stop:step]    */
/*    - wildcards & projections        [*], .*, [?pred], slice projs   */
/*    - flatten                        []                              */
/*    - multi-select list / hash       [a, b], {k: a, ...}             */
/*    - pipes and parens               a | b, (a)                      */
/*    - current node                   @                               */
/*    - literals                       `<json>`, 'string', number      */
/*    - comparisons                    == != < <= > >=                 */
/*    - boolean logic                  && || !                         */
/*    - expression references          &expr                           */
/*    - function calls                 f(arg, ...)                     */
/*                                                                     */
/*  Built-in functions:                                                */
/*    length, type, keys, values, contains, starts_with, ends_with,    */
/*    to_string, to_number, to_array, abs, ceil, floor, sum, avg, max, */
/*    min, max_by, min_by, sort, sort_by, reverse, not_null, merge,    */
/*    join, map.                                                       */
/* ------------------------------------------------------------------ */

#define PN_JMESPATH_ERROR (pn_jmespath_error_quark ())
GQuark pn_jmespath_error_quark (void);

typedef enum
{
    PN_JMESPATH_ERROR_PARSE,
    PN_JMESPATH_ERROR_TYPE,
    PN_JMESPATH_ERROR_ARITY,
    PN_JMESPATH_ERROR_UNKNOWN_FUNCTION,
} PnJmespathError;

/**
 * pn_jmespath_search:
 * @expression: the JMESPath expression to evaluate; may be %NULL or
 *              empty, in which case a JSON-null result is returned.
 * @input:      (transfer none) (nullable): JSON data to query against.
 *              %NULL is treated as JSON null.
 * @error:      (out) (optional): set on parse or evaluation failure.
 *
 * Compiles @expression and evaluates it against @input.  Returns a
 * freshly-allocated #JsonNode the caller owns and must release with
 * json_node_unref().  A successful evaluation that produces "no
 * match" returns a JSON-null node, never %NULL — %NULL is reserved
 * for genuine errors with @error set.
 */
JsonNode *pn_jmespath_search (const gchar *expression,
                              JsonNode    *input,
                              GError     **error);

G_END_DECLS

#endif /* PN_JMESPATH_H */
