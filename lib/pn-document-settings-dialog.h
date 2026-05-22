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

#ifndef PN_DOCUMENT_SETTINGS_DIALOG_H
#define PN_DOCUMENT_SETTINGS_DIALOG_H

#include <gtk/gtk.h>
#include "pn-flow.h"

G_BEGIN_DECLS

#define PN_TYPE_DOCUMENT_SETTINGS_DIALOG (pn_document_settings_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PnDocumentSettingsDialog,
                      pn_document_settings_dialog,
                      PN, DOCUMENT_SETTINGS_DIALOG,
                      GtkWindow)

/**
 * pn_document_settings_dialog_present:
 * @parent: (nullable): a window to anchor the dialog against (transient
 *   parent, position hint, and destroy-with-parent owner).
 * @flow: the document whose settings are edited.  Edits write straight
 *   through to @flow and mark it modified, so they persist the next time
 *   the file is saved.
 *
 * Shows the document-settings dialog for @flow and brings it to the
 * front.  The dialog is non-modal and bound to one #PnFlow; there is at
 * most one instance per flow, so calling this again for the same @flow
 * just raises the existing window.
 */
void pn_document_settings_dialog_present (GtkWindow *parent, PnFlow *flow);

G_END_DECLS

#endif /* PN_DOCUMENT_SETTINGS_DIALOG_H */
