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

#include "pn-var-store.h"

#include "pn-expr-funcs.h"

#include <math.h>

struct _PnVarStore
{
    GObject     parent_instance;

    /* name (gchar*, owned) -> value (gdouble*, owned) */
    GHashTable *vars;

    /* Names bound by an in-expression assignment during the current
     * evaluation (a subset of @vars), so a caller can tell program
     * outputs apart from the inputs it pre-bound.  Same ownership and
     * lifetime as @vars; cleared together by pn_var_store_clear(). */
    GHashTable *assigned;
};

G_DEFINE_TYPE (PnVarStore, pn_var_store, G_TYPE_OBJECT)

G_DEFINE_QUARK (pn-var-store-error, pn_var_store_error)

/* The built-in functions and the constants used to be a private table
 * here.  Both now live in pn-expr-funcs.c and this file only dispatches
 * through it: the PARSER needs each function's arity to reject a
 * wrong-count call as it is typed, and the evaluator needs the
 * implementation, so one table serves both halves and there is exactly
 * one place to add the next name (TODO #81.4). */

/* ------------------------------------------------------------------ */
/*  Values (scalar or vector) — TODO #43.7                             */
/*                                                                     */
/*  The evaluator and the symbol table both deal in #PnExprValue: a    */
/*  plain double, or a reference to an immutable #PnVector treated as a */
/*  bag of numbers.  A vector is never mutated in place (it may be      */
/*  shared across a message fan-out); every transform allocates a fresh */
/*  buffer.                                                             */
/* ------------------------------------------------------------------ */

void
pn_expr_value_clear (PnExprValue *value)
{
    if (value == NULL)
        return;
    g_clear_object (&value->vec);
    value->scalar = 0.0;
}

/* Adopt a freshly-malloc'd buffer of @len doubles as @out's vector.  An
 * empty result (@len == 0, @buf == %NULL) normalises to an empty vector. */
static void
value_take_buffer (PnExprValue *out, gdouble *buf, gsize len)
{
    out->vec    = pn_vector_new_take (buf, len);
    out->scalar = 0.0;
}

/* Box helpers for the symbol table: each binding is a heap #PnExprValue. */

static PnExprValue *
value_box_copy (const PnExprValue *v)
{
    PnExprValue *b = g_new0 (PnExprValue, 1);
    if (v->vec != NULL)
        b->vec = g_object_ref (v->vec);
    else
        b->scalar = v->scalar;
    return b;
}

static void
value_box_free (gpointer p)
{
    PnExprValue *b = p;
    if (b == NULL)
        return;
    g_clear_object (&b->vec);
    g_free (b);
}

/* Per-element scalar kernels. */

/* The bitwise operators work on 64-bit integers: an operand is truncated
 * toward zero, like a C cast.  One that is not finite or does not fit an
 * int64 has no integer reading, so it makes the result NaN — the same
 * "no error, just a non-number" contract '/' has for a zero divisor. */
#define PN_INT64_LIMIT 9223372036854775808.0   /* 2^63 */

static gboolean
to_int64 (gdouble x, gint64 *out)
{
    if (!isfinite (x))
        return FALSE;
    x = trunc (x);
    if (x < -PN_INT64_LIMIT || x >= PN_INT64_LIMIT)
        return FALSE;
    *out = (gint64) x;
    return TRUE;
}

static gdouble
apply_bitwise (gchar op, gdouble x, gdouble y)
{
    gint64 a, b;

    if (!to_int64 (x, &a) || !to_int64 (y, &b))
        return NAN;

    switch (op)
    {
    case '&': return (gdouble) (a & b);
    case '|': return (gdouble) (a | b);
    case '^': return (gdouble) (a ^ b);
    case 'l':
        /* Shift through unsigned so a bit shifted into (or past) the
         * sign position wraps instead of being undefined behaviour. */
        if (b < 0 || b > 63)
            return NAN;
        return (gdouble) (gint64) ((guint64) a << b);
    default:  /* 'r' — arithmetic: a negative value stays negative */
        if (b < 0 || b > 63)
            return NAN;
        return (gdouble) (a >> b);
    }
}

