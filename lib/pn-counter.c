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

#include "pn-counter.h"
#include "pn-message.h"

#include <math.h>

#define PN_COUNTER_IN_TICK   0
#define PN_COUNTER_IN_LOAD   1
#define PN_COUNTER_IN_RESET  2
#define PN_COUNTER_N_INPUTS  3

struct _PnCounter
{
    PnNode parent_instance;

    gdouble initial;   /* value the reset port restores               */
    gdouble step;      /* amount added on each tick                   */
    gdouble modulo;    /* wrap the advanced value at this, 0 = no wrap */

    gdouble stored;    /* the current count / program address          */

    /* A `load` (or `reset`) that lands while a tick's downstream chain
     * is still running must override that cycle's implicit advance —
     * this is how a JUMP decoded from the fetched instruction takes
     * effect next tick.  During the tick's emit we only *record* the
     * override here and apply it after the emit unwinds. */
    gboolean in_tick;
    gboolean pending_write;
    gdouble  pending_value;
};

G_DEFINE_TYPE (PnCounter, pn_counter, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INITIAL,
    PROP_STEP,
    PROP_MODULO,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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

/** The wrapped result of advancing @v, honouring #PnCounter:modulo.
 *  Only the natural per-tick advance wraps; an explicit load/reset
 *  target is stored verbatim. */
static gdouble
wrap_advance (PnCounter *self, gdouble v)
{
    if (self->modulo > 0.0)
    {
        v = fmod (v, self->modulo);
        if (v < 0.0)
            v += self->modulo;
    }
    return v;
}

/** Route a load/reset target: record it as a pending override while a
 *  tick is in flight, otherwise apply it immediately. */
static void
counter_write (PnCounter *self, gdouble v)
{
    if (self->in_tick)
    {
        self->pending_write = TRUE;
        self->pending_value = v;
    }
    else
    {
        self->stored = v;
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_counter_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnCounter *self = PN_COUNTER (node);
    gint       idx  = pn_node_current_input ();
    gdouble    value;
    gdouble    addr;
    gchar     *out;

    switch (idx)
    {
    case PN_COUNTER_IN_LOAD:
        /* JUMP target; silent. */
        if (read_value (message, &value))
            counter_write (self, value);
        return;

    case PN_COUNTER_IN_RESET:
        /* Restore the initial value; silent. */
        counter_write (self, self->initial);
        return;

    case PN_COUNTER_IN_TICK:
    default:
        break;
    }

    /* Tick: emit the current address, then advance — unless a load/reset
     * fired during the (synchronous) downstream emit, in which case that
     * target wins for the next cycle. */
    addr = self->stored;
    out  = g_strdup_printf ("%g", addr);

    pn_message_set_double  (message, "value",   addr);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "output",  out);
    g_free (out);

    self->in_tick       = TRUE;
    self->pending_write = FALSE;

    pn_node_emit_message (node, message);

    self->in_tick = FALSE;
    self->stored  = self->pending_write ? self->pending_value
                                        : wrap_advance (self, addr + self->step);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_counter_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnCounter *self = PN_COUNTER (object);

    switch (prop_id)
    {
    case PROP_INITIAL:
        g_value_set_double (value, self->initial);
        break;
    case PROP_STEP:
        g_value_set_double (value, self->step);
        break;
    case PROP_MODULO:
        g_value_set_double (value, self->modulo);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_counter_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnCounter *self = PN_COUNTER (object);

    switch (prop_id)
    {
    case PROP_INITIAL:
        {
            gdouble v = g_value_get_double (value);
            if (self->initial != v)
            {
                self->initial = v;
                /* The initial value is also where the counter starts, so
                 * a node loaded with a non-zero initial ticks from there. */
                self->stored = v;
                g_object_notify_by_pspec (object, props[PROP_INITIAL]);
            }
        }
        break;
    case PROP_STEP:
        {
            gdouble v = g_value_get_double (value);
            if (self->step != v)
            {
                self->step = v;
                g_object_notify_by_pspec (object, props[PROP_STEP]);
            }
        }
        break;
    case PROP_MODULO:
        {
            gdouble v = g_value_get_double (value);
            if (self->modulo != v)
            {
                self->modulo = v;
                g_object_notify_by_pspec (object, props[PROP_MODULO]);
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
pn_counter_class_init (PnCounterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_counter_get_property;
    object_class->set_property = pn_counter_set_property;
    node_class->receive        = pn_counter_receive;

    node_class->class_name     = "Counter";
    node_class->icon           = "\xef\x81\x91";  /* fa-step-forward U+F051 */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_INITIAL] = g_param_spec_double (
            "initial", "Initial",
            "Value the reset input restores; the counter also starts here.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_STEP] = g_param_spec_double (
            "step", "Step",
            "Amount added to the value after each tick.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODULO] = g_param_spec_double (
            "modulo", "Modulo",
            "When greater than zero, the advanced value wraps into "
            "[0, modulo); zero disables wrapping. An explicit load or "
            "reset target is stored unchanged.",
            0.0, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_counter_init (PnCounter *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };

    self->initial       = 0.0;
    self->step          = 1.0;
    self->modulo        = 0.0;
    self->stored        = 0.0;
    self->in_tick       = FALSE;
    self->pending_write = FALSE;

    pn_node_set_class_name (node, "Counter");
    pn_node_set_icon       (node, "\xef\x81\x91");  /* fa-step-forward U+F051 */
    pn_node_set_color      (node, &indigo);
    pn_node_set_n_inputs   (node, PN_COUNTER_N_INPUTS);
    pn_node_set_has_output (node, TRUE);

    pn_node_set_input_name (node, PN_COUNTER_IN_TICK,  "tick");
    pn_node_set_input_name (node, PN_COUNTER_IN_LOAD,  "load");
    pn_node_set_input_name (node, PN_COUNTER_IN_RESET, "reset");
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnCounter *
pn_counter_new (void)
{
    return g_object_new (PN_TYPE_COUNTER, NULL);
}
