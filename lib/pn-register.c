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

#include "pn-register.h"
#include "pn-message.h"

/* Input ports.  A store on WRITE / RESET is deliberately silent — see
 * the header for why that keeps a feedback loop off the synchronous
 * dispatch stack. */
#define PN_REGISTER_IN_WRITE  0
#define PN_REGISTER_IN_READ   1
#define PN_REGISTER_IN_RESET  2
#define PN_REGISTER_N_INPUTS  3

struct _PnRegister
{
    PnNode parent_instance;

    /* Value the RESET port restores; also the value read out before the
     * first WRITE. */
    gdouble initial;

    /* The currently latched word. */
    gdouble stored;
};

G_DEFINE_TYPE (PnRegister, pn_register, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INITIAL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Pull a number out of @message under "value".  Returns %TRUE only
 *  when the member exists and holds a numeric JSON value (int or
 *  double); @out is left untouched otherwise. */
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

/** Rewrite @message to carry the stored word and emit it. */
static void
emit_stored (PnRegister *self, PnMessage *message)
{
    gchar *out = g_strdup_printf ("%g", self->stored);

    pn_message_set_double  (message, "value",   self->stored);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "output",  out);

    g_free (out);
    pn_node_emit_message (PN_NODE (self), message);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_register_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRegister *self = PN_REGISTER (node);
    gint        idx  = pn_node_current_input ();
    gdouble     value;

    switch (idx)
    {
    case PN_REGISTER_IN_READ:
        /* Emit the current word on the read/clock strobe. */
        emit_stored (self, message);
        break;

    case PN_REGISTER_IN_RESET:
        /* Restore the initial value; silent. */
        self->stored = self->initial;
        break;

    case PN_REGISTER_IN_WRITE:
    default:
        /* Latch a new word; silent.  A message with no usable number is
         * ignored, leaving the latch untouched. */
        if (read_value (message, &value))
            self->stored = value;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_register_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnRegister *self = PN_REGISTER (object);

    switch (prop_id)
    {
    case PROP_INITIAL:
        g_value_set_double (value, self->initial);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_register_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnRegister *self = PN_REGISTER (object);

    switch (prop_id)
    {
    case PROP_INITIAL:
        {
            gdouble v = g_value_get_double (value);
            if (self->initial != v)
            {
                self->initial = v;
                /* The initial value is also the power-on latch, read out
                 * before the first write; seed it so a node loaded with a
                 * non-zero initial reads that value straight away. */
                self->stored = v;
                g_object_notify_by_pspec (object, props[PROP_INITIAL]);
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
pn_register_class_init (PnRegisterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_register_get_property;
    object_class->set_property = pn_register_set_property;
    node_class->receive        = pn_register_receive;

    node_class->class_name     = "Register";
    node_class->icon           = "\xef\x83\x87";  /* fa-save U+F0C7 */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_INITIAL] = g_param_spec_double (
            "initial", "Initial",
            "Value restored by the reset input, and read out before the "
            "first write.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_register_init (PnRegister *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };

    self->initial = 0.0;
    self->stored  = 0.0;

    pn_node_set_class_name (node, "Register");
    pn_node_set_icon       (node, "\xef\x83\x87");  /* fa-save U+F0C7 */
    pn_node_set_color      (node, &indigo);
    pn_node_set_n_inputs   (node, PN_REGISTER_N_INPUTS);
    pn_node_set_has_output (node, TRUE);

    /* Name the ports so the worksheet's stacked input rows read as
     * write / read / reset rather than value1 / value2 / value3. */
    pn_node_set_input_name (node, PN_REGISTER_IN_WRITE, "write");
    pn_node_set_input_name (node, PN_REGISTER_IN_READ,  "read");
    pn_node_set_input_name (node, PN_REGISTER_IN_RESET, "reset");
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRegister *
pn_register_new (void)
{
    return g_object_new (PN_TYPE_REGISTER, NULL);
}
