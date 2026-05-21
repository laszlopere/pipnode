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

#include "pn-auto-random.h"
#include "pn-message.h"

#include <math.h>

/* fa-random U+F074 — the dice-and-arrows glyph, distinct from the
 * paper-plane shared by #PnInject / #PnAutoInjector so the "random
 * sibling" relationship is visible at a glance without confusing the
 * two on-canvas. */
#define PN_AUTO_RANDOM_ICON           "\xef\x81\xb4"

#define PN_AUTO_RANDOM_DEFAULT_PERIOD  1u
#define PN_AUTO_RANDOM_DEFAULT_OUTPUT  "AutoRandom sample."
#define PN_AUTO_RANDOM_DEFAULT_MIN     0.0
#define PN_AUTO_RANDOM_DEFAULT_MAX     1.0

struct _PnAutoRandom
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads these on every tick while the main
     * thread may overwrite them via property setters; @mutex
     * serialises the two so a half-applied property change cannot
     * leak into an in-flight message. */
    GMutex                    mutex;
    gchar                    *output;        /* data.output payload */
    gdouble                   min;           /* lower bound, inclusive */
    gdouble                   max;           /* upper bound, inclusive */
    PnAutoRandomDistribution  distribution;  /* shape of the draw */
    gboolean                  success;       /* data.success flag */

    /* The RNG itself: a per-instance #GRand seeded with time +
     * address bits so two AutoRandom nodes dropped on the same
     * worksheet do not lock-step.  Touched only from the worker
     * thread (the trigger callback is serialised by the auto-trigger
     * base class), so it lives outside @mutex. */
    GRand                    *rng;
};

