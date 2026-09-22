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

/* THE RECIPROCAL TRIG THREE (TODO #83.5).  None is in libm under that
 * name, so each is the single division it is — written out the way
 * `sign` is, rather than special-cased.  Their POLES are documented and
 * not defended against: cot and csc blow up at every multiple of pi
 * (cot(0) is +inf), sec a quarter turn away at pi/2 and every pi after
 * it.  An infinity is a value here (see THE NaN AND DOMAIN POLICY
 * below), so a pole travels down the wire like any other number. */
static gdouble
expr_cot (gdouble x)
{
    return cos (x) / sin (x);
}

static gdouble
expr_sec (gdouble x)
{
    return 1.0 / cos (x);
}

static gdouble
expr_csc (gdouble x)
{
    return 1.0 / sin (x);
}

/* `degrees` and `radians` (TODO #83.6) — the least mathematical rows in
 * the table and among the most useful.  Every trig function here takes
 * radians while every knob, dial and compass bearing a worksheet carries
 * is in degrees, which is why two example sheets had a hand-written
 * 0.017453293 in them until #81 gave them `pi` and this gives them the
 * name they meant. */
static gdouble
expr_degrees (gdouble x)
{
    return x * (180.0 / G_PI);
}

static gdouble
expr_radians (gdouble x)
{
    return x * (G_PI / 180.0);
}

/* SELECTION AND SHAPING (TODO #83.11) — the four rows the argument
 * chain was built for.  A two-argument language cannot express a
 * CHOICE, and a drawing language that cannot choose is a straitjacket.
 *
 * `if(cond, a, b)` is a TRUE SELECT: it returns one arm, it does not
 * weigh them.  The obvious arithmetic spelling, cond*a + (1-cond)*b, is
 * WRONG here and that is the whole point of the row — a NaN or an
 * infinity in the arm nobody chose would poison the answer, so
 * `if(0, sqrt(-1), 1)` has to be 1 and under the multiplication it is
 * NaN.  (Both arms are still evaluated: this language has no
 * short-circuit and 83.18 means evaluating one cannot fail anyway.)
 *
 * Truth is NON-ZERO, the way C reads it, and zero is false.  A NaN
 * condition answers NaN: the question itself had no answer, so neither
 * does the choice — and note that it takes writing `if(v, …)` with a
 * NaN v to get there, because a COMPARISON of a NaN is plain false. */
static gdouble
expr_if (const gdouble *a, gint n)
{
    (void) n;

    if (isnan (a[0]))
        return a[0];

    return (a[0] != 0.0) ? a[1] : a[2];
}

/* `lerp(a, b, t)` = a + (b - a) * t, the straight line between two
 * numbers.  Written in THAT spelling and not (1-t)*a + t*b, so that
 * lerp(a, b, 1) returns b EXACTLY rather than to within a rounding
 * error — which matters because the last frame of an animation is the
 * one a reader checks.  Unclamped on purpose: t outside [0, 1]
 * extrapolates along the same line, which is what an overshooting ease
 * wants. */
static gdouble
expr_lerp (const gdouble *a, gint n)
{
    (void) n;

    return a[0] + (a[1] - a[0]) * a[2];
}

/* `step(edge, x)` — 0 below the edge, 1 at or above it: the mask idiom
 * given a name.  It answers exactly what `x >= edge` answers, including
 * for a NaN x (false, so 0), because it IS that comparison under a name
 * and the two must not disagree. */
static gdouble
expr_step (gdouble edge, gdouble x)
{
    return (x >= edge) ? 1.0 : 0.0;
}

/* `smoothstep(lo, hi, x)` — step's eased twin: 0 below lo, 1 above hi,
 * and the classic 3t^2 - 2t^3 S-curve between, which leaves the ends
 * flat (its slope is 0 at both).
 *
 * Two edge cases decided rather than left to fall out:
 *  - EQUAL BOUNDS would be 0/0.  A ramp of zero width IS a step, so
 *    that is what it degenerates to, rather than a NaN.
 *  - CROSSED BOUNDS (hi < lo) reverse the ramp — 1 below lo, 0 above —
 *    which falls out of the division and is useful enough to keep and
 *    document rather than reject. */
static gdouble
expr_smoothstep (const gdouble *a, gint n)
{
    const gdouble lo = a[0], hi = a[1], x = a[2];
    gdouble       t;

    (void) n;

    if (hi == lo)
        return (x < lo) ? 0.0 : 1.0;

    t = (x - lo) / (hi - lo);
    t = (t < 0.0) ? 0.0 : (t > 1.0) ? 1.0 : t;

    return t * t * (3.0 - 2.0 * t);
}

