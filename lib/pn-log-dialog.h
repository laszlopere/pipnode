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

#ifndef PN_LOG_DIALOG_H
#define PN_LOG_DIALOG_H

#include <gtk/gtk.h>

#include "pn-node.h"

G_BEGIN_DECLS

#define PN_TYPE_LOG_DIALOG (pn_log_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PnLogDialog, pn_log_dialog, PN, LOG_DIALOG, GtkWindow)

/**
 * pn_log_dialog_present:
 * @parent: (nullable): a window to anchor the dialog against (transient
 *   parent + destroy-with-parent owner).
 * @node: the node whose log is shown.  The dialog displays the node's
 *   in-memory log ring (see pn_node_get_log) and refreshes live as the
 *   node logs more, following #PnNode::log-changed.
 *
 * Shows the per-node Log dialog and brings it to the front.  The dialog
 * is non-modal and bound to one #PnNode; there is at most one instance
 * per node, so calling this again for the same @node just raises the
 * existing window.
 */
void pn_log_dialog_present (GtkWindow *parent, PnNode *node);

G_END_DECLS

#endif /* PN_LOG_DIALOG_H */
