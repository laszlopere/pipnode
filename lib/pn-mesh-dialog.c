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
/*  PnMeshDialog — orchestrator for the Meshtastic configuration UI.   */
/*                                                                     */
/*  HPaned holding the device list on the left and a per-stage stack   */
/*  on the right (Identity today; Region / Channels / Share / Test     */
/*  added in later phases).  Owns at most one live PnMeshConnection    */
/*  -- picking a different device closes the previous session and      */
/*  opens a fresh one.  Connection open runs on a GTask worker         */
/*  thread; the dialog stays responsive while the 3 s handshake        */
/*  budget elapses and shows progress in the status bar.  Closing      */
/*  the dialog cancels any in-flight open and drops the live session.  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-dialog.h"
#include "pn-mesh-connection.h"
#include "pn-mesh-device-list.h"
#include "pn-mesh-discover.h"
#include "pn-mesh-page-identity.h"

#define PN_MESH_DIALOG_WIDTH   880
#define PN_MESH_DIALOG_HEIGHT  560
#define PN_MESH_DIALOG_DIVIDER 250
#define PN_MESH_DIALOG_QDATA   "pn-mesh-dialog"

typedef struct
{
    GtkWidget        *dialog;
    GtkWidget        *device_list;
    GtkStack         *right_stack;
    GtkWidget        *identity_page;
    GtkLabel         *status_label;

    /* Active session, if any.  NULL when no device is selected or
     * while a fresh open is in flight. */
    PnMeshConnection *connection;

    /* Kind / tty of the device the live connection is for, owned so
     * the Identity page can be re-painted on subsequent state pushes
     * without re-asking the list. */
    gchar            *connection_kind;
    gchar            *connection_tty;

    /* Cancels an in-flight open.  Replaced (not reset) on every new
     * activation so a still-pending old open is dropped on the floor
     * without disturbing the new one. */
    GCancellable     *open_cancellable;
} MeshDialogCtx;

/* ------------------------------------------------------------------ */
/*  Status bar                                                          */
/* ------------------------------------------------------------------ */

static void
set_status (MeshDialogCtx *ctx, const gchar *text)
{
    gtk_label_set_text (ctx->status_label, text);
}

static void
set_statusf (MeshDialogCtx *ctx, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
set_statusf (MeshDialogCtx *ctx, const gchar *fmt, ...)
{
    va_list  ap;
    gchar   *text;

    va_start (ap, fmt);
    text = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    set_status (ctx, text);
    g_free (text);
}

/* ------------------------------------------------------------------ */
/*  Connection lifecycle                                                */
/* ------------------------------------------------------------------ */

/* Tear down the live session (if any) and reset the right pane to
 * the empty stack page.  Used on device switch and on dialog close. */
static void
drop_connection (MeshDialogCtx *ctx)
{
    if (ctx->connection != NULL)
    {
        pn_mesh_connection_close (ctx->connection);
        ctx->connection = NULL;
    }
    g_clear_pointer (&ctx->connection_kind, g_free);
    g_clear_pointer (&ctx->connection_tty,  g_free);
    pn_mesh_page_identity_set_state (ctx->identity_page, NULL, NULL, NULL);
}

static void
on_connection_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
    MeshDialogCtx    *ctx   = user_data;
    GError           *error = NULL;
    PnMeshConnection *conn;

    (void) source;

    conn = pn_mesh_connection_open_finish (res, &error);

    /* The user may have closed the dialog or activated a different
     * device while we were waiting.  The cancellable carried with the
     * task is the authority; if it was cancelled, drop the connection. */
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error (&error);
        if (conn != NULL)
            pn_mesh_connection_close (conn);
        return;
    }

    if (conn == NULL)
    {
        set_statusf (ctx, "Could not connect to %s: %s",
                     ctx->connection_tty != NULL ? ctx->connection_tty : "(?)",
                     error != NULL ? error->message : "(unknown error)");
        g_clear_error (&error);
        /* Stay on the empty page so the user sees the worksheet area
         * is intentionally blank; their next activation will retry. */
        gtk_stack_set_visible_child_name (ctx->right_stack, "empty");
        return;
    }

    ctx->connection = conn;
    pn_mesh_page_identity_set_state (
            ctx->identity_page,
            ctx->connection_kind,
            ctx->connection_tty,
            pn_mesh_connection_get_state (conn));
    gtk_stack_set_visible_child_name (ctx->right_stack, "identity");

    {
        const PnMeshState *st = pn_mesh_connection_get_state (conn);
        set_statusf (ctx, "Connected to %s (%s%s)",
                     ctx->connection_tty,
                     st->config_complete ? "handshake complete"
                                         : "handshake timed out",
                     st->my_node_num != 0 ? ", state read" : "");
    }
}

/* Device row activated: tear down whatever is live, set up the
 * Identity page placeholder, kick off the async open. */