/* THE CLASSIFICATION THREE (TODO #83.12).  C's isnan/isinf/isfinite are
 * MACROS, so there is no address to put in a row, and each needs the
 * one-line wrapper below anyway to answer in the language's own 1.0/0.0
 * rather than a C int.  They are small and they matter: 83.18 lets a NaN
 * travel, and without these a program has no way to ASK — `x != x` is
 * the C trick and nobody should have to know it.  `isinf` answers 1 for
 * an infinity of either sign; a program that cares which asks `sign`. */
static gdouble
expr_isnan (gdouble x)
{
    return isnan (x) ? 1.0 : 0.0;
}

static gdouble
expr_isinf (gdouble x)
{
    return isinf (x) ? 1.0 : 0.0;
}

static gdouble
expr_isfinite (gdouble x)
{
    return isfinite (x) ? 1.0 : 0.0;
}

/* `sinc(x)` = sin(x)/x, with the removable singularity REMOVED: sinc(0)
 * is 1, not the NaN that 0/0 gives (TODO #83.13a).  TODO #79.6 spends a
 * paragraph on this exact hole and offsets its sample grid by 1e-9 to
 * step around it; this row is the real fix, and a single slit's
 * diffraction envelope IS sinc.
 *
 * UNNORMALISED (sin(x)/x), not the signal-processing convention
 * sin(pi*x)/(pi*x).  Both are common, both are called sinc, and the help
 * says which this is — the physics sheets that asked for it want the
 * unnormalised one, whose first zero is at pi rather than at 1. */
static gdouble
expr_sinc (gdouble x)
{
    if (x == 0.0)
        return 1.0;

    return sin (x) / x;
}

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

/* THE INTEGER-MINDED THREE (TODO #83.14) — `factorial`, `gcd` and `lcm`,
 * the only rows in the table that can REFUSE an argument.
 *
 * This language has one numeric type and it is a double, so each of the
 * three has to say what it does with a fraction.  It refuses, which is
 * what abacus does and is more honest than truncating: the test 83.18
 * sets is whether the question has an answer this language can carry,
 * and "the greatest common divisor of 1.5 and 2" does not have one —
 * unlike "the square root of -1", which has NaN.  A NaN or an infinity
 * is not a whole number either, so the same check stops it; these three
 * are the one place a value that has been travelling down the wire since
 * 83.18 let it through is finally made to answer for itself.
 *
 * factorial also needs a CAP.  171! overflows a double to +inf, so 170
 * is the natural limit and anything above it is an error rather than a
 * silent infinity — the one refusal here that is about size rather than
 * kind, and it is a refusal because a silent +inf is exactly the kind of
 * answer that gets plotted. */
static gboolean
is_whole (gdouble x)
{
    return isfinite (x) && x == floor (x);
}

static const gchar *
check_factorial (const gdouble *a, gint n)
{
    (void) n;

    if (!is_whole (a[0]))
        return "takes a whole number";
    if (a[0] < 0.0)
        return "is not defined below 0";
    if (a[0] > 170.0)
        return "above 170 does not fit a double";

    return NULL;
}

/* gcd and lcm share one check because they share one requirement, and
 * it applies to BOTH arguments — a[0] alone would let gcd(4, 1.5)
 * through. */
static const gchar *
check_whole_pair (const gdouble *a, gint n)
{
    gint i;

    for (i = 0; i < n; i++)
        if (!is_whole (a[i]))
            return "takes whole numbers";

    return NULL;
}

/* Written out as the product rather than as tgamma(x + 1): glibc's
 * tgamma is accurate to within an ulp and that is not the same as
 * EXACT, so tgamma(13) is 479001599.99999994 where a reader who typed
 * factorial(12) expects 479001600 and will check.  The loop gives the
 * exact answer for every n whose factorial a double can hold exactly. */
static gdouble
expr_factorial (gdouble x)
{
    gdouble r = 1.0;
    gint    i, n = (gint) x;        /* the check guarantees 0..170 */

    for (i = 2; i <= n; i++)
        r *= (gdouble) i;

    return r;
}

/* Euclid, run on doubles with fmod — which is exact for whole numbers,
 * so this is integer arithmetic in all but the type.  The sign is
 * dropped (a divisor's sign is not information), gcd(0, 0) is 0, and
 * gcd(n, 0) is n, all of which fall out of the loop. */
static gdouble
expr_gcd (gdouble x, gdouble y)
{
    gdouble a = fabs (x), b = fabs (y);

    while (b > 0.0)
    {
        gdouble t = fmod (a, b);
        a = b;
        b = t;
    }

    return a;
}

