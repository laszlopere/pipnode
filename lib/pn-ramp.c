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

#include "pn-ramp.h"
#include "pn-message.h"
#include "pn-vector.h"

#include <math.h>

/* Input port indices — also the order they appear under the header. */
#define PN_RAMP_IN_FROM 0
#define PN_RAMP_IN_TO   1
#define PN_RAMP_IN_N    2
#define PN_RAMP_N_INPUTS 3

/* Upper bound on the ramp length.  The whole point of TODO #43 is large
 * vectors, so this is generous (10M doubles ~= 80 MB), but it stops a
 * stray "N" of a billion from trying to allocate the machine's RAM. */
#define PN_RAMP_MAX_N 10000000

struct _PnRamp
{
    PnNode parent_instance;

    /* Most recent numeric value seen on each input.  The has_* flags
     * stay FALSE until that input has carried at least one number; no
     * ramp is emitted until all three are TRUE. */
    gboolean has_from;  gdouble from;
    gboolean has_to;    gdouble to;
    gboolean has_n;     gdouble n;
};

G_DEFINE_TYPE (PnRamp, pn_ramp, PN_TYPE_NODE)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Pull a number out of @message under "value".  Returns %TRUE only when
 *  the member exists and holds a numeric JSON value (int or double);
 *  @out is left untouched otherwise. */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

/** Build the linear ramp [from … to] in @len evenly-spaced steps as a
 *  freshly-allocated #PnVector.  @len is assumed already validated to be
 *  in [1, PN_RAMP_MAX_N].  With len == 1 the single sample is @from;
 *  otherwise both endpoints are hit exactly (the last element is @to). */
static PnVector *
build_ramp (gdouble from, gdouble to, gsize len)
{
    gdouble *data = g_new (gdouble, len);
    gsize    i;

    if (len == 1)
    {
        data[0] = from;
    }
    else
    {
        gdouble step = (to - from) / (gdouble) (len - 1);

        for (i = 0; i < len; i++)
            data[i] = from + step * (gdouble) i;

        /* Pin the final element to @to so rounding never leaves the ramp
         * a hair short of its endpoint. */
        data[len - 1] = to;
    }

    return pn_vector_new_take (data, len);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_ramp_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRamp   *self = PN_RAMP (node);
    gint      idx  = pn_node_current_input ();
    gdouble   value;
    gdouble   rounded;
    gsize     len;
    PnVector *vec;
    PnMessage *out;

    /* No usable number — keep our remembered inputs and stay silent. */
    if (!read_value (message, &value))
        return;

    /* Latch the value on whichever input it arrived on. */
    switch (idx)
    {
    case PN_RAMP_IN_FROM:  self->from = value;  self->has_from = TRUE;  break;
    case PN_RAMP_IN_TO:    self->to   = value;  self->has_to   = TRUE;  break;
    case PN_RAMP_IN_N:     self->n    = value;  self->has_n    = TRUE;  break;
    default:               return;
    }

    /* Need all three before there is anything to compute. */
    if (!self->has_from || !self->has_to || !self->has_n)
        return;

    /* Round N to the nearest whole number of samples and clamp it.  A
     * non-positive count has no ramp to produce, so emit nothing. */
    rounded = floor (self->n + 0.5);
    if (rounded < 1.0)
        return;
    if (rounded > (gdouble) PN_RAMP_MAX_N)
        rounded = (gdouble) PN_RAMP_MAX_N;
    len = (gsize) rounded;

    vec = build_ramp (self->from, self->to, len);

    /* Emit a brand-new message carrying the ramp as an out-of-band
     * vector on data.value — the universal slot the Calculator and the
     * Reduce node read by default. */
    out = pn_message_new (node, NULL);
    pn_message_set_vector (out, "value", vec);
    pn_node_emit_message (node, out);

    g_object_unref (vec);
    g_object_unref (out);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ramp_class_init (PnRampClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive    = pn_ramp_receive;

    node_class->class_name = "Ramp";
    node_class->icon       = "\xef\x85\xa0";  /* fa-sort-amount-asc U+F160 */
    node_class->color      = (PnColor){ 0.30, 0.70, 0.74, 1.0 };
    node_class->category   = "Filters/Vectors";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;
}

static void
pn_ramp_init (PnRamp *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.30, 0.70, 0.74, 1.0 };

    self->has_from = FALSE;
    self->has_to   = FALSE;
    self->has_n    = FALSE;

    pn_node_set_class_name  (node, "Ramp");
    pn_node_set_icon        (node, "\xef\x85\xa0");  /* fa-sort-amount-asc U+F160 */
    pn_node_set_color       (node, &teal);
    pn_node_set_n_inputs    (node, PN_RAMP_N_INPUTS);
    pn_node_set_input_name  (node, PN_RAMP_IN_FROM, "from");
    pn_node_set_input_name  (node, PN_RAMP_IN_TO,   "to");
    pn_node_set_input_name  (node, PN_RAMP_IN_N,    "N");
    pn_node_set_has_output  (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRamp *
pn_ramp_new (void)
{
    return g_object_new (PN_TYPE_RAMP, NULL);
}