static void
on_device_activated (const PnMeshDevice *device, gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    /* If this is the same device we are already on (or trying to
     * reach), do nothing -- the user double-clicked by accident. */
    if (ctx->connection_tty != NULL
        && g_strcmp0 (ctx->connection_tty, device->tty) == 0
        && ctx->connection != NULL)
        return;

    /* Cancel any in-flight open and drop any live session so the new
     * one starts from a clean slate. */
    if (ctx->open_cancellable != NULL)
    {
        g_cancellable_cancel (ctx->open_cancellable);
        g_clear_object (&ctx->open_cancellable);
    }
    drop_connection (ctx);

    ctx->connection_kind = g_strdup (device->kind);
    ctx->connection_tty  = g_strdup (device->tty);

    /* Show the Identity page in "connecting" form -- kind and tty are
     * known immediately; the rest of the fields stay as em-dashes
     * until the handshake completes. */
    pn_mesh_page_identity_set_state (ctx->identity_page,
                                     device->kind, device->tty, NULL);
    gtk_stack_set_visible_child_name (ctx->right_stack, "identity");

    set_statusf (ctx, "Connecting to %s on %s…",
                 device->kind, device->tty);

    ctx->open_cancellable = g_cancellable_new ();
    pn_mesh_connection_open_async (device->tty, ctx->open_cancellable,
                                   on_connection_ready, ctx);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_status_bar (MeshDialogCtx *ctx)
{
    GtkWidget *bar = gtk_label_new (
            "Ready. Press Scan to look for connected devices.");

    gtk_label_set_xalign (GTK_LABEL (bar), 0.0);
    gtk_widget_set_margin_start  (bar, 6);
    gtk_widget_set_margin_end    (bar, 6);
    gtk_widget_set_margin_top    (bar, 2);
    gtk_widget_set_margin_bottom (bar, 4);
    /* Long error strings get an ellipsis so they do not stretch the
     * dialog.  The label is selectable so the user can copy any
     * error text out for support. */
    gtk_label_set_ellipsize (GTK_LABEL (bar), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable (GTK_LABEL (bar), TRUE);

    ctx->status_label = GTK_LABEL (bar);
    return bar;
}

/* Empty page for "no device chosen yet". */
static GtkWidget *
build_empty_page (void)
{
    GtkWidget *box;
    GtkWidget *primary;
    GtkWidget *secondary;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

    primary = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (primary),
            "<span size='large' weight='bold'>No device connected</span>");
    gtk_box_pack_start (GTK_BOX (box), primary, FALSE, FALSE, 0);

    secondary = gtk_label_new (
            "Press Scan on the left to look for connected\n"
            "Meshtastic devices, then double-click one to configure it.");
    gtk_label_set_justify (GTK_LABEL (secondary), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start (GTK_BOX (box), secondary, FALSE, FALSE, 0);

    return box;
}

/* The dialog instance.  Modeless so the user keeps access to the
 * worksheet behind it (useful when comparing the device against a
 * node's properties). */
static GtkWidget *
build_dialog (GtkWindow *parent, MeshDialogCtx *ctx)
{
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *paned;
    GtkWidget *stack;
    GtkWidget *identity;

    dialog = gtk_dialog_new_with_buttons (
            "Meshtastic Devices",
            parent,
            GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE,
            NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog),
                                 PN_MESH_DIALOG_WIDTH,
                                 PN_MESH_DIALOG_HEIGHT);
    gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content), 0);

    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), PN_MESH_DIALOG_DIVIDER);
    gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);

    ctx->device_list = pn_mesh_device_list_new ();
    pn_mesh_device_list_set_activated_callback (
            ctx->device_list, on_device_activated, ctx);

    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    ctx->right_stack = GTK_STACK (stack);

    gtk_stack_add_named (GTK_STACK (stack), build_empty_page (), "empty");
    identity = pn_mesh_page_identity_new ();
    gtk_stack_add_named (GTK_STACK (stack), identity, "identity");
    ctx->identity_page = identity;

    gtk_stack_set_visible_child_name (GTK_STACK (stack), "empty");

    gtk_paned_pack1 (GTK_PANED (paned), ctx->device_list, FALSE, FALSE);
    gtk_paned_pack2 (GTK_PANED (paned), stack,           TRUE,  FALSE);

    gtk_box_pack_start (GTK_BOX (content), paned, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), build_status_bar (ctx),
                        FALSE, FALSE, 0);

    gtk_widget_show_all (content);
    return dialog;
}

/* ------------------------------------------------------------------ */
/*  Lifetime                                                            */
/* ------------------------------------------------------------------ */

static void
mesh_dialog_ctx_free (gpointer data)
{
    MeshDialogCtx *ctx = data;

    if (ctx->open_cancellable != NULL)
    {
        g_cancellable_cancel (ctx->open_cancellable);
        g_clear_object (&ctx->open_cancellable);
    }
    drop_connection (ctx);
    g_slice_free (MeshDialogCtx, ctx);
}

static void
on_dialog_destroyed (gpointer parent, GObject *was_dialog)
{
    (void) was_dialog;
    g_object_set_data (G_OBJECT (parent), PN_MESH_DIALOG_QDATA, NULL);
}

void
pn_mesh_dialog_present (GtkWindow *parent_window)
{
    GtkWidget     *dialog;
    MeshDialogCtx *ctx;

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

    ctx = g_slice_new0 (MeshDialogCtx);
    dialog = build_dialog (parent_window, ctx);
    ctx->dialog = dialog;

    /* Ctx dies with the dialog -- the GtkDialog owns one ref via
     * qdata, dropped automatically on widget destroy. */
    g_object_set_data_full (G_OBJECT (dialog), "pn-mesh-dialog-ctx",
                            ctx, mesh_dialog_ctx_free);

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