static gdouble
apply_arith (gchar op, gdouble x, gdouble y)
{
    switch (op)
    {
    case '+': return x + y;
    case '-': return x - y;
    case '*': return x * y;
    case '%':
        {
            /* Floored modulo: the result takes the divisor's sign, so
             * `-1 % 8` is 7 — what an address wrap wants (PnCounter's
             * modulo wraps the same way).  y == 0 gives NaN. */
            gdouble r = fmod (x, y);
            if (r != 0.0 && ((r < 0.0) != (y < 0.0)))
                r += y;
            return r;
        }
    case '&': case '|': case '^': case 'l': case 'r':
        return apply_bitwise (op, x, y);
    default:  return x / y;   /* '/' — IEEE inf/nan on divide-by-zero */
    }
}

/* ~x: bitwise complement of the truncated int64 (NaN when x has none). */
static gdouble
bit_not (gdouble x)
{
    gint64 a;
    return to_int64 (x, &a) ? (gdouble) ~a : NAN;
}

static gboolean
apply_cmp (gchar op, gdouble x, gdouble y)
{
    switch (op)
    {
    case '<': return x <  y;
    case '>': return x >  y;
    case 'L': return x <= y;
    case 'G': return x >= y;
    case '=': return x == y;
    default:  return x != y;  /* '!' */
    }
}

static gboolean
is_cmp_op (gchar op)
{
    return op == '<' || op == '>' || op == 'L'
        || op == 'G' || op == '=' || op == '!';
}

/* out = -a  (negate a scalar, or every element of a vector). */
static void
negate_value (const PnExprValue *a, PnExprValue *out)
{
    if (a->vec != NULL)
    {
        gsize          n = pn_vector_get_len  (a->vec);
        const gdouble *d = pn_vector_get_data (a->vec);
        gdouble       *r = g_new (gdouble, n);
        gsize          i;
        for (i = 0; i < n; i++)
            r[i] = -d[i];
        value_take_buffer (out, r, n);
    }
    else
    {
        out->scalar = -a->scalar;
    }
}

/* out = fn(a)  (apply a unary math function to a scalar, or map it over
 * every element of a vector).  A one-operand zip, so unary `~` and
 * `sin()` share the broadcast with everything else. */
static void
map_value (PnExprUnaryFn fn, const PnExprValue *a, PnExprValue *out);

/* The per-element rule of a combination of N operands: either one of the
 * language's binary operators (@op, run through apply_arith) or a
 * built-in function in whichever of the three shapes its row declares.
 * Exactly one is set.  Carrying them all through one struct is what lets
 * zipn_value() below serve `a * b`, `sin(a)`, `atan2(a, b)` and
 * `clamp(a, b, c)` from the same code, which is the whole of TODO #81.5
 * generalised by 83.19 — a function that broadcast differently from `*`
 * would be a second rule for one idea. */
typedef struct
{
    gchar          op;   /* operator code, when every fn is NULL */
    PnExprUnaryFn  fn1;  /* one-argument built-in                */
    PnExprBinaryFn fn2;  /* two-argument built-in                */
    PnExprNaryFn   fnN;  /* three or more, or a ranged arity     */
} Kernel;

static gdouble
kernel_apply (const Kernel *k, const gdouble *a, gint n)
{
    if (k->fnN != NULL)
        return k->fnN (a, n);
    if (k->fn2 != NULL)
        return k->fn2 (a[0], a[1]);
    if (k->fn1 != NULL)
        return k->fn1 (a[0]);

    return apply_arith (k->op, a[0], a[1]);
}

/* out = k(v[0], …, v[n-1]).  Scalars broadcast over vectors; vectors go
 * elementwise; on a length mismatch the result takes the LONGEST length
 * and, at an index where some operand has run out, the kernel is not
 * applied at all — the FIRST operand that still has an element passes
 * through verbatim.
 *
 * At n == 2 that is exactly the rule 43.7 pinned for `*`, because only
 * one operand can survive there: `[2,3] * [3,4,5]` = `[6,12,5]`.  At
 * three and four it is the reading that keeps the same promise — for
 * every function in this language the first argument is the VALUE and
 * the rest are parameters, so a bound vector running out leaves the
 * value passing through untouched rather than clamped against nothing.
 *
 * Generalising THIS function rather than writing a second one beside it
 * is the structural part (83.19): arithmetic, unary maps and calls of
 * every arity share one broadcast, so a function CANNOT drift from an
 * operator. */
