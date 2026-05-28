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
/*  Mesh device list — left pane of the Meshtastic dialog.             */
/*                                                                     */
/*  Layout: Scan button on top, a stack of three states underneath:    */
/*  pre-scan hint, post-scan "no devices" hint, and the real list.     */
/*  Scan runs pn_mesh_discover_async() on a worker thread; results     */
/*  arrive on the main thread, which replaces (not appends) the list   */
/*  rows.  A pending scan is cancelled when the widget is destroyed    */
/*  so a closing dialog does not race with the worker thread.          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-device-list.h"
#include "pn-mesh-discover.h"

/* Each row carries a PnMeshDevice* under this qdata key so Phase 2d
 * can pick it up when the user activates a row -- the connection
 * code needs the tty path (and the human-friendly kind for the
 * status bar). */
#define PN_MESH_ROW_DEVICE_QDATA "pn-mesh-device"

typedef struct
{
    GtkWidget    *root;          /* the vbox returned to the caller        */
    GtkStack     *stack;
    GtkListBox   *list;
    GtkWidget    *reload_menu;   /* right-click context menu, ctx-owned    */
    GCancellable *cancellable;   /* cancels an in-flight scan on teardown  */
    gboolean      scanning;

    /* TRUE only for the initial auto-scan kicked off at construction;
     * consumed in on_scan_done to auto-activate the first device row.
     * Manual rescans (user-clicked) leave the current selection alone. */
    gboolean      auto_select_pending;

    /* tty path of the most recently activated device, or NULL when
     * nothing has been activated yet.  Lets a rescan re-select the same
     * device (so the right pane refreshes instead of going stale). */
    gchar        *activated_tty;

    /* Optional consumer wired in by the dialog to learn about row
     * activations.  Set via pn_mesh_device_list_set_activated_callback. */
    PnMeshDeviceActivatedFunc activated_cb;
    gpointer                  activated_user_data;
} DeviceListCtx;

static void on_row_activated_thunk (GtkListBox    *box,
                                    GtkListBoxRow *row,
                                    gpointer       user_data);

static gboolean idle_start_initial_scan (gpointer user_data);

static void
device_list_ctx_free (gpointer data)
{
    DeviceListCtx *ctx = data;

    if (ctx->cancellable != NULL)
    {
        g_cancellable_cancel (ctx->cancellable);
        g_clear_object (&ctx->cancellable);
    }
    g_free (ctx->activated_tty);
    g_slice_free (DeviceListCtx, ctx);
}

/* ------------------------------------------------------------------ */
/*  Row template                                                        */
/* ------------------------------------------------------------------ */

/* One device row.  Two-line layout:
 *   primary  — the human-friendly device family ("Heltec V3")
 *   subtitle — the tty path, dim-styled
 * The PnMeshDevice* is held as qdata so the dialog can act on the
 * selected row in Phase 2d. */
static GtkWidget *
build_device_row (PnMeshDevice *device)
{
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *primary;
    GtkWidget *subtitle;

    row = gtk_list_box_row_new ();

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start  (box, 8);
    gtk_widget_set_margin_end    (box, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    primary = gtk_label_new (device->kind);
    gtk_label_set_xalign (GTK_LABEL (primary), 0.0);
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs,
                                pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (primary), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_box_pack_start (GTK_BOX (box), primary, FALSE, FALSE, 0);

    subtitle = gtk_label_new (device->tty);
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (box), subtitle, FALSE, FALSE, 0);

    gtk_container_add (GTK_CONTAINER (row), box);

    /* Hand the row our own copy of the descriptor; row-destroy frees it. */
    g_object_set_data_full (G_OBJECT (row),
                            PN_MESH_ROW_DEVICE_QDATA,
                            pn_mesh_device_copy (device),
                            (GDestroyNotify) pn_mesh_device_free);

    gtk_widget_show_all (row);
    return row;
}

