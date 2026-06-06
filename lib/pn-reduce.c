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

#include "pn-reduce.h"
#include "pn-message.h"
#include "pn-vector.h"

#include <math.h>

struct _PnReduce
{
    PnNode parent_instance;

    PnReduceMode mode;
};

G_DEFINE_TYPE (PnReduce, pn_reduce, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_MODE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Enum boxed type                                                    */
/* ------------------------------------------------------------------ */

GType
pn_reduce_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The nicks double as the combo-box labels; the numeric values
           stay fixed so saved projects keep their selection. */
        static const GEnumValue values[] = {
            { PN_REDUCE_MODE_SUM,     "PN_REDUCE_MODE_SUM",     "sum"     },
            { PN_REDUCE_MODE_MEAN,    "PN_REDUCE_MODE_MEAN",    "mean"    },
            { PN_REDUCE_MODE_MIN,     "PN_REDUCE_MODE_MIN",     "min"     },
            { PN_REDUCE_MODE_MAX,     "PN_REDUCE_MODE_MAX",     "max"     },
            { PN_REDUCE_MODE_RANGE,   "PN_REDUCE_MODE_RANGE",   "range"   },
            { PN_REDUCE_MODE_COUNT,   "PN_REDUCE_MODE_COUNT",   "count"   },
            { PN_REDUCE_MODE_PRODUCT, "PN_REDUCE_MODE_PRODUCT", "product" },
            { PN_REDUCE_MODE_STDDEV,  "PN_REDUCE_MODE_STDDEV",  "stddev"  },
            { PN_REDUCE_MODE_FIRST,   "PN_REDUCE_MODE_FIRST",   "first"   },
            { PN_REDUCE_MODE_LAST,    "PN_REDUCE_MODE_LAST",    "last"    },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnReduceMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Fold                                                               */
/* ------------------------------------------------------------------ */

/** Collapse the @n doubles at @d to a single scalar per @mode.  An empty
 *  buffer folds to the mode's identity: 0 for the additive folds, 1 for
 *  PRODUCT, and 0 for the order/spread folds that have no defined value
 *  without elements. */
static gdouble
fold (const gdouble *d, gsize n, PnReduceMode mode)
{
    gdouble acc;
    gsize   i;

    if (n == 0)
        return (mode == PN_REDUCE_MODE_PRODUCT) ? 1.0 : 0.0;

    switch (mode)
    {
    case PN_REDUCE_MODE_SUM:
    case PN_REDUCE_MODE_MEAN:
        acc = 0.0;
        for (i = 0; i < n; i++)
            acc += d[i];
        return (mode == PN_REDUCE_MODE_MEAN) ? acc / (gdouble) n : acc;

    case PN_REDUCE_MODE_MIN:
        acc = d[0];
        for (i = 1; i < n; i++)
            if (d[i] < acc)
                acc = d[i];
        return acc;

    case PN_REDUCE_MODE_MAX:
        acc = d[0];
        for (i = 1; i < n; i++)
            if (d[i] > acc)
                acc = d[i];
        return acc;

    case PN_REDUCE_MODE_RANGE:
    {
        gdouble lo = d[0], hi = d[0];
        for (i = 1; i < n; i++)
        {
            if (d[i] < lo) lo = d[i];
            if (d[i] > hi) hi = d[i];
        }
        return hi - lo;
    }

    case PN_REDUCE_MODE_COUNT:
        return (gdouble) n;

    case PN_REDUCE_MODE_PRODUCT:
        acc = 1.0;
        for (i = 0; i < n; i++)
            acc *= d[i];
        return acc;

    case PN_REDUCE_MODE_STDDEV:
    {
        gdouble mean = 0.0, var = 0.0;
        for (i = 0; i < n; i++)
            mean += d[i];
        mean /= (gdouble) n;
        for (i = 0; i < n; i++)
        {
            gdouble dev = d[i] - mean;
            var += dev * dev;
        }
        return sqrt (var / (gdouble) n);   /* population stddev */
    }

    case PN_REDUCE_MODE_FIRST:
        return d[0];

    case PN_REDUCE_MODE_LAST:
        return d[n - 1];

    default:
        return 0.0;
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_reduce_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnReduce *self = PN_REDUCE (node);
    JsonNode *value = pn_message_get_member (message, "value");
    PnVector *vec;
    gdouble   result;

    if (value == NULL)
        return;

    vec = pn_message_resolve_vector (message, value);
    if (vec != NULL)
    {
        result = fold (pn_vector_get_data (vec),
                       pn_vector_get_len  (vec),
                       self->mode);
    }
    else if (JSON_NODE_HOLDS_VALUE (value) &&
             (json_node_get_value_type (value) == G_TYPE_DOUBLE ||
              json_node_get_value_type (value) == G_TYPE_INT64))
    {
        /* A plain scalar folds as a one-element vector. */
        gdouble scalar = json_node_get_double (value);
        result = fold (&scalar, 1, self->mode);
    }
    else
    {
        /* Neither a vector nor a number — nothing to fold; stay silent. */
        return;
    }

    /* Re-enter the scalar world: overwrite data.value with the fold and
     * forward the message, leaving topic / id / other members intact. */
    pn_message_set_double (message, "value", result);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_reduce_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnReduce *self = PN_REDUCE (object);

    switch (prop_id)
    {
    case PROP_MODE:
        g_value_set_enum (value, self->mode);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_reduce_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnReduce *self = PN_REDUCE (object);

    switch (prop_id)
    {
    case PROP_MODE:
        {
            PnReduceMode m = g_value_get_enum (value);
            if (self->mode != m)
            {
                self->mode = m;
                g_object_notify_by_pspec (object, props[PROP_MODE]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_reduce_class_init (PnReduceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_reduce_get_property;
    object_class->set_property = pn_reduce_set_property;
    node_class->receive        = pn_reduce_receive;

    node_class->class_name     = "Reduce";
    node_class->icon           = "\xef\x81\xa6";  /* fa-compress U+F066 */
    node_class->color          = (PnColor){ 0.30, 0.62, 0.74, 1.0 };
    node_class->category       = "Filters/Vectors";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_MODE] = g_param_spec_enum (
            "mode", "Mode",
            "Which fold collapses the vector to a single scalar",
            PN_TYPE_REDUCE_MODE,
            PN_REDUCE_MODE_MEAN,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_reduce_init (PnReduce *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.30, 0.62, 0.74, 1.0 };

    self->mode = PN_REDUCE_MODE_MEAN;

    pn_node_set_class_name  (node, "Reduce");
    pn_node_set_icon        (node, "\xef\x81\xa6");  /* fa-compress U+F066 */
    pn_node_set_color       (node, &blue);
    pn_node_set_has_input   (node, TRUE);
    pn_node_set_has_output  (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnReduce *
pn_reduce_new (void)
{
    return g_object_new (PN_TYPE_REDUCE, NULL);
}