/* lcm(a, b) = |a * b| / gcd(a, b), divided FIRST so the product cannot
 * overflow a case the answer itself would fit.  Any zero gives 0 —
 * there is no smallest common multiple of something and nothing, and 0
 * is the answer every other language settled on. */
static gdouble
expr_lcm (gdouble x, gdouble y)
{
    gdouble g = expr_gcd (x, y);

    if (g == 0.0)
        return 0.0;

    return fabs (x / g * y);
}

/* THE PERCENT FAMILY (TODO #83.15) — three rows of trivial arithmetic,
 * which is exactly the point.  They exist so that nobody hand-rolls
 * `/ 100` in a sheet and nobody writes a basis point where they meant a
 * percent: a Tasmota power reading scaled by the wrong factor of a
 * hundred looks plausible all the way to the graph.
 *
 * `pct_change` has the one trap worth stating twice: its arguments are
 * OLD then NEW, and getting them backwards gives an answer that is
 * wrong without looking wrong.  It divides by zero when `old` is 0, and
 * under 83.18 that is an infinity travelling on rather than an error. */
static gdouble
expr_pct (gdouble x, gdouble p)
{
    return x * p / 100.0;
}

static gdouble
expr_pct_change (gdouble old_value, gdouble new_value)
{
    return (new_value - old_value) / old_value;
}

static gdouble
expr_bps (gdouble x, gdouble b)
{
    return x * b / 10000.0;
}

/* THE ANNUITY FAMILY (TODO #83.16) — `compound`, `fv`, `pv` and `pmt`,
 * four three-argument rows taken from abacus and unreachable before the
 * argument chain.  A worksheet that reads an energy meter is one
 * expression away from reading a bill, and each of these is a formula
 * people get wrong from memory.
 *
 * TWO THINGS THAT MUST BE SAID, and the help says them too:
 *  - THE RATE IS PER PERIOD and `periods`/`nper` counts the SAME unit.
 *    A yearly 5% over 24 months is compound(p, 0.05 / 12, 24), not
 *    compound(p, 0.05, 24); nothing here can catch that mistake.
 *  - r = 0 IS A LIMIT, NOT A DIVISION.  Every one of the three annuity
 *    rows has r in a denominator and a limit that is perfectly
 *    well-behaved as r approaches zero — a stream of payments with no
 *    interest is worth pmt * nper — so each special-cases it rather
 *    than returning the inf the formula would give.  That is the one
 *    place these rows depart from writing the textbook formula out. */
static gdouble
expr_compound (const gdouble *a, gint n)
{
    const gdouble principal = a[0], rate = a[1], periods = a[2];

    (void) n;                       /* fixed arity 3 */

    return principal * pow (1.0 + rate, periods);
}

static gdouble
expr_fv (const gdouble *a, gint n)
{
    const gdouble pmt = a[0], rate = a[1], nper = a[2];

    (void) n;

    if (rate == 0.0)
        return pmt * nper;

    return pmt * (pow (1.0 + rate, nper) - 1.0) / rate;
}

static gdouble
expr_pv (const gdouble *a, gint n)
{
    const gdouble pmt = a[0], rate = a[1], nper = a[2];

    (void) n;

    if (rate == 0.0)
        return pmt * nper;

    return pmt * (1.0 - pow (1.0 + rate, -nper)) / rate;
}

