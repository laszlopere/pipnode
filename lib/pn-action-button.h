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

#ifndef PN_ACTION_BUTTON_H
#define PN_ACTION_BUTTON_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The accent of an action button.  Device dialogs use a small, fixed
 * vocabulary of button accents so the same kind of action looks the same
 * everywhere -- a green/blue "go ahead" for the section's committing
 * action and a red "be careful" for the irreversible one. */
typedef enum
{
    /* No accent -- an ordinary secondary button (Refresh, Copy, …). */
    PN_ACTION_BUTTON_NORMAL,

    /* Blue (GTK "suggested-action"): the complex or final action of a
     * section -- the Apply / Save / Install that commits the user's
     * edits.  Use one per section. */
    PN_ACTION_BUTTON_SUGGESTED,

    /* Red (GTK "destructive-action"): a dangerous, hard-to-undo action --
     * Delete, Remove, Reset, Factory-reset. */
    PN_ACTION_BUTTON_DESTRUCTIVE
} PnActionButtonStyle;

/* A #GtkButton that wears one of the #PnActionButtonStyle accents.
 *
 * Device dialogs (host and plugin) had started reaching for the raw GTK
 * "suggested-action" / "destructive-action" style classes by hand, which
 * drifts as soon as one caller forgets a class or picks the wrong one.
 * This widget centralises the mapping: pick the accent by intent and the
 * look (and any future theming, via the "pn-action-button" CSS class it
 * always carries) is controlled in one place.
 *
 * Pack it into the right-aligned bar a section's Apply lives in --
 * pn_device_form_add_action_bar(). */

#define PN_TYPE_ACTION_BUTTON (pn_action_button_get_type ())
G_DECLARE_FINAL_TYPE (PnActionButton, pn_action_button, PN, ACTION_BUTTON,
                      GtkButton)

/* A button labelled @label (with mnemonic, like
 * gtk_button_new_with_mnemonic(): an '_' marks the accelerator, "__" is a
 * literal underscore) and the given @style accent. */
GtkWidget *pn_action_button_new (const gchar         *label,
                                 PnActionButtonStyle  style);

/* The same, showing the themed icon @icon_name instead of a label --
 * for a compact action (a trash icon for a destructive row button). */
GtkWidget *pn_action_button_new_from_icon_name (const gchar         *icon_name,
                                                PnActionButtonStyle  style);

/* Change the accent after construction (e.g. enabling a destructive
 * accent once a selection makes the action available). */
void                pn_action_button_set_style (PnActionButton      *self,
                                                PnActionButtonStyle  style);
PnActionButtonStyle pn_action_button_get_style (PnActionButton      *self);

G_END_DECLS

#endif /* PN_ACTION_BUTTON_H */
