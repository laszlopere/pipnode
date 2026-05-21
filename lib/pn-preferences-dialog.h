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

#ifndef PN_PREFERENCES_DIALOG_H
#define PN_PREFERENCES_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_PREFERENCES_DIALOG (pn_preferences_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PnPreferencesDialog,
                      pn_preferences_dialog,
                      PN, PREFERENCES_DIALOG,
                      GtkWindow)

/**
 * pn_preferences_dialog_present:
 * @parent: (nullable): a window to anchor the dialog against (transient
 *   parent and on-screen position hint); %NULL leaves the dialog
 *   parent-less and positioned by the WM.
 *
 * Shows the application-wide preferences dialog and brings it to the
 * front.  The dialog is non-modal, lives for the rest of the process
 * lifetime once created, and lazy-binds to the
 * #PnPreferences singleton so changes apply live to every open
 * worksheet.  Calling this while the dialog is already up just raises
 * the existing window — there is at most one dialog instance.
 */
void pn_preferences_dialog_present (GtkWindow *parent);

G_END_DECLS

#endif /* PN_PREFERENCES_DIALOG_H */
