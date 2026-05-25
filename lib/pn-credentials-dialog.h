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

#ifndef PN_CREDENTIALS_DIALOG_H
#define PN_CREDENTIALS_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_CREDENTIALS_DIALOG (pn_credentials_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PnCredentialsDialog,
                      pn_credentials_dialog,
                      PN, CREDENTIALS_DIALOG,
                      GtkWindow)

/**
 * pn_credentials_dialog_present:
 * @parent: (nullable): a window to anchor the dialog against.
 * @select_type_id: (nullable): a profile type id to show first (e.g.
 *   "mqtt-broker" when opened from a node's broker-profile picker), or %NULL.
 *
 * Shows the credentials manager — the editor for the host vault's provisioned
 * profiles (#PnVault) — and brings it to the front.  Unlike the per-document
 * settings dialog this is process-wide (the vault is a singleton), so there is
 * at most one instance; calling again just raises it and, if @select_type_id
 * is set, switches to that type's page.
 *
 * Returns: (transfer none): the live dialog window, so a caller (the node
 *   dialog's profile picker) can connect to its ::destroy to refresh.
 */
GtkWidget *pn_credentials_dialog_present (GtkWindow   *parent,
                                          const gchar *select_type_id);

G_END_DECLS

#endif /* PN_CREDENTIALS_DIALOG_H */