G_DEFINE_TYPE (PnAutoRandom, pn_auto_random, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_OUTPUT,
    PROP_MIN,
    PROP_MAX,
    PROP_DISTRIBUTION,
    PROP_SUCCESS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Distribution enum boxed type                                       */
/* ------------------------------------------------------------------ */

GType
pn_auto_random_distribution_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_AUTO_RANDOM_UNIFORM,
              "PN_AUTO_RANDOM_UNIFORM",     "uniform"     },
            { PN_AUTO_RANDOM_NORMAL,
              "PN_AUTO_RANDOM_NORMAL",      "normal"      },
            { PN_AUTO_RANDOM_TRIANGULAR,
              "PN_AUTO_RANDOM_TRIANGULAR",  "triangular"  },
            { PN_AUTO_RANDOM_EXPONENTIAL,
              "PN_AUTO_RANDOM_EXPONENTIAL", "exponential" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnAutoRandomDistribution",
                                             values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Sampling                                                           */
/*                                                                     */
/*  Each sampler maps a uniform draw on [0, 1) to the desired shape    */
/*  on [min, max].  The Box-Muller and inverse-CDF formulas are the    */
/*  textbook ones — they need no explanation, but the *parameter      */
/*  choices* below are conventions specific to this node and worth    */
/*  documenting:                                                       */
/*                                                                     */
/*    Normal      mean = midpoint, stddev = range / 6.  Three sigma    */
/*                covers half the range on each side, so ~99.7% of    */
/*                draws fall inside [min, max] before clamping; the   */
/*                clamp catches the tails that escape rather than    */
/*                changing the bulk shape.                            */
/*    Triangular  symmetric, apex at the midpoint.  The pdf is zero   */
/*                at both bounds, so the sampler is naturally bounded */
/*                and the clamp at the end is a no-op except for     */
/*                rounding noise.                                      */
/*    Exponential lambda = 3 / range, shifted by min.  Mean lands at  */
/*                min + range/3; ~95% of mass falls before max, the   */
/*                remaining tail is clamped at max.                    */
/* ------------------------------------------------------------------ */

static gdouble
clamp (gdouble x, gdouble lo, gdouble hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static gdouble
sample_uniform (GRand *rng, gdouble lo, gdouble hi)
{
    return g_rand_double_range (rng, lo, hi);
}

static gdouble
sample_normal (GRand *rng, gdouble lo, gdouble hi)
{
    /* Box-Muller.  Reject u1 == 0 so log(0) never happens. */
    gdouble u1, u2, z;
    gdouble mean   = 0.5  * (lo + hi);
    gdouble stddev = (hi - lo) / 6.0;

    do
        u1 = g_rand_double (rng);
    while (u1 <= 0.0);

    u2 = g_rand_double (rng);
    z  = sqrt (-2.0 * log (u1)) * cos (2.0 * G_PI * u2);

    return clamp (mean + z * stddev, lo, hi);
}

static gdouble
sample_triangular (GRand *rng, gdouble lo, gdouble hi)
{
    /* Symmetric triangular (mode = midpoint) via inverse-CDF. */
    gdouble u    = g_rand_double (rng);
    gdouble span = hi - lo;
    gdouble x;

    if (span <= 0.0)
        return lo;

    if (u < 0.5)
        x = lo + sqrt (u * 0.5)         * span;
    else
        x = hi - sqrt ((1.0 - u) * 0.5) * span;

    return clamp (x, lo, hi);
}

static gdouble
sample_exponential (GRand *rng, gdouble lo, gdouble hi)
{
    gdouble span = hi - lo;
    gdouble u, x;

    if (span <= 0.0)
        return lo;

    do
        u = g_rand_double (rng);
    while (u >= 1.0);

    /* lambda = 3 / span → mean = lo + span/3, ~95% below hi. */
    x = lo + (-log (1.0 - u)) * span / 3.0;
    return clamp (x, lo, hi);
}

static gdouble
sample (PnAutoRandom *self,
        PnAutoRandomDistribution dist,
        gdouble lo, gdouble hi)
{
    /* If the user wrote min > max accept the inversion silently —
     * either bound makes equal sense as "the other end of the range",
     * and refusing to sample would leave the node looking broken. */
    if (lo > hi)
    {
        gdouble t = lo;
        lo = hi;
        hi = t;
    }

    if (lo == hi)
        return lo;

    switch (dist)
    {
    case PN_AUTO_RANDOM_NORMAL:      return sample_normal      (self->rng, lo, hi);
    case PN_AUTO_RANDOM_TRIANGULAR:  return sample_triangular  (self->rng, lo, hi);
    case PN_AUTO_RANDOM_EXPONENTIAL: return sample_exponential (self->rng, lo, hi);
    case PN_AUTO_RANDOM_UNIFORM:
    default:                         return sample_uniform     (self->rng, lo, hi);
    }
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

static void
pn_auto_random_trigger (PnAutoTrigger *trigger)
{
    PnAutoRandom *self = PN_AUTO_RANDOM (trigger);
    PnNode       *node = PN_NODE (self);
    PnMessage    *msg;
    gchar        *output_copy;
    gdouble       lo, hi;
    PnAutoRandomDistribution dist;
    gboolean      success;
    gdouble       value;

    /* Snapshot the configured payload under the lock so the message
     * we hand to the main loop is internally consistent even if the
     * user changes a property mid-tick. */
    g_mutex_lock (&self->mutex);
    output_copy = g_strdup (self->output);
    lo          = self->min;
    hi          = self->max;
    dist        = self->distribution;
    success     = self->success;
    g_mutex_unlock (&self->mutex);

    value = sample (self, dist, lo, hi);

    msg = pn_message_new (node, NULL);
    pn_message_set_double  (msg, "value",   value);
    pn_message_set_boolean (msg, "success", success);
    if (output_copy != NULL)
        pn_message_set_string (msg, "output", output_copy);

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (output_copy);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_auto_random_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnAutoRandom *self = PN_AUTO_RANDOM (object);

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_OUTPUT:
        g_value_set_string (value, self->output);
        break;
    case PROP_MIN:
        g_value_set_double (value, self->min);
        break;
    case PROP_MAX:
        g_value_set_double (value, self->max);
        break;
    case PROP_DISTRIBUTION:
        g_value_set_enum (value, self->distribution);
        break;
    case PROP_SUCCESS:
        g_value_set_boolean (value, self->success);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }

    g_mutex_unlock (&self->mutex);
}

static void
pn_auto_random_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnAutoRandom *self    = PN_AUTO_RANDOM (object);
    gboolean      changed = FALSE;

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_OUTPUT:
        {
            const gchar *new_text = g_value_get_string (value);
            if (g_strcmp0 (self->output, new_text) != 0)
            {
                g_free (self->output);
                self->output = g_strdup (new_text);
                changed = TRUE;
            }
        }
        break;
    case PROP_MIN:
        {
            gdouble v = g_value_get_double (value);
            if (self->min != v)
            {
                self->min = v;
                changed = TRUE;
            }
        }
        break;
    case PROP_MAX:
        {
            gdouble v = g_value_get_double (value);
            if (self->max != v)
            {
                self->max = v;
                changed = TRUE;
            }
        }
        break;
    case PROP_DISTRIBUTION:
        {
            PnAutoRandomDistribution v =
                    (PnAutoRandomDistribution) g_value_get_enum (value);
            if (self->distribution != v)
            {
                self->distribution = v;
                changed = TRUE;
            }
        }
        break;
    case PROP_SUCCESS:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->success != v)
            {
                self->success = v;
                changed = TRUE;
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }

    g_mutex_unlock (&self->mutex);

    if (changed)
        g_object_notify_by_pspec (object, props[prop_id]);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_auto_random_finalize (GObject *object)
{
    PnAutoRandom *self = PN_AUTO_RANDOM (object);

    g_clear_pointer (&self->output, g_free);
    g_clear_pointer (&self->rng,    g_rand_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_auto_random_parent_class)->finalize (object);
}

static void
pn_auto_random_class_init (PnAutoRandomClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_auto_random_get_property;
    object_class->set_property = pn_auto_random_set_property;
    object_class->finalize     = pn_auto_random_finalize;
    trigger_class->trigger     = pn_auto_random_trigger;

    node_class->palette_icon   = PN_AUTO_RANDOM_ICON;
    node_class->class_name     = "AutoRandom";
    node_class->icon           = PN_AUTO_RANDOM_ICON;
    /* Coral / salmon — clearly distinct from #PnAutoInjector's
     * orange and #PnInject's teal so the three sources do not blur
     * together in a glance at the canvas. */
    node_class->color          = (GdkRGBA){ 0.86, 0.44, 0.39, 1.0 };
    node_class->category       = "Sources";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUT] = g_param_spec_string (
            "output", "Output",
            "Text emitted as data.output on every tick",
            PN_AUTO_RANDOM_DEFAULT_OUTPUT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MIN] = g_param_spec_double (
            "min", "Min",
            "Lower bound of the sampled range",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            PN_AUTO_RANDOM_DEFAULT_MIN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX] = g_param_spec_double (
            "max", "Max",
            "Upper bound of the sampled range",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            PN_AUTO_RANDOM_DEFAULT_MAX,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DISTRIBUTION] = g_param_spec_enum (
            "distribution", "Distribution",
            "Shape of the random draw on [min, max]",
            PN_TYPE_AUTO_RANDOM_DISTRIBUTION,
            PN_AUTO_RANDOM_UNIFORM,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SUCCESS] = g_param_spec_boolean (
            "success", "Success",
            "Boolean emitted as data.success on every tick",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_auto_random_init (PnAutoRandom *self)
{
    PnNode  *node  = PN_NODE (self);
    GdkRGBA  coral = { 0.86, 0.44, 0.39, 1.0 };

    g_mutex_init (&self->mutex);
    self->output       = g_strdup (PN_AUTO_RANDOM_DEFAULT_OUTPUT);
    self->min          = PN_AUTO_RANDOM_DEFAULT_MIN;
    self->max          = PN_AUTO_RANDOM_DEFAULT_MAX;
    self->distribution = PN_AUTO_RANDOM_UNIFORM;
    self->success      = TRUE;

    /* Mix the instance address into the seed so two AutoRandom nodes
     * created in the same millisecond do not produce the same first
     * draw — important when the user drops a couple of them onto the
     * worksheet in quick succession to compare distributions. */
    self->rng = g_rand_new_with_seed_array (
            (const guint32 [2]) {
                (guint32) g_get_real_time (),
                (guint32) (gulong) self,
            }, 2);

    pn_node_set_class_name (node, "AutoRandom");
    pn_node_set_icon       (node, PN_AUTO_RANDOM_ICON);
    pn_node_set_color      (node, &coral);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_AUTO_RANDOM_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnAutoRandom *
pn_auto_random_new (void)
{
    return g_object_new (PN_TYPE_AUTO_RANDOM, NULL);
}
