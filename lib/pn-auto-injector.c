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

#include "pn-auto-injector.h"
#include "pn-message.h"

/* fa-paper-plane U+F1D8 — same glyph as the manual #PnInject so the
 * family resemblance is visible at a glance; the body colour
 * (orange instead of teal) carries the "this one fires itself" cue. */
#define PN_AUTO_INJECTOR_ICON           "\xef\x87\x98"

#define PN_AUTO_INJECTOR_DEFAULT_PERIOD  1u
#define PN_AUTO_INJECTOR_DEFAULT_OUTPUT  "AutoInjector activated."

struct _PnAutoInjector
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads these on every tick while the main
     * thread may overwrite them via property setters; @mutex
     * serialises the two so a half-applied property change cannot
     * leak into an in-flight message. */
    GMutex   mutex;
    gchar   *output;     /* data.output payload */
    gdouble  value;      /* data.value  payload */
    gboolean success;    /* data.success flag   */
};

G_DEFINE_TYPE (PnAutoInjector, pn_auto_injector, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_OUTPUT,
    PROP_VALUE,
    PROP_SUCCESS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

static void
pn_auto_injector_trigger (PnAutoTrigger *trigger)
{
    PnAutoInjector *self = PN_AUTO_INJECTOR (trigger);
    PnNode         *node = PN_NODE (self);
    PnMessage      *msg;
    gchar          *output_copy;
    gdouble         value;
    gboolean        success;

    /* Snapshot the configured payload under the lock so the message
     * we hand to the main loop is internally consistent even if the
     * user changes a property mid-tick. */
    g_mutex_lock (&self->mutex);
    output_copy = g_strdup (self->output);
    value       = self->value;
    success     = self->success;
    g_mutex_unlock (&self->mutex);

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
pn_auto_injector_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnAutoInjector *self = PN_AUTO_INJECTOR (object);

    g_mutex_lock (&self->mutex);

    switch (prop_id)
    {
    case PROP_OUTPUT:
        g_value_set_string (value, self->output);
        break;
    case PROP_VALUE:
        g_value_set_double (value, self->value);
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
pn_auto_injector_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnAutoInjector *self = PN_AUTO_INJECTOR (object);
    gboolean        changed = FALSE;

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
    case PROP_VALUE:
        {
            gdouble v = g_value_get_double (value);
            if (self->value != v)
            {
                self->value = v;
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
pn_auto_injector_finalize (GObject *object)
{
    PnAutoInjector *self = PN_AUTO_INJECTOR (object);

    g_clear_pointer (&self->output, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_auto_injector_parent_class)->finalize (object);
}

static void
pn_auto_injector_class_init (PnAutoInjectorClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_auto_injector_get_property;
    object_class->set_property = pn_auto_injector_set_property;
    object_class->finalize     = pn_auto_injector_finalize;
    trigger_class->trigger     = pn_auto_injector_trigger;

    node_class->palette_icon   = PN_AUTO_INJECTOR_ICON;
    node_class->class_name     = "AutoInjector";
    node_class->icon           = PN_AUTO_INJECTOR_ICON;
    node_class->color          = (GdkRGBA){ 0.93, 0.55, 0.18, 1.0 };
    node_class->category       = "Sources";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUT] = g_param_spec_string (
            "output", "Output",
            "Text emitted as data.output on every tick",
            PN_AUTO_INJECTOR_DEFAULT_OUTPUT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Number emitted as data.value on every tick",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SUCCESS] = g_param_spec_boolean (
            "success", "Success",
            "Boolean emitted as data.success on every tick",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_auto_injector_init (PnAutoInjector *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  orange = { 0.93, 0.55, 0.18, 1.0 };

    g_mutex_init (&self->mutex);
    self->output  = g_strdup (PN_AUTO_INJECTOR_DEFAULT_OUTPUT);
    self->value   = 0.0;
    self->success = TRUE;

    pn_node_set_class_name (node, "AutoInjector");
    pn_node_set_icon       (node, PN_AUTO_INJECTOR_ICON);
    pn_node_set_color      (node, &orange);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_AUTO_INJECTOR_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnAutoInjector *
pn_auto_injector_new (void)
{
    return g_object_new (PN_TYPE_AUTO_INJECTOR, NULL);
}
