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
/*  PnPanelDisplay — logic tier (headless core).                       */
/*                                                                     */
/*  GTK-free: holds the last display string, derives it from incoming  */
/*  messages, and emits "value-changed" so the panel engine can mirror */
/*  it onto an applet button over D-Bus.  Renders with the default     */
/*  node painter (no gui companion needed).                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-panel-display.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>

/* fa-desktop U+F108 — a screen/panel, matching the "show on the panel"
 * role.  FontAwesome 4.7 (the only set whose glyphs render here). */
#define PN_PANEL_DISPLAY_ICON  "\xef\x84\x88"

struct _PnPanelDisplay
{
    PnNode parent_instance;

    /* Most recently displayed string.  Never NULL once init has run;
     * the empty string until the first message arrives. */
    gchar *text;
};

G_DEFINE_TYPE (PnPanelDisplay, pn_panel_display, PN_TYPE_NODE)

enum {
    SIG_VALUE_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  Derive the display string from a message's data bag               */
/*                                                                     */
/*  Preference order mirrors the standard data-bag members documented  */
/*  in the user manual: an explicit "text" wins; otherwise "value" is  */
/*  formatted (booleans as on/off, numbers locale-independently); a    */
/*  message with neither falls back to its topic, then the empty       */
/*  string.  Caller frees the result.                                  */
/* ------------------------------------------------------------------ */

static gchar *
display_string_for (PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *n;

    if (data != NULL && json_object_has_member (data, "text"))
    {
        n = json_object_get_member (data, "text");
        if (JSON_NODE_HOLDS_VALUE (n) &&
            json_node_get_value_type (n) == G_TYPE_STRING)
            return g_strdup (json_node_get_string (n));
    }

    if (data != NULL && json_object_has_member (data, "value"))
    {
        n = json_object_get_member (data, "value");
        if (JSON_NODE_HOLDS_VALUE (n))
        {
            GType t = json_node_get_value_type (n);

            if (t == G_TYPE_BOOLEAN)
                return g_strdup (json_node_get_boolean (n) ? "on" : "off");
            if (t == G_TYPE_INT64)
                return g_strdup_printf ("%" G_GINT64_FORMAT,
                                        json_node_get_int (n));
            if (t == G_TYPE_DOUBLE)
            {
                gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
                g_ascii_dtostr (buf, sizeof buf, json_node_get_double (n));
                return g_strdup (buf);
            }
            if (t == G_TYPE_STRING)
                return g_strdup (json_node_get_string (n));
        }
    }

    {
        const gchar *topic = pn_message_get_topic (message);
        return g_strdup (topic != NULL ? topic : "");
    }
}

/* ------------------------------------------------------------------ */
/*  Receive — store + announce                                         */
/* ------------------------------------------------------------------ */

static void
pn_panel_display_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnPanelDisplay *self = PN_PANEL_DISPLAY (node);
    gchar          *next = display_string_for (message);

    if (g_strcmp0 (next, self->text) == 0)
    {
        /* No visible change: keep the panel quiet rather than firing a
         * redundant D-Bus signal on every repeated value. */
        g_free (next);
        return;
    }

    g_free (self->text);
    self->text = next;

    pn_node_request_repaint (node);
    g_signal_emit (self, signals[SIG_VALUE_CHANGED], 0, self->text);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_panel_display_finalize (GObject *object)
{
    PnPanelDisplay *self = PN_PANEL_DISPLAY (object);

    g_clear_pointer (&self->text, g_free);

    G_OBJECT_CLASS (pn_panel_display_parent_class)->finalize (object);
}

static void
pn_panel_display_class_init (PnPanelDisplayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->finalize = pn_panel_display_finalize;

    node_class->receive = pn_panel_display_receive;

    node_class->palette_icon = PN_PANEL_DISPLAY_ICON;
    node_class->class_name   = "Panel Display";
    node_class->icon         = PN_PANEL_DISPLAY_ICON;
    node_class->color        = (PnColor){ 0.36, 0.50, 0.72, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    /* No output port: this sink never stamps its own base topic onto an
     * emitted message, so the inherited "topic" row is dead UI — hide it. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    /* Emitted whenever the displayed string changes.  The panel engine
     * connects to this and forwards the new text to the applet over
     * D-Bus; a sink emits no node "message", so this is the only
     * observation seam. */
    signals[SIG_VALUE_CHANGED] = g_signal_new (
            "value-changed",
            PN_TYPE_PANEL_DISPLAY,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);
}

static void
pn_panel_display_init (PnPanelDisplay *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.36, 0.50, 0.72, 1.0 };

    self->text = g_strdup ("");

    pn_node_set_class_name (node, "Panel Display");
    pn_node_set_icon       (node, PN_PANEL_DISPLAY_ICON);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
    pn_node_set_color      (node, &blue);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPanelDisplay *
pn_panel_display_new (void)
{
    return g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);
}

gchar *
pn_panel_display_dup_text (PnPanelDisplay *self)
{
    g_return_val_if_fail (PN_IS_PANEL_DISPLAY (self), g_strdup (""));
    return g_strdup (self->text != NULL ? self->text : "");
}