static gdouble
expr_pmt (const gdouble *a, gint n)
{
    const gdouble present = a[0], rate = a[1], nper = a[2];

    (void) n;

    if (rate == 0.0)
        return present / nper;

    return present * rate / (1.0 - pow (1.0 + rate, -nper));
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
 * The macros keep a row to one line whatever the struct grows:
 * PN_EXPR_FN1/FN2 for a fixed one or two arguments, PN_EXPR_FNN for a
 * fixed three or more, PN_EXPR_FNR for an arity RANGE (TODO #83.2), and
 * the FN1C/FN2C pair for the three rows that carry an argument CHECK
 * (TODO #83.14).  `clamp` and `log(x[, base])` are the specimens that
 * exercise the chain, the range and the N-operand broadcast end to end
 * — the way `atan2` was #81's specimen for the comma.
 *
 * THE NaN AND DOMAIN POLICY, settled once for the whole table rather
 * than row by row (TODO #83.18), because this is where most rows first
 * have a domain — acosh below 1, atanh outside (-1, 1), log at and
 * below 0, the reciprocal trig poles, sqrt of a negative:
 *
 *   A result mathematics does not define is a VALUE, not an error.
 *   sqrt(-1), acosh(0) and atanh(2) are NaN; log(0), cot(0) and
 *   csc(0) are infinite; and each travels down the wire like any
 *   other number, exactly as `1 / 0` already did before this entry.
 *
 * THE ONE EXCEPTION, and it is about the ARGUMENT rather than the
 * result: three rows carry a CHECK (see PnExprCheckFn) and refuse an
 * argument wrong in KIND rather than out of range — factorial(-1),
 * factorial(1.5), gcd(1.5, 2) (83.14).  The test is whether the
 * question has an answer this language can carry: "the square root of
 * -1" has one, and NaN is how a double says it; "the factorial of a
 * half" has none, because that is a different function.  A program
 * tests for the first kind with isnan/isfinite (83.12) and is stopped
 * by the second. */
static const PnExprFunc builtin_funcs[] = {
    PN_EXPR_FN1 ("sin",   sin),
    PN_EXPR_FN1 ("cos",   cos),
    PN_EXPR_FN1 ("tan",   tan),
    PN_EXPR_FN1 ("asin",  asin),
    PN_EXPR_FN1 ("acos",  acos),
    PN_EXPR_FN1 ("atan",  atan),
    PN_EXPR_FN1 ("cot",   expr_cot),
    PN_EXPR_FN1 ("sec",   expr_sec),
    PN_EXPR_FN1 ("csc",   expr_csc),
    PN_EXPR_FN1 ("degrees", expr_degrees),
    PN_EXPR_FN1 ("radians", expr_radians),
    PN_EXPR_FN1 ("sinh",  sinh),
    PN_EXPR_FN1 ("cosh",  cosh),
    PN_EXPR_FN1 ("tanh",  tanh),
    PN_EXPR_FN1 ("asinh", asinh),
    PN_EXPR_FN1 ("acosh", acosh),
    PN_EXPR_FN1 ("atanh", atanh),
    PN_EXPR_FNR ("log",   1, 2, expr_log),
    PN_EXPR_FN1 ("ln",    log),
    PN_EXPR_FN1 ("log10", log10),
    PN_EXPR_FN1 ("log2",  log2),
    PN_EXPR_FN1 ("log1p", log1p),
    PN_EXPR_FN1 ("exp",   exp),
    PN_EXPR_FN1 ("exp2",  exp2),
    PN_EXPR_FN1 ("expm1", expm1),
    PN_EXPR_FN1 ("sqrt",  sqrt),
    PN_EXPR_FN1 ("cbrt",  cbrt),
    PN_EXPR_FN1 ("abs",   fabs),
    PN_EXPR_FN1 ("floor", floor),
    PN_EXPR_FN1 ("ceil",  ceil),
    PN_EXPR_FN1 ("round", round),
    PN_EXPR_FN1 ("trunc", trunc),
    PN_EXPR_FN1 ("sign",  expr_sign),
    PN_EXPR_FN1 ("isnan", expr_isnan),
    PN_EXPR_FN1 ("isinf", expr_isinf),
    PN_EXPR_FN1 ("isfinite", expr_isfinite),
    PN_EXPR_FN1 ("sinc",  expr_sinc),
    PN_EXPR_FN1 ("erf",   erf),
    PN_EXPR_FN1 ("erfc",  erfc),
    PN_EXPR_FN1 ("j0",    j0),
    PN_EXPR_FN1 ("j1",    j1),
    PN_EXPR_FN2 ("atan2", atan2),
    PN_EXPR_FN2 ("min",   fmin),
    PN_EXPR_FN2 ("max",   fmax),
    PN_EXPR_FN2 ("pow",   pow),
    PN_EXPR_FN2 ("hypot", hypot),
    PN_EXPR_FN2 ("fmod",  fmod),
    PN_EXPR_FN2 ("copysign", copysign),
    PN_EXPR_FN1C ("factorial", expr_factorial, check_factorial),
    PN_EXPR_FN2C ("gcd",  expr_gcd, check_whole_pair),
    PN_EXPR_FN2C ("lcm",  expr_lcm, check_whole_pair),
    PN_EXPR_FN2 ("pct",   expr_pct),
    PN_EXPR_FN2 ("pct_change", expr_pct_change),
    PN_EXPR_FN2 ("bps",   expr_bps),
    PN_EXPR_FNN ("clamp", 3, expr_clamp),
    PN_EXPR_FNN ("if",    3, expr_if),
    PN_EXPR_FNN ("lerp",  3, expr_lerp),
    PN_EXPR_FN2 ("step",  expr_step),
    PN_EXPR_FNN ("smoothstep", 3, expr_smoothstep),
    PN_EXPR_FNN ("compound", 3, expr_compound),
    PN_EXPR_FNN ("fv",    3, expr_fv),
    PN_EXPR_FNN ("pv",    3, expr_pv),
    PN_EXPR_FNN ("pmt",   3, expr_pmt),
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