static void
zipn_value (const Kernel      *k,
            const PnExprValue *v,
            gint               n,
            PnExprValue       *out)
{
    const gdouble *data[PN_EXPR_MAX_ARITY];
    gsize          lens[PN_EXPR_MAX_ARITY];
    gdouble        args[PN_EXPR_MAX_ARITY];
    gsize          len = 0;
    gdouble       *r;
    gint           i;
    gsize          j;

    g_assert (n >= 1 && n <= PN_EXPR_MAX_ARITY);

    for (i = 0; i < n; i++)
    {
        data[i] = v[i].vec != NULL ? pn_vector_get_data (v[i].vec) : NULL;
        lens[i] = v[i].vec != NULL ? pn_vector_get_len  (v[i].vec) : 0;
        len     = MAX (len, lens[i]);
    }

    if (len == 0)                       /* every operand a scalar */
    {
        for (i = 0; i < n; i++)
            args[i] = v[i].scalar;
        out->scalar = kernel_apply (k, args, n);
        return;
    }

    r = g_new (gdouble, len);
    for (j = 0; j < len; j++)
    {
        gboolean complete   = TRUE;
        gboolean have_first = FALSE;
        gdouble  first      = 0.0;

        for (i = 0; i < n; i++)
        {
            if (data[i] == NULL)            /* a scalar never runs out */
                args[i] = v[i].scalar;
            else if (j < lens[i])
                args[i] = data[i][j];
            else                            /* this operand has run out */
            {
                complete = FALSE;
                continue;
            }

            if (!have_first)
            {
                first      = args[i];
                have_first = TRUE;
            }
        }

        r[j] = complete ? kernel_apply (k, args, n) : first;
    }

    value_take_buffer (out, r, len);
}

/* out = a OP b for an arithmetic or bitwise operator
 * (+ - * / % & | ^ << >>). */
static void
arith_value (gchar op, const PnExprValue *a, const PnExprValue *b,
             PnExprValue *out)
{
    Kernel      k = { op, NULL, NULL, NULL };
    PnExprValue v[2];

    v[0] = *a;
    v[1] = *b;
    zipn_value (&k, v, 2, out);
}

static void
map_value (PnExprUnaryFn fn, const PnExprValue *a, PnExprValue *out)
{
    Kernel k = { '\0', fn, NULL, NULL };

    zipn_value (&k, a, 1, out);
}

/* out = a CMP b for a comparison operator.  ALWAYS reduces to a scalar
 * 0.0/1.0: true iff every compared element passes (all()-semantics).  A
 * scalar broadcasts over a vector; for two vectors of unequal length the
 * surplus tail has no counterpart and is vacuously true. */
static void
compare_value (gchar op, const PnExprValue *a, const PnExprValue *b,
               PnExprValue *out)
{
    const gdouble *ad = a->vec ? pn_vector_get_data (a->vec) : NULL;
    const gdouble *bd = b->vec ? pn_vector_get_data (b->vec) : NULL;
    gsize          la = a->vec ? pn_vector_get_len  (a->vec) : 0;
    gsize          lb = b->vec ? pn_vector_get_len  (b->vec) : 0;
    gboolean       all_true = TRUE;
    gsize          i;

    if (a->vec == NULL && b->vec == NULL)
    {
        out->scalar = apply_cmp (op, a->scalar, b->scalar) ? 1.0 : 0.0;
        return;
    }

    if (a->vec != NULL && b->vec != NULL)
    {
        gsize n = MIN (la, lb);             /* tail beyond n is vacuously true */
        for (i = 0; i < n && all_true; i++)
            all_true = apply_cmp (op, ad[i], bd[i]);
    }
    else if (a->vec != NULL)               /* vector CMP scalar */
    {
        for (i = 0; i < la && all_true; i++)
            all_true = apply_cmp (op, ad[i], b->scalar);
    }
    else                                   /* scalar CMP vector */
    {
        for (i = 0; i < lb && all_true; i++)
            all_true = apply_cmp (op, a->scalar, bd[i]);
    }

    out->scalar = all_true ? 1.0 : 0.0;
}

