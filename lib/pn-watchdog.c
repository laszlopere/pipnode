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

#include "pn-watchdog.h"
#include "pn-message.h"

struct _PnWatchdog
{
    PnAutoTrigger parent_instance;

    /* Pre-set string copied into data.output of the alarm message.
     * Owned by the instance; touched only from the main thread. */
    gchar *output_text;

    /* Set to 1 by receive() on the main thread, atomically exchanged
     * back to 0 by trigger() on the worker thread.  Pure flag — we
     * only care whether anything arrived, not how many. */
    gint   saw_message;
};

G_DEFINE_TYPE (PnWatchdog, pn_watchdog, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_OUTPUT_TEXT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive (main thread)                                              */
/*                                                                     */
/*  We deliberately do not forward the message: this node's whole     */
/*  job is to watch the stream go by, not to pass it on.  We just     */
/*  mark that something arrived so the next tick can stay silent.     */
/* ------------------------------------------------------------------ */

static void
pn_watchdog_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnWatchdog *self = PN_WATCHDOG (node);

    (void) message;

    g_atomic_int_set (&self->saw_message, 1);
}

/* ------------------------------------------------------------------ */
/*  Trigger (worker thread)                                            */
/*                                                                     */
/*  Atomically read-and-clear the flag.  If the prior interval saw    */
/*  no traffic, build the alarm message and hand it back to the main  */
/*  thread for emission.  output-text is read directly: it is set     */
/*  via the property from the main thread, and a torn read here only  */
/*  costs at worst one tick of stale text — acceptable given that     */
/*  emission itself crosses back to the main thread immediately.      */
/* ------------------------------------------------------------------ */

static void
pn_watchdog_trigger (PnAutoTrigger *trigger)
{
    PnWatchdog *self = PN_WATCHDOG (trigger);
    PnNode     *node = PN_NODE (self);
    PnMessage  *msg;

    if (g_atomic_int_exchange (&self->saw_message, 0) != 0)
        return;

    msg = pn_message_new (node, NULL);
    pn_message_set_boolean (msg, "success", FALSE);
    pn_message_set_string  (msg, "output",
                            self->output_text ? self->output_text : "");

    pn_auto_trigger_emit_on_main (trigger, msg);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_watchdog_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnWatchdog *self = PN_WATCHDOG (object);

    switch (prop_id)
    {
    case PROP_OUTPUT_TEXT:
        g_value_set_string (value, self->output_text);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_watchdog_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnWatchdog *self = PN_WATCHDOG (object);

    switch (prop_id)
    {
    case PROP_OUTPUT_TEXT:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->output_text, s) != 0)
            {
                g_free (self->output_text);
                self->output_text = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_OUTPUT_TEXT]);
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
pn_watchdog_finalize (GObject *object)
{
    PnWatchdog *self = PN_WATCHDOG (object);

    g_clear_pointer (&self->output_text, g_free);

    G_OBJECT_CLASS (pn_watchdog_parent_class)->finalize (object);
}

static void
pn_watchdog_class_init (PnWatchdogClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_watchdog_get_property;
    object_class->set_property = pn_watchdog_set_property;
    object_class->finalize     = pn_watchdog_finalize;
    node_class->receive        = pn_watchdog_receive;
    trigger_class->trigger     = pn_watchdog_trigger;

    node_class->class_name     = "Watchdog";
    node_class->icon           = "\xef\x84\xb2";  /* fa-shield U+F132 */
    node_class->color          = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUT_TEXT] = g_param_spec_string (
            "output-text", "Output text",
            "Pre-set string written to data.output of the alarm "
            "message emitted when no input arrived during the period",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_watchdog_init (PnWatchdog *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->output_text = g_strdup ("");

    /* Start in the "just saw activity" state so the first auto-trigger
     * tick — which PnAutoTrigger now fires ~1s after construction so
     * long-period nodes do not stay blank after a worksheet load —
     * silently consumes this prearm instead of raising an alarm before
     * any upstream node has had a chance to produce.  Without this the
     * watchdog would emit a false alarm one second after every load,
     * regardless of the configured period.  After the prearm is
     * consumed the regular "no traffic during a full period = alarm"
     * semantics resume. */
    self->saw_message = 1;

    pn_node_set_class_name (node, "Watchdog");
    pn_node_set_icon       (node, "\xef\x84\xb2");  /* fa-shield U+F132 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnWatchdog *
pn_watchdog_new (void)
{
    return g_object_new (PN_TYPE_WATCHDOG, NULL);
}
