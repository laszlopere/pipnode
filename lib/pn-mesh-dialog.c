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
/*  PnMeshDialog — Phase 1 skeleton.                                   */
/*                                                                     */
/*  Transient dialog hosting the Meshtastic configuration UX described */
/*  in TODO #29.  This file ships only the layout shell: an HPaned     */
/*  with the device list widget on the left and a placeholder right    */
/*  pane (a GtkStack waiting for the per-stage pages added in Phases   */
/*  2-7) plus a status bar.  Singletonised by parent: opening from     */
/*  Devices -> Meshtastic twice raises the existing dialog instead of  */
/*  creating a second one.                                             */
/*                                                                     */
/*  No protocol code lives here.  The dialog will own a per-device     */
/*  PnMeshConnection once Phase 2 introduces it; today the device      */
/*  list emits a "device-activated" signal that this file ignores.    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-dialog.h"
#include "pn-mesh-device-list.h"

/* Initial window size.  Wide enough that the left list + a right pane
 * with two columns of controls reads as one window rather than two
 * cramped halves; tall enough for the Channels page's row list and
 * the Test page's monitor log without scrolling on day one. */
#define PN_MESH_DIALOG_WIDTH   880
#define PN_MESH_DIALOG_HEIGHT  560

/* Initial divider position.  ~250 px gives the device list room for
 * "Heltec V3 — /dev/ttyUSB0" plus a status badge without truncation,
 * leaving the bulk of the dialog for the per-device settings. */
#define PN_MESH_DIALOG_DIVIDER 250

/* One live dialog per parent window.  A second present() call raises
 * the same instance rather than opening a duplicate; the qdata key
 * holds a weak-cleared pointer back to it. */
#define PN_MESH_DIALOG_QDATA "pn-mesh-dialog"

static GtkWidget *
build_status_bar (void)
{
    GtkWidget *bar = gtk_label_new (NULL);

    /* Plain left-aligned status line at the bottom of the dialog.  Set
     * by the connection code in later phases ("Scanning…", "Connected
     * to /dev/ttyUSB0", "Disconnected", "Error: …"); for Phase 1 it
     * just tells the user the dialog is alive. */
    gtk_label_set_xalign (GTK_LABEL (bar), 0.0);
    gtk_widget_set_margin_start  (bar, 6);
    gtk_widget_set_margin_end    (bar, 6);
    gtk_widget_set_margin_top    (bar, 2);
    gtk_widget_set_margin_bottom (bar, 4);
    gtk_label_set_text (GTK_LABEL (bar),
                        "Ready. Press Scan to look for connected devices.");
    return bar;
}

/* Right pane: a GtkStack that will hold one page per configuration
 * stage in later phases (Identity / Region / Channels / Share / Test).
 * Phase 1 ships a single "no device" page so the right side is not
 * blank when the dialog opens. */
static GtkWidget *
build_right_pane (void)
{
    GtkWidget *stack;
    GtkWidget *empty;
    GtkWidget *empty_label;

    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    /* Empty-state pane: centred prompt nudging the user toward Scan.
     * Later phases swap this out for the per-stage pages when a device
     * is connected. */
    empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (empty, GTK_ALIGN_CENTER);

    empty_label = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (empty_label),
            "<span size='large' weight='bold'>No device connected</span>");
    gtk_box_pack_start (GTK_BOX (empty), empty_label, FALSE, FALSE, 0);

    empty_label = gtk_label_new (
            "Press Scan on the left to look for connected\n"
            "Meshtastic devices, then pick one to configure it.");
    gtk_label_set_justify (GTK_LABEL (empty_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start (GTK_BOX (empty), empty_label, FALSE, FALSE, 0);

    gtk_stack_add_named (GTK_STACK (stack), empty, "empty");
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "empty");
    return stack;
}

/* The dialog itself.  GtkDialog (not GtkWindow) so the Close button
 * gets standard placement and Escape closes naturally; we never call
 * gtk_dialog_run() on it -- it lives modelessly so the user can flip
 * back to the worksheet to consult a node's properties while a setup
 * step is open. */
static GtkWidget *
build_dialog (GtkWindow *parent)
{
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *paned;
    GtkWidget *left;
    GtkWidget *right;
    GtkWidget *status;

    dialog = gtk_dialog_new_with_buttons (
            "Meshtastic Devices",
            parent,
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE,
            NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog),
                                 PN_MESH_DIALOG_WIDTH,
                                 PN_MESH_DIALOG_HEIGHT);
    /* Modeless: the user keeps access to the worksheet behind the
     * dialog (helpful when comparing what a node expects against
     * what the device is set to). */
    gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);

    /* Standard GtkDialog response: Close button (and Escape via the
     * "close" accelerator) tears the window down. */
    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content), 0);

    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), PN_MESH_DIALOG_DIVIDER);
    gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);

    left  = pn_mesh_device_list_new ();
    right = build_right_pane ();
    gtk_paned_pack1 (GTK_PANED (paned), left,  FALSE, FALSE);
    gtk_paned_pack2 (GTK_PANED (paned), right, TRUE,  FALSE);

    gtk_box_pack_start (GTK_BOX (content), paned, TRUE, TRUE, 0);

    /* Thin separator above the status bar so it reads as a footer
     * rather than floating loose under the panes. */
    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 0);
    status = build_status_bar ();
    gtk_box_pack_start (GTK_BOX (content), status, FALSE, FALSE, 0);

    gtk_widget_show_all (content);
    return dialog;
}

/* Drop the qdata link when the singleton dialog goes away, so the next
 * present() builds a fresh one rather than handing back a destroyed
 * pointer. */
static void
on_dialog_destroyed (gpointer parent, GObject *was_dialog)
{
    (void) was_dialog;
    g_object_set_data (G_OBJECT (parent), PN_MESH_DIALOG_QDATA, NULL);
}

void
pn_mesh_dialog_present (GtkWindow *parent_window)
{
    GtkWidget *dialog;

    g_return_if_fail (parent_window == NULL || GTK_IS_WINDOW (parent_window));

    if (parent_window != NULL)
    {
        dialog = g_object_get_data (G_OBJECT (parent_window),
                                    PN_MESH_DIALOG_QDATA);
        if (dialog != NULL)
        {
            gtk_window_present (GTK_WINDOW (dialog));
            return;
        }
    }

    dialog = build_dialog (parent_window);

    if (parent_window != NULL)
    {
        g_object_set_data (G_OBJECT (parent_window),
                           PN_MESH_DIALOG_QDATA, dialog);
        g_object_weak_ref (G_OBJECT (dialog),
                           (GWeakNotify) on_dialog_destroyed,
                           parent_window);
    }

    gtk_window_present (GTK_WINDOW (dialog));
}
