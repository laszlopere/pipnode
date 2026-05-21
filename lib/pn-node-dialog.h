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

#ifndef PN_NODE_DIALOG_H
#define PN_NODE_DIALOG_H

#include <gtk/gtk.h>

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNodeDialog                                                       */
/*                                                                     */
/*  Modal property editor for a single #PnNode.  The dialog            */
/*  introspects the GObject type chain of the node and creates one    */
/*  notebook tab per class that introduces properties, so a derived   */
/*  type's tab shows only its own additions while the base #PnNode    */
/*  attributes (color, name, position, ports, …) live on a shared tab.*/
/*                                                                     */
/*  Editor widgets are bound bidirectionally to the node, so changes  */
/*  take effect immediately and the worksheet repaints as the user    */
/*  edits.                                                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NODE_DIALOG (pn_node_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PnNodeDialog, pn_node_dialog,
                      PN, NODE_DIALOG, GtkDialog)

/**
 * pn_node_dialog_new:
 * @parent: (nullable): transient-parent window, or %NULL
 * @node:   node to edit; the dialog holds a strong reference for its
 *          lifetime
 *
 * Returns: (transfer floating): the new dialog widget
 */
GtkWidget *pn_node_dialog_new (GtkWindow *parent, PnNode *node);

G_END_DECLS

#endif /* PN_NODE_DIALOG_H */