gchar *
pn_var_store_value_to_string (const PnExprValue *value)
{
    if (value == NULL)
        return g_strdup ("");
    if (value->vec == NULL)
        return g_strdup_printf ("%g", value->scalar);

    /* Shared bounded renderer so the Calculator's "output" and the Debug
     * pane preview a vector identically ("[a, b, …] (N values)"). */
    return pn_vector_to_sample_string (value->vec, 8);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_var_store_finalize (GObject *object)
{
    PnVarStore *self = PN_VAR_STORE (object);

    g_clear_pointer (&self->vars,     g_hash_table_unref);
    g_clear_pointer (&self->assigned, g_hash_table_unref);

    G_OBJECT_CLASS (pn_var_store_parent_class)->finalize (object);
}

static void
pn_var_store_class_init (PnVarStoreClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = pn_var_store_finalize;
}

static void
pn_var_store_init (PnVarStore *self)
{
    self->vars = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, value_box_free);
    self->assigned = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, value_box_free);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnVarStore *
pn_var_store_new (void)
{
    return g_object_new (PN_TYPE_VAR_STORE, NULL);
}

/* Insert a copy of @v (refs a vector) under @name into @table. */
static void
store_put (GHashTable *table, const gchar *name, const PnExprValue *v)
{
    g_hash_table_insert (table, g_strdup (name), value_box_copy (v));
}

void
pn_var_store_set (PnVarStore  *self,
                  const gchar *name,
                  gdouble      value)
{
    PnExprValue v = { NULL, value };

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (name != NULL);

    store_put (self->vars, name, &v);
}

void
pn_var_store_set_vector (PnVarStore  *self,
                         const gchar *name,
                         PnVector    *vec)
{
    PnExprValue v = { vec, 0.0 };

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (name != NULL);
    g_return_if_fail (PN_IS_VECTOR (vec));

    store_put (self->vars, name, &v);
}

/* Internal: bind @value (scalar or vector) and record it as an
 * assignment so a caller can surface program outputs. */
static void
var_store_assign_value (PnVarStore        *self,
                        const gchar       *name,
                        const PnExprValue *value)
{
    store_put (self->vars,     name, value);
    store_put (self->assigned, name, value);
}

void
pn_var_store_assign (PnVarStore  *self,
                     const gchar *name,
                     gdouble      value)
{
    PnExprValue v = { NULL, value };

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (name != NULL);

    var_store_assign_value (self, name, &v);
}

void
pn_var_store_foreach_assignment (PnVarStore            *self,
                                 PnVarStoreForeachFunc  func,
                                 gpointer               user_data)
{
    GHashTableIter  iter;
    gpointer        k, v;

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (func != NULL);

    g_hash_table_iter_init (&iter, self->assigned);
    while (g_hash_table_iter_next (&iter, &k, &v))
    {
        const PnExprValue *box = v;
        /* The scalar API cannot carry a vector; report it as 0.0. */
        func ((const gchar *) k, box->vec ? 0.0 : box->scalar, user_data);
    }
}

void
pn_var_store_foreach_assignment_value (PnVarStore                 *self,
                                       PnVarStoreForeachValueFunc  func,
                                       gpointer                    user_data)
{
    GHashTableIter  iter;
    gpointer        k, v;

    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_return_if_fail (func != NULL);

    g_hash_table_iter_init (&iter, self->assigned);
    while (g_hash_table_iter_next (&iter, &k, &v))
        func ((const gchar *) k, (const PnExprValue *) v, user_data);
}

gboolean
pn_var_store_get (PnVarStore  *self,
                  const gchar *name,
                  gdouble     *out_value)
{
    PnExprValue *box;

    g_return_val_if_fail (PN_IS_VAR_STORE (self), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);

    box = g_hash_table_lookup (self->vars, name);
    if (box == NULL || box->vec != NULL)   /* unset, or a vector (not scalar) */
        return FALSE;

    if (out_value != NULL)
        *out_value = box->scalar;
    return TRUE;
}

