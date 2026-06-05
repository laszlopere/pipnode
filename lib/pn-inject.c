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

#include "pn-inject.h"
#include "pn-message.h"

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_INJECT_NORMAL_ICON  "\xef\x87\x98"      /* fa-paper-plane U+F1D8 */

struct _PnInject
{
    PnNode parent_instance;

    gchar    *text;
    gdouble   value;
    gboolean  success;

    /* Freedesktop icon name displayed on the worksheet's fire button.
     * Empty / NULL keeps the legacy 16-px tab with the cairo-drawn play
     * triangle; any other value swaps the triangle for the named theme
     * icon and widens the button to accommodate it.  Picked via the
     * Settings dialog's icon combo (see pn-inject-gui.c). */
    gchar    *button_icon;
};

G_DEFINE_TYPE (PnInject, pn_inject, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_TEXT,
    PROP_VALUE,
    PROP_SUCCESS,
    PROP_BUTTON_ICON,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Keep the node's healthy teal 💉 identity at all times and toggle the
 *  has-error flag for the unconfigured state; the red body + ❗ overlay
 *  is painted centrally by the worksheet whenever has-error is set. */
static void
apply_visual_state (
        PnInject *self,
        gboolean  configured)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.30, 0.66, 0.66, 1.0 };

    pn_node_set_color     (node, &teal);
    pn_node_set_icon      (node, PN_INJECT_NORMAL_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
inject_set_text (
        PnInject    *self,
        const gchar *text)
{
    /* Treat NULL and "" as the same unconfigured state but still
     * round-trip whatever the caller passed so `notify::text` reports
     * a stable value. */
    gchar    *replacement = (text != NULL) ? g_strdup (text) : NULL;
    gboolean  configured  = (text != NULL && *text != '\0');

    g_free (self->text);
    self->text = replacement;

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT]);
}

static void
pn_inject_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnInject *self = PN_INJECT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        g_value_set_string (value, self->text);
        break;
    case PROP_VALUE:
        g_value_set_double (value, self->value);
        break;
    case PROP_SUCCESS:
        g_value_set_boolean (value, self->success);
        break;
    case PROP_BUTTON_ICON:
        g_value_set_string (value, self->button_icon);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_inject_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnInject *self = PN_INJECT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        inject_set_text (self, g_value_get_string (value));
        break;
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
    case PROP_SUCCESS:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->success != v)
            {
                self->success = v;
                g_object_notify_by_pspec (object, props[PROP_SUCCESS]);
            }
        }
        break;
    case PROP_BUTTON_ICON:
        {
            const gchar *new_icon = g_value_get_string (value);
            /* Treat NULL and "" as the same unconfigured state so the
             * worksheet's renderer (which falls back when the string is
             * empty) does not need a NULL-vs-"" branch. */
            if (g_strcmp0 (self->button_icon, new_icon) != 0)
            {
                g_free (self->button_icon);
                self->button_icon = (new_icon != NULL)
                                       ? g_strdup (new_icon)
                                       : NULL;
                g_object_notify_by_pspec (object, props[PROP_BUTTON_ICON]);
                /* The fire button is part of the node's painted face, and
                 * the panel-applet mirror follows repaint-needed for live
                 * updates — so an icon swap has to ask for a repaint to
                 * propagate (both to the worksheet and to the applet). */
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
pn_inject_finalize (GObject *object)
{
    PnInject *self = PN_INJECT (object);

    g_clear_pointer (&self->text, g_free);
    g_clear_pointer (&self->button_icon, g_free);

    G_OBJECT_CLASS (pn_inject_parent_class)->finalize (object);
}

static void
pn_inject_class_init (PnInjectClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_inject_get_property;
    object_class->set_property = pn_inject_set_property;
    object_class->finalize     = pn_inject_finalize;

    /* The instance icon flips between 💉 and ❗ depending on whether
     * a text payload has been configured.  The palette wants a stable
     * glyph regardless of state, so pin the normal one here. */
    node_class->palette_icon   = PN_INJECT_NORMAL_ICON;
    node_class->class_name     = "Injector";
    node_class->icon           = PN_INJECT_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.30, 0.66, 0.66, 1.0 };
    node_class->category       = "Sources";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text",
            "Text payload to emit on activation under the message's "
            "\"output\" data member; while empty the node is marked "
            "as needing configuration and refuses to fire",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_VALUE] = g_param_spec_double (
            "value", "Value",
            "Number attached to the message's \"value\" data member "
            "on activation",
            -G_MAXDOUBLE, G_MAXDOUBLE,
            0.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SUCCESS] = g_param_spec_boolean (
            "success", "Success",
            "Boolean attached to the message's \"success\" data "
            "member; lets an inject node feed the edge-detector "
            "filter with synthetic success/failure events",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BUTTON_ICON] = g_param_spec_string (
            "button-icon", "Icon",
            "Freedesktop icon name shown on the worksheet's fire "
            "button.  When empty the button keeps the legacy compact "
            "tab with a small play triangle; when set the named theme "
            "icon is rendered inside a wider button to give a larger "
            "click target.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_inject_init (PnInject *self)
{
    PnNode *node = PN_NODE (self);

    self->text        = NULL;
    self->value       = 0.0;
    self->success     = TRUE;
    self->button_icon = NULL;

    pn_node_set_class_name (node, "Injector");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Pre-populate with a friendly default so a freshly-dropped node
     * is immediately fireable; the user can change it via the
     * Configure dialog or clear it to drop back into the
     * unconfigured (red ❗) state. */
    inject_set_text (self, "Injector activated.");
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnInject *
pn_inject_new (void)
{
    return g_object_new (PN_TYPE_INJECT, NULL);
}

void
pn_inject_fire (PnInject *self)
{
    PnNode    *node;
    PnMessage *msg;

    g_return_if_fail (PN_IS_INJECT (self));

    if (self->text == NULL || *self->text == '\0')
        return;

    node = PN_NODE (self);
    msg  = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "output",  self->text);
    pn_message_set_double  (msg, "value",   self->value);
    pn_message_set_boolean (msg, "success", self->success);

    pn_node_emit_message (node, msg);

    g_object_unref (msg);
}

const gchar *
pn_inject_get_button_icon (PnInject *self)
{
    g_return_val_if_fail (PN_IS_INJECT (self), NULL);
    return self->button_icon;
}
