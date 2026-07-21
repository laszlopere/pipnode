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

#include "pn-mux.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>

#define PN_MUX_MIN_DATA 2
#define PN_MUX_MAX_DATA 8
#define PN_MUX_DEF_DATA 2

/* Input 0 is always the selector; the data inputs follow it. */
#define PN_MUX_IN_SELECT 0

struct _PnMux
{
    PnNode parent_instance;

    gint n_data;   /* number of data inputs, 2..8 (total ports = n_data + 1) */
};

G_DEFINE_TYPE (PnMux, pn_mux, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_INPUTS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Name the ports "select", "in1" … "inN" so the collated per-input
 *  values (and the worksheet's stacked input rows) read meaningfully. */
static void
mux_relabel (PnMux *self)
{
    PnNode *node = PN_NODE (self);
    gint    i;

    pn_node_set_input_name (node, PN_MUX_IN_SELECT, "select");
    for (i = 1; i <= self->n_data; i++)
    {
        gchar *name = g_strdup_printf ("in%d", i);
        pn_node_set_input_name (node, i, name);
        g_free (name);
    }
}

/** Numeric member @key of @data -> @out (int64/double only). */
static gboolean
read_member (JsonObject *data, const gchar *key, gdouble *out)
{
    JsonNode *node;
    GType     vt;

    if (data == NULL)
        return FALSE;

    node = json_object_get_member (data, key);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_mux_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnMux      *self = PN_MUX (node);
    gint        idx  = pn_node_current_input ();
    JsonObject *data = pn_message_get_data (message);
    gdouble     sel  = 0.0;
    gint        di;
    gint        port;
    gdouble     value;
    gchar      *out;

    /* The core has latched every input's last data.value under the
     * input's name.  The selector defaults to 0 until the select line
     * has carried a number. */
    read_member (data, "select", &sel);

    di = (gint) llround (sel);
    if (di < 0)
        di = 0;
    else if (di >= self->n_data)
        di = self->n_data - 1;
    port = di + 1;   /* data inputs occupy ports 1..n_data */

    /* Emit only when the selector itself changed or the *selected* data
     * line updated; an update on an unselected line is absorbed. */
    if (idx != PN_MUX_IN_SELECT && idx != port)
        return;

    /* The selected line may not have carried a value yet. */
    if (!read_member (data, pn_node_get_input_name (node, port), &value))
        return;

    out = g_strdup_printf ("%g", value);
    pn_message_set_double  (message, "value",   value);
    pn_message_set_boolean (message, "success", TRUE);
    pn_message_set_string  (message, "output",  out);
    g_free (out);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_mux_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnMux *self = PN_MUX (object);

    switch (prop_id)
    {
    case PROP_INPUTS:
        g_value_set_int (value, self->n_data);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_mux_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnMux *self = PN_MUX (object);

    switch (prop_id)
    {
    case PROP_INPUTS:
        {
            gint v = g_value_get_int (value);
            if (v != self->n_data)
            {
                self->n_data = v;
                /* One selector port plus v data ports; the core resizes
                 * its per-input collation latches to match. */
                pn_node_set_n_inputs (PN_NODE (self), v + 1);
                mux_relabel (self);
                g_object_notify_by_pspec (object, props[PROP_INPUTS]);
                pn_node_request_repaint (PN_NODE (self));
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
pn_mux_class_init (PnMuxClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_mux_get_property;
    object_class->set_property = pn_mux_set_property;
    node_class->receive        = pn_mux_receive;

    node_class->class_name     = "Mux";
    node_class->icon           = "\xef\x84\xa6";  /* fa-code-fork U+F126 */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_INPUTS] = g_param_spec_int (
            "inputs", "Data inputs",
            "How many data inputs the multiplexer has (2..8). The node "
            "also has a leading 'select' input whose value chooses which "
            "data line — 0 for in1, 1 for in2, … clamped into range — is "
            "forwarded onto data.value.",
            PN_MUX_MIN_DATA, PN_MUX_MAX_DATA, PN_MUX_DEF_DATA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Inputs");
        pn_settings_schema_row (schema, "inputs", PN_EDITOR_SPIN);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_mux_init (PnMux *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };

    self->n_data = PN_MUX_DEF_DATA;

    pn_node_set_class_name (node, "Mux");
    pn_node_set_icon       (node, "\xef\x84\xa6");  /* fa-code-fork U+F126 */
    pn_node_set_color      (node, &indigo);
    pn_node_set_n_inputs   (node, self->n_data + 1);  /* select + data lines */
    pn_node_set_has_output (node, TRUE);
    /* Latch each input's data.value under its name so any fire can read
     * the selector and every data line at once. */
    pn_node_set_collate_inputs (node, TRUE);
    mux_relabel (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMux *
pn_mux_new (void)
{
    return g_object_new (PN_TYPE_MUX, NULL);
}