void
pn_var_store_clear (PnVarStore *self)
{
    g_return_if_fail (PN_IS_VAR_STORE (self));
    g_hash_table_remove_all (self->vars);
    g_hash_table_remove_all (self->assigned);
}

/* Recursive value-aware evaluator.  @out is caller-allocated; on success
 * it owns the result (a vector reference if @out->vec is set) and the
 * caller releases it with pn_expr_value_clear().  On failure @out is left
 * cleared and nothing leaks. */
static gboolean
eval_value (PnVarStore       *self,
            const PnExprNode *node,
            PnExprValue      *out,
            GError          **error)
{
    out->vec    = NULL;
    out->scalar = 0.0;

    if (node == NULL)
    {
        g_set_error_literal (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_BAD_AST,
                             "empty expression tree");
        return FALSE;
    }

    switch (node->type)
    {
    case PN_EXPR_NODE_NUMBER:
        out->scalar = node->number;
        return TRUE;

    case PN_EXPR_NODE_VARIABLE:
        {
            PnExprValue *box = g_hash_table_lookup (self->vars, node->name);
            if (box == NULL)
            {
                /* The language's constants resolve here, AFTER the
                 * bindings and only as a fallback (TODO #81.6).  They are
                 * deliberately not pre-bound: pn_var_store_clear() drops
                 * every binding, so a pre-bound `pi` would evaporate on
                 * the next clear and come back only if every caller
                 * remembered to re-add it.  As a fallback it cannot be
                 * lost — and a data-bag member actually called `pi` still
                 * shadows it, which is the conservative way round. */
                if (pn_expr_constant_lookup (node->name, &out->scalar))
                    return TRUE;

                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_UNKNOWN_VARIABLE,
                             "unknown variable '%s'", node->name);
                return FALSE;
            }
            if (box->vec != NULL)
                out->vec = g_object_ref (box->vec);
            else
                out->scalar = box->scalar;
            return TRUE;
        }

    case PN_EXPR_NODE_UNARY:
        {
            PnExprValue a = { NULL, 0.0 };
            if (!eval_value (self, node->left, &a, error))
                return FALSE;
            if (node->op == '-')
                negate_value (&a, out);
            else if (node->op == '~')
                map_value (bit_not, &a, out);
            else                            /* unary '+': pass the value */
                { out->vec = a.vec; out->scalar = a.scalar; a.vec = NULL; }
            pn_expr_value_clear (&a);
            return TRUE;
        }

    case PN_EXPR_NODE_BINARY:
        {
            PnExprValue a = { NULL, 0.0 }, b = { NULL, 0.0 };
            gboolean    ok = TRUE;

            if (!eval_value (self, node->left, &a, error))
                return FALSE;
            if (!eval_value (self, node->right, &b, error))
            {
                pn_expr_value_clear (&a);
                return FALSE;
            }

            switch (node->op)
            {
            case '+': case '-': case '*': case '/': case '%':
            case '&': case '|': case '^': case 'l': case 'r':
                arith_value (node->op, &a, &b, out);
                break;
            case '<': case '>': case 'L':
            case 'G': case '=': case '!':
                compare_value (node->op, &a, &b, out);
                break;
            default:
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_BAD_AST,
                             "unknown operator '%c'", node->op);
                ok = FALSE;
                break;
            }

            pn_expr_value_clear (&a);
            pn_expr_value_clear (&b);
            return ok;
        }

    case PN_EXPR_NODE_CALL:
        {
            const PnExprNode *arg_nodes[PN_EXPR_MAX_ARITY];
            PnExprValue       args[PN_EXPR_MAX_ARITY];
            const PnExprNode *chain;
            const PnExprFunc *fn = pn_expr_func_lookup (node->name);
            Kernel            k;
            gint              n_args = 0;
            gint              i;
            gboolean          ok = TRUE;

            if (fn == NULL)
            {
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_UNKNOWN_FUNCTION,
                             "unknown function '%s'", node->name);
                return FALSE;
            }

            /* Argument one is .left; two onwards hang off .right as a
             * chain of ARG nodes (TODO #83.1).  Collect the nodes first
             * so the count is known before anything is evaluated. */
            if (node->left != NULL)
                arg_nodes[n_args++] = node->left;

            for (chain = node->right; chain != NULL; chain = chain->right)
            {
                if (chain->type != PN_EXPR_NODE_ARG ||
                    chain->left == NULL ||
                    n_args == PN_EXPR_MAX_ARITY)
                {
                    g_set_error (error, PN_VAR_STORE_ERROR,
                                 PN_VAR_STORE_ERROR_BAD_AST,
                                 "malformed argument list for '%s'",
                                 node->name);
                    return FALSE;
                }
                arg_nodes[n_args++] = chain->left;
            }

            /* The parser already rejected a wrong argument count against
             * the same table, so a mismatch here means a hand-built tree
             * rather than a typed program — a bad AST, not a user error.
             * The phrase comes from the table either way, so the two
             * messages cannot drift (TODO #83.2). */
            if (n_args < fn->min_arity || n_args > fn->max_arity)
            {
                gchar *phrase = pn_expr_func_arity_phrase (fn);
                g_set_error (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_BAD_AST,
                             "'%s' takes %s", node->name, phrase);
                g_free (phrase);
                return FALSE;
            }

            for (i = 0; i < n_args; i++)
            {
                args[i].vec    = NULL;
                args[i].scalar = 0.0;
            }

            for (i = 0; i < n_args && ok; i++)
                ok = eval_value (self, arg_nodes[i], &args[i], error);

            if (ok)
            {
                k.op  = '\0';
                k.fn1 = fn->fn1;
                k.fn2 = fn->fn2;
                k.fnN = fn->fnN;
                zipn_value (&k, args, n_args, out);
            }

            for (i = 0; i < n_args; i++)
                pn_expr_value_clear (&args[i]);

            return ok;
        }

    case PN_EXPR_NODE_ASSIGN:
        {
            PnExprValue v = { NULL, 0.0 };
            if (!eval_value (self, node->left, &v, error))
                return FALSE;
            /* Bind the name (for later statements) and record it as an
             * assignment; an assignment's own value is the value bound.
             * var_store_assign_value() takes its own reference, so the
             * value computed here transfers straight into @out. */
            var_store_assign_value (self, node->name, &v);
            out->vec = v.vec; out->scalar = v.scalar;
            return TRUE;
        }

    case PN_EXPR_NODE_SEQ:
        {
            PnExprValue discard = { NULL, 0.0 };
            /* Evaluate the statement for its effect (typically a binding)
             * and discard its value; the sequence's value is the rest. */
            if (!eval_value (self, node->left, &discard, error))
                return FALSE;
            pn_expr_value_clear (&discard);
            return eval_value (self, node->right, out, error);
        }

    default:
        g_set_error (error, PN_VAR_STORE_ERROR,
                     PN_VAR_STORE_ERROR_BAD_AST,
                     "unknown AST node type %d", (gint) node->type);
        return FALSE;
    }
}

