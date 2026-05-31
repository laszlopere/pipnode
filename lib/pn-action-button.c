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

#include "pn-action-button.h"

struct _PnActionButton
{
    GtkButton            parent_instance;
    PnActionButtonStyle  style;
};

G_DEFINE_TYPE (PnActionButton, pn_action_button, GTK_TYPE_BUTTON)

/* The GTK style class for each accent, or NULL for the plain button.  The
 * two accent classes are the standard Adwaita ones, so the widget looks
 * native in any GTK theme while keeping the choice in one place. */
static const gchar *
style_class (PnActionButtonStyle style)
{
    switch (style)
    {
        case PN_ACTION_BUTTON_SUGGESTED:   return "suggested-action";
        case PN_ACTION_BUTTON_DESTRUCTIVE: return "destructive-action";
        case PN_ACTION_BUTTON_NORMAL:
        default:                           return NULL;
    }
}

static void
pn_action_button_class_init (PnActionButtonClass *klass)
{
    (void) klass;
}

static void
pn_action_button_init (PnActionButton *self)
{
    self->style = PN_ACTION_BUTTON_NORMAL;
    gtk_style_context_add_class (
            gtk_widget_get_style_context (GTK_WIDGET (self)),
            "pn-action-button");
}

void
pn_action_button_set_style (PnActionButton      *self,
                            PnActionButtonStyle  style)
{
    GtkStyleContext *sc;

    g_return_if_fail (PN_IS_ACTION_BUTTON (self));

    sc = gtk_widget_get_style_context (GTK_WIDGET (self));

    /* Drop whichever accent we may have worn, then add the new one --
     * order matters so set_style is idempotent and switches cleanly. */
    gtk_style_context_remove_class (sc, "suggested-action");
    gtk_style_context_remove_class (sc, "destructive-action");
    if (style_class (style) != NULL)
        gtk_style_context_add_class (sc, style_class (style));

    self->style = style;
}

PnActionButtonStyle
pn_action_button_get_style (PnActionButton *self)
{
    g_return_val_if_fail (PN_IS_ACTION_BUTTON (self), PN_ACTION_BUTTON_NORMAL);
    return self->style;
}

GtkWidget *
pn_action_button_new (const gchar         *label,
                      PnActionButtonStyle  style)
{
    PnActionButton *self = g_object_new (PN_TYPE_ACTION_BUTTON,
                                         "label",         label,
                                         "use-underline", TRUE,
                                         NULL);
    pn_action_button_set_style (self, style);
    return GTK_WIDGET (self);
}

GtkWidget *
pn_action_button_new_from_icon_name (const gchar         *icon_name,
                                     PnActionButtonStyle  style)
{
    GtkWidget      *image = gtk_image_new_from_icon_name (icon_name,
                                                          GTK_ICON_SIZE_BUTTON);
    PnActionButton *self  = g_object_new (PN_TYPE_ACTION_BUTTON,
                                          "image", image,
                                          NULL);
    pn_action_button_set_style (self, style);
    return GTK_WIDGET (self);
}
