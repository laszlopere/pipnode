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

/* ------------------------------------------------------------------ */
/*  PnPanelInput — logic tier (headless core).                         */
/*                                                                     */
/*  GTK-free source node: the engine calls pn_panel_input_send() when  */
/*  the panel applet is clicked, and the node emits a "value" message  */
/*  into the worksheet.  Announces its current value once on load, the */
/*  same startup-report PnKnob / PnSwitch do.  Renders with the        */
/*  default node painter (no gui companion needed).                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-panel-input.h"
#include "pn-message.h"

/* fa-hand-pointer-o U+F25A — a finger about to press, matching the
 * "panel button" role.  FontAwesome 4.7 (the only set that renders). */
#define PN_PANEL_INPUT_ICON  "\xef\x89\x9a"

struct _PnPanelInput
{
    PnNode parent_instance;

    /* Current value, carried on the next emitted message.  Restored
     * from the file on load and updated by every pn_panel_input_send(). */
    gdouble value;

    /* Startup announce: emit @value once shortly after construction so
     * downstream nodes learn the starting value without a click.
     * @startup_emit_id is the one-shot idle source id, 0 once fired or
     * cancelled (pulled in dispose if the node dies first). */
    guint startup_emit_id;
};

G_DEFINE_TYPE (PnPanelInput, pn_panel_input, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_VALUE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Emit                                                               */
/* ------------------------------------------------------------------ */

static void
emit_value_message (PnPanelInput *self)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg  = pn_message_new (node, NULL);

    pn_message_set_double  (msg, "value",   self->value);
    pn_message_set_boolean (msg, "success", TRUE);

    pn_node_emit_message (node, msg);
    g_object_unref (msg);
}

/* ------------------------------------------------------------------ */
/*  Startup announce (see PnSwitch for the deferred-idle rationale)    */
/* ------------------------------------------------------------------ */

static gboolean
emit_startup_value (gpointer data)
{
    PnPanelInput *self = PN_PANEL_INPUT (data);

    self->startup_emit_id = 0;
    emit_value_message (self);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_panel_input_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPanelInput *self = PN_PANEL_INPUT (object);

    switch (prop_id)
    {
    case PROP_VALUE:
        g_value_set_double (value, self->value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_panel_input_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPanelInput *self = PN_PANEL_INPUT (object);

    switch (prop_id)
    {
    case PROP_VALUE:
        {
            gdouble v = g_value_get_double (value);
            if (self->value != v)
            {
                self->value = v;
                g_object_notify_by_pspec (object, props[PROP_VALUE]);
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
pn_panel_input_constructed (GObject *object)
{
    PnPanelInput *self = PN_PANEL_INPUT (object);

    G_OBJECT_CLASS (pn_panel_input_parent_class)->constructed (object);

    /* Deferred so the announce fires after the loader has wired the
     * output and applied the saved "value" — same reasoning as
     * PnSwitch::constructed.  The idle holds no reference; dispose()
     * pulls it if the node dies first. */
    self->startup_emit_id = g_idle_add (emit_startup_value, self);
}

static void
pn_panel_input_dispose (GObject *object)
{
    PnPanelInput *self = PN_PANEL_INPUT (object);

    if (self->startup_emit_id != 0)
    {
        g_source_remove (self->startup_emit_id);
        self->startup_emit_id = 0;
    }

    G_OBJECT_CLASS (pn_panel_input_parent_class)->dispose (object);
}

static void
pn_panel_input_class_init (PnPanelInputClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_panel_input_get_property;
    object_class->set_property = pn_panel_input_set_property;
    object_class->constructed  = pn_panel_input_constructed;
    object_class->dispose      = pn_panel_input_dispose;

    node_class->palette_icon = PN_PANEL_INPUT_ICON;
    node_class->class_name   = "Panel Input";
    node_class->icon         = PN_PANEL_INPUT_ICON;
    node_class->color        = (PnColor){ 0.36, 0.50, 0.72, 1.0 };
    node_class->category     = "Sources";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;

    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Numeric value carried on the next emitted message.  "
            "Restored from the worksheet on load (and announced once on "
            "startup); updated each time the panel applet is clicked.",
            -G_MAXDOUBLE, G_MAXDOUBLE, 0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_panel_input_init (PnPanelInput *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.36, 0.50, 0.72, 1.0 };

    self->value           = 0.0;
    self->startup_emit_id = 0;

    pn_node_set_class_name (node, "Panel Input");
    pn_node_set_icon       (node, PN_PANEL_INPUT_ICON);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
    pn_node_set_color      (node, &blue);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPanelInput *
pn_panel_input_new (void)
{
    return g_object_new (PN_TYPE_PANEL_INPUT, NULL);
}

void
pn_panel_input_send (
        PnPanelInput *self,
        gdouble       value)
{
    g_return_if_fail (PN_IS_PANEL_INPUT (self));

    if (self->value != value)
    {
        self->value = value;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALUE]);
    }

    emit_value_message (self);
}