gboolean
pn_var_store_evaluate_value (PnVarStore       *self,
                             const PnExprNode *node,
                             PnExprValue      *out_value,
                             GError          **error)
{
    g_return_val_if_fail (PN_IS_VAR_STORE (self), FALSE);
    g_return_val_if_fail (out_value != NULL, FALSE);

    out_value->vec    = NULL;
    out_value->scalar = 0.0;
    return eval_value (self, node, out_value, error);
}

gboolean
pn_var_store_evaluate (PnVarStore       *self,
                       const PnExprNode *node,
                       gdouble          *out_value,
                       GError          **error)
{
    PnExprValue v = { NULL, 0.0 };

    g_return_val_if_fail (out_value != NULL, FALSE);

    if (!pn_var_store_evaluate_value (self, node, &v, error))
        return FALSE;

    if (v.vec != NULL)
    {
        /* A scalar-only caller cannot represent a vector result; fail
         * loudly rather than silently collapsing it. */
        g_set_error_literal (error, PN_VAR_STORE_ERROR,
                             PN_VAR_STORE_ERROR_TYPE_MISMATCH,
                             "expression produced a vector where a "
                             "scalar was required");
        pn_expr_value_clear (&v);
        return FALSE;
    }

    *out_value = v.scalar;
    return TRUE;
}
