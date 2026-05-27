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
/*  Mesh device list — left pane of the Meshtastic dialog (Phase 1).   */
/*                                                                     */
/*  Layout: a Scan button at the top, an empty-state hint in the       */
/*  middle, and a GtkListBox underneath that fills with one row per    */
/*  discovered device when Scan is pressed.  Empty until the user      */
/*  asks -- pipnode does not auto-discover, because USB scans can be   */
/*  slow and, per the user, we want them to be a deliberate act.       */
/*                                                                     */
/*  Phase 1 stubs the Scan worker to a single fake row so the layout   */
/*  is judgable end-to-end before the real sysfs / VID:PID walk lands  */
/*  in Phase 2 (pn-mesh-discover.c).  The row template, selection      */
/*  semantics and empty/full toggling are real -- only the data        */
/*  source is fake.                                                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-device-list.h"

/* One device row.  Two-line layout: top line is the human-friendly
 * device name (Phase 2: pulled from the VID:PID table; Phase 1: a
 * placeholder), bottom line the tty path.  This is the same shape
 * pip-mesh --list-devices --long prints to the terminal. */
static GtkWidget *
build_device_row (const gchar *name, const gchar *path)
{
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *name_label;
    GtkWidget *path_label;

    row = gtk_list_box_row_new ();

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start  (box, 8);
    gtk_widget_set_margin_end    (box, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    name_label = gtk_label_new (name);
    gtk_label_set_xalign (GTK_LABEL (name_label), 0.0);
    /* Bold by Pango attribute rather than CSS so it works on every
     * theme the editor might run under. */
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs,
                                pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (name_label), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_box_pack_start (GTK_BOX (box), name_label, FALSE, FALSE, 0);

    path_label = gtk_label_new (path);
    gtk_label_set_xalign (GTK_LABEL (path_label), 0.0);
    /* Dim the path so the name reads as the primary identifier and
     * the device file as supporting detail.  Same pattern Files /
     * Nautilus uses for subtitles. */
    {
        GtkStyleContext *ctx = gtk_widget_get_style_context (path_label);
        gtk_style_context_add_class (ctx, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (box), path_label, FALSE, FALSE, 0);

    gtk_container_add (GTK_CONTAINER (row), box);
    gtk_widget_show_all (row);
    return row;
}

/* Wipe every row out of the list -- called at the start of each scan
 * so the result replaces the previous one rather than accumulating
 * (we are not allowed to imply "remembered" devices). */
static void
clear_list (GtkListBox *list)
{
    GList *rows;
    GList *l;

    rows = gtk_container_get_children (GTK_CONTAINER (list));
    for (l = rows; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (rows);
}

/* Show the empty-state hint when the list is empty and hide it when
 * the list has rows.  The two widgets share their stack so the right
 * one is shown automatically. */
static void
update_empty_state (GtkStack *stack, GtkListBox *list)
{
    gboolean empty = (gtk_container_get_children (GTK_CONTAINER (list)) == NULL);
    gtk_stack_set_visible_child_name (stack, empty ? "empty" : "list");
}

typedef struct
{
    GtkStack   *stack;
    GtkListBox *list;
} ScanCtx;

static void
scan_ctx_free (gpointer data, GClosure *closure)
{
    (void) closure;
    g_slice_free (ScanCtx, data);
}

/* Scan button handler.
 *
 * Phase 1: STUB -- drops one canned row in so the dialog has
 * something to react to without touching real hardware.  Phase 2
 * replaces the body with a GTask-wrapped discovery worker that calls
 * pn-mesh-discover, then marshalls the resulting list of {name,path}
 * back here and populates the list.  The clear-then-populate dance,
 * the empty-state toggle and the selection wiring all stay -- only
 * the row source changes. */
static void
on_scan_clicked (GtkButton *button, gpointer user_data)
{
    ScanCtx *ctx = user_data;

    (void) button;

    clear_list (ctx->list);

    /* Stubbed row matching what pn-mesh-discover will produce for a
     * Heltec V3 once it can walk /sys/bus/usb/devices. */
    gtk_container_add (GTK_CONTAINER (ctx->list),
                       build_device_row ("Heltec V3 (stub)",
                                         "/dev/ttyUSB0"));

    update_empty_state (ctx->stack, ctx->list);
}

GtkWidget *
pn_mesh_device_list_new (void)
{
    GtkWidget *root;
    GtkWidget *scan_button;
    GtkWidget *scan_box;
    GtkWidget *stack;
    GtkWidget *list_window;
    GtkWidget *list;
    GtkWidget *empty;
    GtkWidget *empty_icon;
    GtkWidget *empty_label;
    ScanCtx   *ctx;

    root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (root, 6);
    gtk_widget_set_margin_end    (root, 6);
    gtk_widget_set_margin_top    (root, 6);
    gtk_widget_set_margin_bottom (root, 6);

    /* Top: Scan button.  Wrapped in a box so future siblings (e.g.
     * "Connect by address…" for the manual / no-USB case) can be
     * added without resizing the button.  view-refresh is the
     * standard "rescan / look again" glyph. */
    scan_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    scan_button = gtk_button_new_from_icon_name ("view-refresh",
                                                 GTK_ICON_SIZE_BUTTON);
    gtk_button_set_label (GTK_BUTTON (scan_button), "Scan");
    gtk_button_set_always_show_image (GTK_BUTTON (scan_button), TRUE);
    gtk_widget_set_tooltip_text (
            scan_button,
            "Look for connected Meshtastic devices on USB.");
    gtk_box_pack_start (GTK_BOX (scan_box), scan_button, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (root), scan_box, FALSE, FALSE, 0);

    /* Middle: stack between an empty-state hint and the live list.
     * The list is built and kept around even when empty so its
     * children can be queried without a NULL guard. */
    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand (stack, TRUE);

    empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (empty, GTK_ALIGN_CENTER);
    /* A muted icon + line of prose so the empty pane reads as
     * "do this next" rather than "broken". */
    empty_icon = gtk_image_new_from_icon_name ("network-wireless-disabled",
                                               GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_opacity (empty_icon, 0.4);
    gtk_box_pack_start (GTK_BOX (empty), empty_icon, FALSE, FALSE, 0);
    empty_label = gtk_label_new ("No devices yet.\nPress Scan to look.");
    gtk_label_set_justify (GTK_LABEL (empty_label), GTK_JUSTIFY_CENTER);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (empty_label);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (empty), empty_label, FALSE, FALSE, 0);
    gtk_stack_add_named (GTK_STACK (stack), empty, "empty");

    /* Scrolled list pane: a single-selection ListBox so picking a row
     * is the natural "this is the device I want to set up" gesture. */
    list_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_window),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_SINGLE);
    gtk_container_add (GTK_CONTAINER (list_window), list);
    gtk_stack_add_named (GTK_STACK (stack), list_window, "list");

    gtk_stack_set_visible_child_name (GTK_STACK (stack), "empty");
    gtk_box_pack_start (GTK_BOX (root), stack, TRUE, TRUE, 0);

    /* Wire Scan -> populate.  The context is owned by the closure so
     * it dies with the button. */
    ctx = g_slice_new0 (ScanCtx);
    ctx->stack = GTK_STACK (stack);
    ctx->list  = GTK_LIST_BOX (list);
    g_signal_connect_data (scan_button, "clicked",
                           G_CALLBACK (on_scan_clicked),
                           ctx, scan_ctx_free, 0);

    return root;
}
