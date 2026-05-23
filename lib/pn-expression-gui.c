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
/*  PnExpression — gui tier.                                           */
/*                                                                     */
/*  The settings-dialog customisation for the Expression node.  The    */
/*  node's GType, properties and receive()/evaluation logic live in    */
/*  the GTK-free core file pn-expression.c; this file installs the     */
/*  build_class_tab vfunc onto that class at editor startup            */
/*  (pn_expression_gui_install).  The headless runtime never loads     */
/*  this half, so the expression evaluation runs without GTK.          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-expression-gui.h"
#include "pn-expression.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog tab                                                */
/* ------------------------------------------------------------------ */

/** Replace the auto-generated property tab with a single full-width
 *  editor.  The node has one property and the tab is already titled
 *  "Expression", so the per-row "Expression" label the default tab
 *  would add is redundant — drop it and let the editor fill the tab. */
static GtkWidget *
pn_expression_build_class_tab (
        PnNode    *node,
        GtkWindow *dialog_parent)
{
    GtkWidget  *grid = pn_node_dialog_new_property_grid ();
    GtkWidget  *editor;
    GParamSpec *pspec;

    (void) dialog_parent;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node),
                                          "expression");
    editor = pn_node_dialog_default_editor (G_OBJECT (node), pspec);
    gtk_widget_set_hexpand (editor, TRUE);
    gtk_widget_set_vexpand (editor, TRUE);

    /* Span both columns so the editor uses the full width the
     * "label : editor" grid would otherwise reserve for the label. */
    gtk_grid_attach (GTK_GRID (grid), editor, 0, 0, 2, 1);

    return grid;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_expression_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_EXPRESSION));

    node_class->build_class_tab = pn_expression_build_class_tab;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
