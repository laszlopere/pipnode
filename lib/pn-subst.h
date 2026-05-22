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

#ifndef PN_SUBST_H
#define PN_SUBST_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  ${...} placeholder substitution                                    */
/*                                                                     */
/*  One engine for every "${key}" template in Pipnode.  The scanner    */
/*  is fixed; what varies between call sites is captured by three      */
/*  knobs:                                                              */
/*                                                                     */
/*    - the OUTPUT MODE: plain text vs. JSON-embedding;                */
/*    - the MISSING-KEY policy: empty / JSON null / verbatim;          */
/*    - a RESOLVER CHAIN that maps a key to a value, decoupling the    */
/*      scanner from where values come from (a message's JSON, a       */
/*      node's variables, …).  Resolvers are tried in order, first     */
/*      hit wins, so one template can draw from several sources.       */
/* ------------------------------------------------------------------ */

/**
 * PnSubstMode:
 * @PN_SUBST_TEXT: scalars render in their natural textual form (strings
 *   as-is, numbers / booleans as text); objects and arrays fall back to
 *   compact JSON.
 * @PN_SUBST_JSON: the value is rendered as JSON suitable for inlining
 *   into a JSON document.  The engine tracks whether the cursor sits
 *   inside a JSON string literal: in a value slot strings emerge quoted
 *   and numbers / booleans / null as bare JSON tokens; inside a string
 *   literal strings emerge as their raw escaped contents.
 */
typedef enum
{
    PN_SUBST_TEXT,
    PN_SUBST_JSON
} PnSubstMode;

/**
 * PnSubstMiss:
 * @PN_SUBST_MISS_EMPTY: emit nothing.
 * @PN_SUBST_MISS_NULL: emit the JSON literal `null`.
 * @PN_SUBST_MISS_VERBATIM: re-emit the literal "${key}" untouched.
 *
 * What to do when no resolver yields a value for a key.
 */
typedef enum
{
    PN_SUBST_MISS_EMPTY,
    PN_SUBST_MISS_NULL,
    PN_SUBST_MISS_VERBATIM
} PnSubstMiss;

/**
 * PnSubstResolver:
 * @resolve: maps @key to a #JsonNode, or %NULL for a miss.  Sets
 *   @owned to %TRUE when the returned node must be json_node_unref()'d
 *   by the engine after rendering, %FALSE when it is borrowed and
 *   outlives the call.
 * @user_data: closure for @resolve.
 *
 * A source of placeholder values.  Stack-allocate one and initialise it
 * with a built-in helper (pn_subst_resolver_json /
 * pn_subst_resolver_strv) or by hand.  Returning a #JsonNode rather
 * than a string is what lets one renderer serve both output modes.
 */
typedef struct _PnSubstResolver PnSubstResolver;
struct _PnSubstResolver
{
    JsonNode *(*resolve) (PnSubstResolver *self,
                          const gchar     *key,
                          gboolean        *owned);
    gpointer  user_data;
};

/**
 * PnSubstContext:
 * @resolvers: %NULL-terminated array of resolvers, tried in order.
 *   May itself be %NULL (every key misses).
 * @mode: output mode.
 * @miss: missing-key policy.
 *
 * Binds a resolver chain to an output mode and a miss policy.  Stack-
 * allocate it; it owns no heap state of its own.
 */
typedef struct
{
    PnSubstResolver **resolvers;
    PnSubstMode       mode;
    PnSubstMiss       miss;
} PnSubstContext;

/**
 * pn_subst_expand:
 * @tmpl: template text; %NULL is treated as "".
 * @ctx: substitution context.
 *
 * Walks @tmpl, replacing each "${key}" with the rendered value the
 * context's resolver chain returns for @key.  An unterminated "${" (no
 * closing "}") is copied verbatim.
 *
 * Returns: (transfer full): a newly-allocated expanded string.
 */
gchar *pn_subst_expand (const gchar          *tmpl,
                        const PnSubstContext *ctx);

/**
 * pn_subst_resolver_json:
 * @r: caller-owned resolver struct to initialise (stack ok).
 * @root: lookup root, e.g. from pn_json_lookup_root_for_message().
 *   Borrowed; may be %NULL.
 *
 * Initialises @r to resolve keys as "/"-separated paths via
 * pn_json_resolve_path().  Returned nodes are borrowed from @root.
 */
void pn_subst_resolver_json (PnSubstResolver *r,
                             JsonObject      *root);

/**
 * pn_subst_resolver_strv:
 * @r: caller-owned resolver struct to initialise (stack ok).
 * @pairs: %NULL-terminated { key, value, key, value, …, %NULL } array
 *   of borrowed strings.  The array and its strings must outlive the
 *   pn_subst_expand() call.
 *
 * Initialises @r to resolve a key by exact (whole-key) match against
 * the names in @pairs, wrapping the matched value in a transient string
 * node.
 */
void pn_subst_resolver_strv (PnSubstResolver     *r,
                             const gchar * const *pairs);

G_END_DECLS

#endif /* PN_SUBST_H */