/* ------------------------------------------------------------------ */
/*  List management                                                     */
/* ------------------------------------------------------------------ */

static void
clear_list (GtkListBox *list)
{
    GList *rows = gtk_container_get_children (GTK_CONTAINER (list));
    GList *l;

    for (l = rows; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (rows);
}

/* Pick the right empty state to show: before any scan we nudge the
 * user toward Scan ("pre-scan"); after a scan that returned nothing
 * we tell them so explicitly ("empty-result"), so a silent zero-row
 * scan does not look like a broken Scan button. */
static void
show_state (DeviceListCtx *ctx, const char *name)
{
    gtk_stack_set_visible_child_name (ctx->stack, name);
}

/* ------------------------------------------------------------------ */
/*  Scan flow                                                           */
/* ------------------------------------------------------------------ */

static void
set_scanning (DeviceListCtx *ctx, gboolean scanning)
{
    ctx->scanning = scanning;
}

static void
on_scan_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    DeviceListCtx *ctx = user_data;
    GPtrArray     *devices;
    GError        *error = NULL;
    guint          i;

    (void) source;

    devices = pn_mesh_discover_finish (result, &error);

    /* Cancellation: the widget was torn down while we were scanning
     * (the cancellable was cancelled in device_list_ctx_free).  The
     * ctx is gone in that case, so we cannot touch the UI here.  But
     * because the cancellable is the very ctx that holds @ctx alive
     * through the GTask's user_data we are safe to inspect ctx itself
     * once -- it is only destroyed when the closure dies, after this
     * callback returns. */
    if (error != NULL)
    {
        g_warning ("pn-mesh-discover: %s", error->message);
        g_clear_error (&error);
        if (devices != NULL)
            g_ptr_array_unref (devices);
        set_scanning (ctx, FALSE);
        return;
    }

    clear_list (ctx->list);
    for (i = 0; i < devices->len; i++)
        gtk_container_add (GTK_CONTAINER (ctx->list),
                           build_device_row (g_ptr_array_index (devices, i)));
    g_ptr_array_unref (devices);

    show_state (ctx,
                gtk_container_get_children (GTK_CONTAINER (ctx->list)) == NULL
                ? "empty-result" : "list");

    set_scanning (ctx, FALSE);

    /* Decide which row (if any) to re-activate after the rescan:
     *   1. The same device the user had open before, when still present
     *      -- so the right pane refreshes instead of going stale.
     *   2. Otherwise the first row, but only on the initial auto-scan
     *      or when we previously had something activated (preferring a
     *      fresh selection over a now-orphaned right pane). */
    {
        GtkListBoxRow *target = NULL;

        if (ctx->activated_tty != NULL)
        {
            GList *rows = gtk_container_get_children (
                    GTK_CONTAINER (ctx->list));
            for (GList *l = rows; l != NULL; l = l->next)
            {
                PnMeshDevice *d = g_object_get_data (
                        G_OBJECT (l->data), PN_MESH_ROW_DEVICE_QDATA);
                if (d != NULL &&
                    g_strcmp0 (d->tty, ctx->activated_tty) == 0)
                {
                    target = GTK_LIST_BOX_ROW (l->data);
                    break;
                }
            }
            g_list_free (rows);
        }

        if (target == NULL &&
            (ctx->auto_select_pending || ctx->activated_tty != NULL))
            target = gtk_list_box_get_row_at_index (ctx->list, 0);

        ctx->auto_select_pending = FALSE;

        if (target != NULL)
        {
            PnMeshDevice *device =
                    g_object_get_data (G_OBJECT (target),
                                       PN_MESH_ROW_DEVICE_QDATA);
            gtk_list_box_select_row (ctx->list, target);
            if (device != NULL && ctx->activated_cb != NULL)
            {
                g_free (ctx->activated_tty);
                ctx->activated_tty = g_strdup (device->tty);
                ctx->activated_cb (device, ctx->activated_user_data);
            }
        }
    }
}

