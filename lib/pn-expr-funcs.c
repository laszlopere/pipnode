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

/* The built-in functions, one row each.  `log` is the natural logarithm
 * (matching C's log()); `log10` is the base-10 form; `abs` maps to
 * fabs() since every value in the language is a double.
 *
 * The list is settled DELIBERATELY rather than by importing libm
 * wholesale (TODO #81.1): every name here is one that an expression
 * somewhere actually wanted.  `atan2` is the first two-argument entry —
 * it is the one that forced this table to exist at all, because a
 * mechanics figure (TODO #80) wants atan2(dy, dx) in its first ten
 * lines and the language had no way to spell it.  Adding the eleventh
 * function is now one row and nothing else. */
static const PnExprFunc builtin_funcs[] = {
    { "sin",   1, sin,   NULL  },
    { "cos",   1, cos,   NULL  },
    { "tan",   1, tan,   NULL  },
    { "log",   1, log,   NULL  },
    { "log10", 1, log10, NULL  },
    { "exp",   1, exp,   NULL  },
    { "sqrt",  1, sqrt,  NULL  },
    { "abs",   1, fabs,  NULL  },
    { "floor", 1, floor, NULL  },
    { "ceil",  1, ceil,  NULL  },
    { "atan2", 2, NULL,  atan2 },
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
