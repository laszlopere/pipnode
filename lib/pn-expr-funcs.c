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
 * adding ten.
 *
 * Where a name means something in C, C wins: `round` is therefore
 * half-AWAY-FROM-ZERO (round(0.5) is 1, round(-0.5) is -1, round(2.5)
 * is 3), not the banker's ties-to-even that some calculators use, and
 * `min`/`max` are fmin/fmax, which SKIP a NaN operand rather than
 * propagating it — min(nan, 3) is 3.  Both are written down in the two
 * help pages, because a reader cannot guess either one.
 *
 * `pow(x, y)` also exists to catch a mistake: `^` in this language is
 * bitwise XOR, not exponentiation, so `2 ^ 10` is 8 and not 1024. */
static const PnExprFunc builtin_funcs[] = {
    { "sin",   1, sin,       NULL  },
    { "cos",   1, cos,       NULL  },
    { "tan",   1, tan,       NULL  },
    { "log",   1, log,       NULL  },
    { "log10", 1, log10,     NULL  },
    { "exp",   1, exp,       NULL  },
    { "sqrt",  1, sqrt,      NULL  },
    { "abs",   1, fabs,      NULL  },
    { "floor", 1, floor,     NULL  },
    { "ceil",  1, ceil,      NULL  },
    { "asin",  1, asin,      NULL  },
    { "acos",  1, acos,      NULL  },
    { "atan",  1, atan,      NULL  },
    { "round", 1, round,     NULL  },
    { "trunc", 1, trunc,     NULL  },
    { "sign",  1, expr_sign, NULL  },
    { "atan2", 2, NULL,      atan2 },
    { "min",   2, NULL,      fmin  },
    { "max",   2, NULL,      fmax  },
    { "pow",   2, NULL,      pow   },
    { "hypot", 2, NULL,      hypot },
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