static void
on_scan_clicked (GtkButton *button, gpointer user_data)
{
    DeviceListCtx *ctx = user_data;

    (void) button;

    if (ctx->scanning)
        return;

    set_scanning (ctx, TRUE);

    /* A fresh cancellable per scan: if the previous scan was cancelled
     * (rare, the widget would normally be gone too) we do not want to
     * inherit a cancelled token here. */
    g_clear_object (&ctx->cancellable);
    ctx->cancellable = g_cancellable_new ();

    pn_mesh_discover_async (ctx->cancellable, on_scan_done, ctx);
}

/* One-shot idle: fire the same code path as a user click so the dialog
 * lands on a scanning state immediately after opening. */
static gboolean
idle_start_initial_scan (gpointer user_data)
{
    DeviceListCtx *ctx = user_data;

    if (!ctx->scanning)
        on_scan_clicked (NULL, ctx);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Right-click context menu                                            */
/* ------------------------------------------------------------------ */

static void
on_reload_activate (GtkMenuItem *item, gpointer user_data)
{
    (void) item;
    on_scan_clicked (NULL, user_data);
}

static GtkWidget *
build_reload_menu (DeviceListCtx *ctx)
{
    GtkWidget *menu  = gtk_menu_new ();
    GtkWidget *item  = gtk_menu_item_new ();
    GtkWidget *row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon  = gtk_image_new_from_icon_name ("view-refresh",
                                                     GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new ("Reload");

    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (row), icon,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), label, TRUE,  TRUE,  0);
    gtk_container_add  (GTK_CONTAINER (item), row);

    g_signal_connect (item, "activate",
                      G_CALLBACK (on_reload_activate), ctx);
    gtk_widget_show_all (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    /* Attach to the root widget so the menu is destroyed alongside it. */
    gtk_menu_attach_to_widget (GTK_MENU (menu), ctx->root, NULL);
    return menu;
}

static gboolean
on_list_button_press (GtkWidget      *widget,
                      GdkEventButton *event,
                      gpointer        user_data)
{
    DeviceListCtx *ctx = user_data;

    (void) widget;
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY)
        return GDK_EVENT_PROPAGATE;

    if (ctx->reload_menu == NULL)
        ctx->reload_menu = build_reload_menu (ctx);

    gtk_menu_popup_at_pointer (GTK_MENU (ctx->reload_menu),
                               (const GdkEvent *) event);
    return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

/* Build one of the two empty-state pages used in the stack. */
static GtkWidget *
build_empty_page (const char *icon_name, const char *primary,
                  const char *secondary)
{
    GtkWidget *box;
    GtkWidget *icon;
    GtkWidget *primary_label;
    GtkWidget *secondary_label;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

    icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_opacity (icon, 0.4);
    gtk_box_pack_start (GTK_BOX (box), icon, FALSE, FALSE, 0);

    primary_label = gtk_label_new (primary);
    gtk_label_set_justify (GTK_LABEL (primary_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start (GTK_BOX (box), primary_label, FALSE, FALSE, 0);

    if (secondary != NULL)
    {
        secondary_label = gtk_label_new (secondary);
        gtk_label_set_justify (GTK_LABEL (secondary_label), GTK_JUSTIFY_CENTER);
        {
            GtkStyleContext *sc = gtk_widget_get_style_context (secondary_label);
            gtk_style_context_add_class (sc, "dim-label");
        }
        gtk_box_pack_start (GTK_BOX (box), secondary_label, FALSE, FALSE, 0);
    }

    return box;
}

GtkWidget *
pn_mesh_device_list_new (void)
{
    DeviceListCtx *ctx;
    GtkWidget     *root;
    GtkWidget     *event_box;
    GtkWidget     *stack;
    GtkWidget     *pre;
    GtkWidget     *no_result;
    GtkWidget     *list_window;
    GtkWidget     *list;

    root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (root, 6);
    gtk_widget_set_margin_end    (root, 6);
    gtk_widget_set_margin_top    (root, 6);
    gtk_widget_set_margin_bottom (root, 6);

    /* Empty-state pages and the real list, swapped via stack. */
    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand (stack, TRUE);

    pre = build_empty_page ("network-wireless-disabled",
                            "Looking for devices…",
                            "Right-click to reload.");
    gtk_stack_add_named (GTK_STACK (stack), pre, "pre-scan");

    /* Distinct empty state for "we looked and found nothing", so a
     * zero-result scan does not look like a broken reload.  The
     * dialog-question icon reads as "did you mean to plug something
     * in?" rather than the wireless-disabled "scan blocked" feel. */
    no_result = build_empty_page ("dialog-question",
                                  "No Meshtastic devices found.",
                                  "Plug one in and right-click → Reload.");
    gtk_stack_add_named (GTK_STACK (stack), no_result, "empty-result");

    list_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_window),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_SINGLE);
    gtk_container_add (GTK_CONTAINER (list_window), list);
    gtk_stack_add_named (GTK_STACK (stack), list_window, "list");

    gtk_stack_set_visible_child_name (GTK_STACK (stack), "pre-scan");

    /* Wrap the stack in an event box so right-click works regardless of
     * which page is showing (pre-scan / empty-result are non-windowed
     * GtkBoxes that would not receive button events on their own). */
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    gtk_container_add (GTK_CONTAINER (event_box), stack);
    gtk_box_pack_start (GTK_BOX (root), event_box, TRUE, TRUE, 0);

    /* Context lives on the root widget so it is freed exactly when
     * the widget is destroyed, taking the cancellable (and any
     * in-flight scan) with it. */
    ctx = g_slice_new0 (DeviceListCtx);
    ctx->root        = root;
    ctx->stack       = GTK_STACK (stack);
    ctx->list        = GTK_LIST_BOX (list);
    ctx->cancellable = NULL;
    ctx->scanning    = FALSE;

    g_object_set_data_full (G_OBJECT (root), "pn-mesh-device-list-ctx",
                            ctx, device_list_ctx_free);

    g_signal_connect (list, "row-activated",
                      G_CALLBACK (on_row_activated_thunk), ctx);
    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_list_button_press), ctx);
    g_signal_connect (list, "button-press-event",
                      G_CALLBACK (on_list_button_press), ctx);

    /* Kick off the initial scan once the dialog has finished
     * constructing (so the dialog has had a chance to wire its
     * activated callback).  The g_idle defers to the next main-loop
     * iteration, by which time pn_mesh_device_list_set_activated_callback
     * has been called. */
    ctx->auto_select_pending = TRUE;
    g_idle_add (idle_start_initial_scan, ctx);

    return root;
}

/* Forward selected-row activation (double-click / Enter) to the
 * dialog's handler, if one is wired. */
static void
on_row_activated_thunk (GtkListBox    *box,
                        GtkListBoxRow *row,
                        gpointer       user_data)
{
    DeviceListCtx *ctx    = user_data;
    PnMeshDevice  *device;

    (void) box;
    if (ctx->activated_cb == NULL || row == NULL)
        return;
    device = g_object_get_data (G_OBJECT (row), PN_MESH_ROW_DEVICE_QDATA);
    if (device != NULL)
    {
        g_free (ctx->activated_tty);
        ctx->activated_tty = g_strdup (device->tty);
        ctx->activated_cb (device, ctx->activated_user_data);
    }
}

void
pn_mesh_device_list_set_activated_callback (
        GtkWidget                  *list_widget,
        PnMeshDeviceActivatedFunc   callback,
        gpointer                    user_data)
{
    DeviceListCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (list_widget));

    ctx = g_object_get_data (G_OBJECT (list_widget),
                             "pn-mesh-device-list-ctx");
    g_return_if_fail (ctx != NULL);

    ctx->activated_cb        = callback;
    ctx->activated_user_data = user_data;
}
