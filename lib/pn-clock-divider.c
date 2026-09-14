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

#include "pn-clock-divider.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

struct _PnClockDivider
{
    PnNode parent_instance;

    gint     n_outputs;   /* configurable output count, 2..16           */
    guint64  count;       /* ticks since creation or the last reset     */

    /* Always PN_CLOCK_DIVIDER_MAX_OUTPUTS slots, so the divisors of
     * outputs removed by lowering the count survive until it is raised
     * again.  Every slot holds a valid divisor. */
    guint    divisors[PN_CLOCK_DIVIDER_MAX_OUTPUTS];
};

G_DEFINE_TYPE (PnClockDivider, pn_clock_divider, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OUTPUTS,
    PROP_DIVISORS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Firing plan                                                        */
/* ------------------------------------------------------------------ */

guint
pn_clock_divider_default_divisor (gint index)
{
    g_return_val_if_fail (index >= 0 && index < PN_CLOCK_DIVIDER_MAX_OUTPUTS,
                          2u);
    return 2u << index;
}

gint
pn_clock_divider_plan (
        PnClockDivider *self,
        guint64         count,
        gint           *order)
{
    gint n = 0;
    gint k;

    g_return_val_if_fail (PN_IS_CLOCK_DIVIDER (self), 0);
    g_return_val_if_fail (order != NULL, 0);

    if (count == 0)
        return 0;

    /* Insertion sort by divisor, largest first; walking the outputs from
     * the highest down and inserting after equal divisors keeps ties in
     * highest-output-first order. */
    for (k = self->n_outputs - 1; k >= 0; k--)
    {
        guint d = self->divisors[k];
        gint  i;

        if (count % d != 0)
            continue;

        for (i = n; i > 0 && self->divisors[order[i - 1]] < d; i--)
            order[i] = order[i - 1];
        order[i] = k;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_clock_divider_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnClockDivider *self = PN_CLOCK_DIVIDER (node);
    gint            order[PN_CLOCK_DIVIDER_MAX_OUTPUTS];
    gint            n;
    gint            i;

    if (pn_node_current_input () == PN_CLOCK_DIVIDER_IN_RESET)
    {
        /* Silent, so a reset wired from downstream forms no cycle. */
        self->count = 0;
        return;
    }

    self->count++;
    n = pn_clock_divider_plan (self, self->count, order);

    /* Each emission is a fresh clone so a "message" handler that edits
     * what it is given cannot reach the outputs that fire after it. */
    for (i = 0; i < n; i++)
    {
        PnMessage *out = pn_message_clone (message);

        pn_message_set_source (out, node);
        pn_node_emit_message_on_output (node, out, order[i]);
        g_object_unref (out);
    }
}

/* ------------------------------------------------------------------ */
/*  Divisor slots                                                      */
/* ------------------------------------------------------------------ */

static void
update_output_name (PnClockDivider *self, gint index)
{
    gchar *label = g_strdup_printf ("/%u", self->divisors[index]);

    pn_node_set_output_name (PN_NODE (self), index, label);
    g_free (label);
}

/* Store one slot; returns TRUE when it changed.  Does not notify. */
static gboolean
store_divisor (PnClockDivider *self, gint index, guint divisor)
{
    divisor = CLAMP (divisor, PN_CLOCK_DIVIDER_MIN_DIVISOR,
                     PN_CLOCK_DIVIDER_MAX_DIVISOR);
    if (divisor == self->divisors[index])
        return FALSE;

    self->divisors[index] = divisor;
    update_output_name (self, index);
    return TRUE;
}

/* JSON array of every slot, trailing default slots trimmed. */
static gchar *
divisors_to_json (PnClockDivider *self)
{
    JsonArray *arr  = json_array_new ();
    JsonNode  *root = json_node_new (JSON_NODE_ARRAY);
    gchar     *json;
    gint       last = PN_CLOCK_DIVIDER_MAX_OUTPUTS - 1;
    gint       i;

    while (last >= 0 &&
           self->divisors[last] == pn_clock_divider_default_divisor (last))
        last--;

    for (i = 0; i <= last; i++)
        json_array_add_int_element (arr, self->divisors[i]);

    json_node_take_array (root, arr);
    json = json_to_string (root, FALSE);
    json_node_unref (root);
    return json;
}

/* Replace every slot from a JSON array of numbers.  A slot the array
 * does not reach, or whose element is not a number, gets its default. */
static void
divisors_from_json (PnClockDivider *self, const gchar *json)
{
    JsonParser *parser = json_parser_new ();
    JsonArray  *arr    = NULL;
    guint       n      = 0;
    gint        i;

    if (json != NULL && *json != '\0' &&
        json_parser_load_from_data (parser, json, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            arr = json_node_get_array (root);
            n   = json_array_get_length (arr);
        }
    }

    for (i = 0; i < PN_CLOCK_DIVIDER_MAX_OUTPUTS; i++)
    {
        guint divisor = pn_clock_divider_default_divisor (i);

        if ((guint) i < n)
        {
            JsonNode *el = json_array_get_element (arr, i);

            if (JSON_NODE_HOLDS_VALUE (el))
            {
                GType   t = json_node_get_value_type (el);
                gdouble v = -1.0;

                if (t == G_TYPE_INT64)
                    v = (gdouble) json_node_get_int (el);
                else if (t == G_TYPE_DOUBLE)
                    v = json_node_get_double (el);

                if (v >= 0.0)
                    divisor = (guint) MIN (v + 0.5,
                                           (gdouble) PN_CLOCK_DIVIDER_MAX_DIVISOR);
            }
        }
        store_divisor (self, i, divisor);
    }

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
clock_divider_set_outputs (PnClockDivider *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    self->n_outputs = n;
    pn_node_set_n_outputs (node, n);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
pn_clock_divider_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnClockDivider *self = PN_CLOCK_DIVIDER (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        g_value_set_int (value, self->n_outputs);
        break;
    case PROP_DIVISORS:
        g_value_take_string (value, divisors_to_json (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_clock_divider_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnClockDivider *self = PN_CLOCK_DIVIDER (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        clock_divider_set_outputs (self, g_value_get_int (value));
        break;
    case PROP_DIVISORS:
        divisors_from_json (self, g_value_get_string (value));
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_clock_divider_class_init (PnClockDividerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_clock_divider_get_property;
    object_class->set_property = pn_clock_divider_set_property;
    node_class->receive        = pn_clock_divider_receive;
    /* build_class_tab installed by the gui tier (pn_clock_divider_gui_install). */

    node_class->class_name     = "Clock Divider";
    node_class->icon           = "\xef\x85\xa1";  /* fa-sort-amount-desc U+F161 */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of outputs, each with its own divisor.",
            PN_CLOCK_DIVIDER_MIN_OUTPUTS, PN_CLOCK_DIVIDER_MAX_OUTPUTS,
            PN_CLOCK_DIVIDER_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    props[PROP_DIVISORS] = g_param_spec_string (
            "divisors", "Divisors",
            "JSON array of divisors, one per output. Output k forwards "
            "every tick whose count is a multiple of its divisor. Missing "
            "entries default to powers of two (2, 4, 8, ...).",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_clock_divider_init (PnClockDivider *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };
    gint     i;

    self->n_outputs = PN_CLOCK_DIVIDER_DEF_OUTPUTS;

    pn_node_set_class_name (node, "Clock Divider");
    pn_node_set_icon       (node, "\xef\x85\xa1");  /* fa-sort-amount-desc U+F161 */
    pn_node_set_color      (node, &indigo);
    pn_node_set_n_inputs   (node, PN_CLOCK_DIVIDER_N_INPUTS);
    pn_node_set_input_name (node, PN_CLOCK_DIVIDER_IN_TICK,  "tick");
    pn_node_set_input_name (node, PN_CLOCK_DIVIDER_IN_RESET, "reset");
    pn_node_set_n_outputs  (node, self->n_outputs);

    for (i = 0; i < PN_CLOCK_DIVIDER_MAX_OUTPUTS; i++)
    {
        self->divisors[i] = pn_clock_divider_default_divisor (i);
        update_output_name (self, i);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnClockDivider *
pn_clock_divider_new (void)
{
    return g_object_new (PN_TYPE_CLOCK_DIVIDER, NULL);
}

guint
pn_clock_divider_get_divisor (PnClockDivider *self, gint index)
{
    g_return_val_if_fail (PN_IS_CLOCK_DIVIDER (self), 1u);
    g_return_val_if_fail (index >= 0 && index < PN_CLOCK_DIVIDER_MAX_OUTPUTS,
                          1u);

    return self->divisors[index];
}

void
pn_clock_divider_set_divisor (
        PnClockDivider *self,
        gint            index,
        guint           divisor)
{
    g_return_if_fail (PN_IS_CLOCK_DIVIDER (self));
    g_return_if_fail (index >= 0 && index < PN_CLOCK_DIVIDER_MAX_OUTPUTS);

    if (store_divisor (self, index, divisor))
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DIVISORS]);
}

guint64
pn_clock_divider_get_count (PnClockDivider *self)
{
    g_return_val_if_fail (PN_IS_CLOCK_DIVIDER (self), 0);
    return self->count;
}
