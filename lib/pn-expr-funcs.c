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

#include "pn-expr-funcs.h"

#include <math.h>

/* `log(x)` is the natural logarithm and `log(x, base)` is
 * log(x)/log(base) — the first name in the language whose arity is a
 * RANGE rather than a number (TODO #83.2/#83.7).  Written as one row
 * with an N-ary implementation that dispatches on the count it was
 * actually given; base 10 and base 2 will get their own names later, and
 * this spelling is for the base a program computes rather than knows. */
static gdouble
expr_log (const gdouble *a, gint n)
{
    return (n == 1) ? log (a[0]) : log (a[0]) / log (a[1]);
}

/* clamp(x, lo, hi) — the first THREE-argument function in the language
 * and the specimen that proves 83.1's argument chain end to end.  It is
 * here rather than written as min(max(x, lo), hi) because that spelling
 * is the one people get the argument order wrong in (83.11b).
 *
 * Two decisions, both of which 83.11(b) and 83.18 insist are stated
 * rather than left to fall out:
 *  - CROSSED BOUNDS (hi < lo) return lo.  The lower bound wins; it is
 *    defensible, it is deterministic, and silence is not an option.
 *  - A NaN VALUE passes through as NaN.  fmin/fmax would have quietly
 *    turned it into a bound, which is the same "a NaN became a real
 *    number" mistake `sign` refuses to make (81.11).
 */
static gdouble
expr_clamp (const gdouble *a, gint n)
{
    const gdouble x = a[0], lo = a[1], hi = a[2];

    (void) n;                       /* fixed arity 3 */

    if (isnan (x))
        return x;
    if (hi < lo)
        return lo;

    return (x < lo) ? lo : (x > hi) ? hi : x;
}

/* `sign` is the one name in the table with no libm function behind it,
 * so it is written out — and its two edge cases are DECIDED here rather
 * than left to fall out (TODO #81.11):
 *
 *  - sign(0) is 0, and so is sign(-0.0).  Zero has no sign to report;
 *    answering 1 or -1 would make `sign(a - b)` claim an order where
 *    there is none, which is the whole reason a program asks.
 *  - sign(NaN) is NaN.  Every other function in the table lets a NaN
 *    through untouched (that is what libm does), and a NaN silently
 *    becoming 0 would read as "equal" — the one answer that is
 *    certainly wrong.
 */
static gdouble
expr_sign (gdouble x)
{
    if (isnan (x))
        return x;

    return (gdouble) ((x > 0.0) - (x < 0.0));
}

/* The built-in functions, one row each.  `log` is the natural logarithm
 * (matching C's log()); `log10` is the base-10 form; `abs` maps to
 * fabs() since every value in the language is a double.
 *
 * The list is settled DELIBERATELY rather than by importing libm
 * wholesale (TODO #81.1): every name here is one that an expression
 * somewhere actually wanted.  `atan2` is the first two-argument entry —
 * it is the one that forced this table to exist at all, because a
 * mechanics figure (TODO #80) wants atan2(dy, dx) in its first ten
 * lines and the language had no way to spell it.  Adding a function is
 * one row and nothing else, which is what TODO #81.11 then proved by
 * adding ten, and #83 goes on proving at three and four arguments.
 *
 * Where a name means something in C, C wins: `round` is therefore
 * half-AWAY-FROM-ZERO (round(0.5) is 1, round(-0.5) is -1, round(2.5)
 * is 3), not the banker's ties-to-even that some calculators use, and
 * `min`/`max` are fmin/fmax, which SKIP a NaN operand rather than
 * propagating it — min(nan, 3) is 3.  Both are written down in the two
 * help pages, because a reader cannot guess either one.
 *
 * `pow(x, y)` also exists to catch a mistake: `^` in this language is
 * bitwise XOR, not exponentiation, so `2 ^ 10` is 8 and not 1024.
 *
 * The three macros keep a row to one line whatever the struct grows:
 * PN_EXPR_FN1/FN2 for a fixed one or two arguments, PN_EXPR_FNN for a
 * fixed three or more, PN_EXPR_FNR for an arity RANGE (TODO #83.2).
 * `clamp` and `log(x[, base])` are the specimens that exercise those
 * last two paths end to end — the chain, the range and the N-operand
 * broadcast — the way `atan2` was #81's specimen for the comma. */
static const PnExprFunc builtin_funcs[] = {
    PN_EXPR_FN1 ("sin",   sin),
    PN_EXPR_FN1 ("cos",   cos),
    PN_EXPR_FN1 ("tan",   tan),
    PN_EXPR_FN1 ("asin",  asin),
    PN_EXPR_FN1 ("acos",  acos),
    PN_EXPR_FN1 ("atan",  atan),
    PN_EXPR_FNR ("log",   1, 2, expr_log),
    PN_EXPR_FN1 ("log10", log10),
    PN_EXPR_FN1 ("exp",   exp),
    PN_EXPR_FN1 ("sqrt",  sqrt),
    PN_EXPR_FN1 ("abs",   fabs),
    PN_EXPR_FN1 ("floor", floor),
    PN_EXPR_FN1 ("ceil",  ceil),
    PN_EXPR_FN1 ("round", round),
    PN_EXPR_FN1 ("trunc", trunc),
    PN_EXPR_FN1 ("sign",  expr_sign),
    PN_EXPR_FN2 ("atan2", atan2),
    PN_EXPR_FN2 ("min",   fmin),
    PN_EXPR_FN2 ("max",   fmax),
    PN_EXPR_FN2 ("pow",   pow),
    PN_EXPR_FN2 ("hypot", hypot),
    PN_EXPR_FNN ("clamp", 3, expr_clamp),
};

/* The named constants.  Two, and both of them are here because writing
 * 3.14159265 out by hand (TODO #79.5 did exactly that) is a rounding
 * error waiting to be copied. */
static const struct
{
    const gchar *name;
    gdouble      value;
}
builtin_constants[] = {
    { "pi", G_PI },
    { "e",  G_E  },
};

const PnExprFunc *
pn_expr_func_lookup (const gchar *name)
{
    gsize i;

    if (name == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS (builtin_funcs); i++)
        if (g_strcmp0 (builtin_funcs[i].name, name) == 0)
            return &builtin_funcs[i];

    return NULL;
}

gchar *
pn_expr_func_arity_phrase (const PnExprFunc *fn)
{
    if (fn == NULL)
        return g_strdup ("no arguments");

    if (fn->min_arity == fn->max_arity)
        return g_strdup_printf ("%d argument%s", fn->min_arity,
                                fn->min_arity == 1 ? "" : "s");

    if (fn->max_arity == fn->min_arity + 1)
        return g_strdup_printf ("%d or %d arguments",
                                fn->min_arity, fn->max_arity);

    return g_strdup_printf ("%d to %d arguments",
                            fn->min_arity, fn->max_arity);
}

gsize
pn_expr_func_count (void)
{
    return G_N_ELEMENTS (builtin_funcs);
}

const PnExprFunc *
pn_expr_func_nth (gsize index)
{
    if (index >= G_N_ELEMENTS (builtin_funcs))
        return NULL;

    return &builtin_funcs[index];
}

gboolean
pn_expr_constant_lookup (const gchar *name,
                         gdouble     *out_value)
{
    gsize i;

    if (name == NULL)
        return FALSE;

    for (i = 0; i < G_N_ELEMENTS (builtin_constants); i++)
        if (g_strcmp0 (builtin_constants[i].name, name) == 0)
        {
            if (out_value != NULL)
                *out_value = builtin_constants[i].value;
            return TRUE;
        }

    return FALSE;
}
