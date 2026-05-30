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
/*  PnDeviceDialog -- the reusable device-dialog frame.                */
/*                                                                     */
/*  Extracted from pn-mesh-dialog.c's build_dialog / build_status_bar  */
/*  / show_loading / hide_loading / busy_inc / busy_dec, with no        */
/*  behaviour change.  The struct is attached to the GtkDialog via      */
/*  qdata so it lives exactly as long as the dialog.                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-device-dialog.h"

#define PN_DEVICE_DIALOG_CTX_QDATA "pn-device-dialog-ctx"

struct _PnDeviceDialog
{
    GtkWidget   *dialog;
    GtkWidget   *content;          /* the dialog's content vbox */
    GtkWidget   *paned;            /* NULL until a sidebar is set */
    GtkWidget   *body;             /* the overlay (notebook + spinner) */
    GtkNotebook *notebook;
    GtkWidget   *loading_overlay;  /* spinner+label box; visibility toggled */
    GtkSpinner  *loading_spinner;
    GtkLabel    *status_label;

    /* Reference-counted busy state.  The overlay is shown on the 0->1
     * edge and hidden on the 1->0 edge so concurrent waiters keep the
     * spinner up until the last one settles. */
    gint         busy_count;
};

/* ------------------------------------------------------------------ */
/*  Status bar                                                          */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_status (PnDeviceDialog *self, const gchar *text)
{
    g_return_if_fail (self != NULL);
    gtk_label_set_text (self->status_label, text != NULL ? text : "");
}

void
pn_device_dialog_set_statusf (PnDeviceDialog *self, const gchar *fmt, ...)
{
    va_list  ap;
    gchar   *text;

    g_return_if_fail (self != NULL);

    va_start (ap, fmt);
    text = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    pn_device_dialog_set_status (self, text);
    g_free (text);
}

/* ------------------------------------------------------------------ */
/*  Busy overlay                                                        */
/* ------------------------------------------------------------------ */

/* Show the big spinner over the notebook and lock the notebook so the
 * user cannot switch tabs (or read a half-populated form) while a
 * device round-trip is in flight. */
static void
show_loading (PnDeviceDialog *self)
{
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), FALSE);
    gtk_widget_show (self->loading_overlay);
    gtk_spinner_start (self->loading_spinner);
}

static void
hide_loading (PnDeviceDialog *self)
{
    gtk_spinner_stop (self->loading_spinner);
    gtk_widget_hide (self->loading_overlay);
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), TRUE);
}

void
pn_device_dialog_push_busy (PnDeviceDialog *self)
{
    g_return_if_fail (self != NULL);
    if (self->busy_count++ == 0)
        show_loading (self);
}

void
pn_device_dialog_pop_busy (PnDeviceDialog *self)
{
    g_return_if_fail (self != NULL);
    if (self->busy_count == 0)
        return;
    if (--self->busy_count == 0)
        hide_loading (self);
}

gboolean
pn_device_dialog_is_busy (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, FALSE);
    return self->busy_count > 0;
}

/* ------------------------------------------------------------------ */
/*  Notebook                                                            */
/* ------------------------------------------------------------------ */

GtkNotebook *
pn_device_dialog_get_notebook (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->notebook;
}

void
pn_device_dialog_append_page (PnDeviceDialog *self,
                              GtkWidget      *child,
                              const gchar    *tab_label)
{
    g_return_if_fail (self != NULL);
    g_return_if_fail (GTK_IS_WIDGET (child));
    gtk_notebook_append_page (self->notebook, child,
                              gtk_label_new (tab_label));
}

gint
pn_device_dialog_get_current_page (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, -1);
    return gtk_notebook_get_current_page (self->notebook);
}

void
pn_device_dialog_set_current_page (PnDeviceDialog *self, gint index)
{
    g_return_if_fail (self != NULL);
    gtk_notebook_set_current_page (self->notebook, index);
}

void
pn_device_dialog_set_pages_sensitive (PnDeviceDialog *self,
                                      gboolean        sensitive)
{
    g_return_if_fail (self != NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), sensitive);
}

/* ------------------------------------------------------------------ */
/*  Sidebar                                                             */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_sidebar (PnDeviceDialog *self,
                              GtkWidget      *sidebar,
                              gint            position)
{
    g_return_if_fail (self != NULL);

    if (sidebar == NULL)
    {
        /* Drop an existing sidebar: move the body back out of the paned
         * and into the content box where the paned sat. */
        if (self->paned != NULL)
        {
            g_object_ref (self->body);
            gtk_container_remove (GTK_CONTAINER (self->paned), self->body);
            gtk_widget_destroy (self->paned);
            self->paned = NULL;
            gtk_box_pack_start (GTK_BOX (self->content), self->body,
                                TRUE, TRUE, 0);
            gtk_box_reorder_child (GTK_BOX (self->content), self->body, 0);
            g_object_unref (self->body);
            gtk_widget_show (self->body);
        }
        return;
    }

    if (self->paned == NULL)
    {
        /* First sidebar: lift the body out of the content box, wrap it
         * and the sidebar in an HPaned, and put that back where the
         * body was (above the separator + status bar). */
        self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_wide_handle (GTK_PANED (self->paned), TRUE);

        g_object_ref (self->body);
        gtk_container_remove (GTK_CONTAINER (self->content), self->body);

        gtk_paned_pack1 (GTK_PANED (self->paned), sidebar, FALSE, FALSE);
        gtk_paned_pack2 (GTK_PANED (self->paned), self->body, TRUE, FALSE);
        g_object_unref (self->body);

        gtk_box_pack_start (GTK_BOX (self->content), self->paned,
                            TRUE, TRUE, 0);
        gtk_box_reorder_child (GTK_BOX (self->content), self->paned, 0);
    }
    else
    {
        /* Replace the existing sidebar in place. */
        GtkWidget *old = gtk_paned_get_child1 (GTK_PANED (self->paned));
        if (old != NULL)
            gtk_container_remove (GTK_CONTAINER (self->paned), old);
        gtk_paned_pack1 (GTK_PANED (self->paned), sidebar, FALSE, FALSE);
    }

    gtk_paned_set_position (GTK_PANED (self->paned), position);
    gtk_widget_show_all (self->paned);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_status_bar (PnDeviceDialog *self)
{
    GtkWidget *bar = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (bar), 0.0);
    gtk_widget_set_margin_start  (bar, 6);
    gtk_widget_set_margin_end    (bar, 6);
    gtk_widget_set_margin_top    (bar, 2);
    gtk_widget_set_margin_bottom (bar, 4);
    /* Long error strings get an ellipsis so they do not stretch the
     * dialog.  The label is selectable so the user can copy any
     * error text out for support. */
    gtk_label_set_ellipsize  (GTK_LABEL (bar), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable (GTK_LABEL (bar), TRUE);

    self->status_label = GTK_LABEL (bar);
    return bar;
}

/* The notebook wrapped in a GtkOverlay so a big spinner can float on
 * top while the dialog waits on the device (handshake or any write's
 * verify-cycle).  The notebook stays the bottom (interactive) layer;
 * the spinner+label box is the top (passive) overlay child, set
 * no_show_all so gtk_widget_show_all does NOT reveal it -- the busy
 * counter flips its visibility. */
static GtkWidget *
build_body (PnDeviceDialog *self)
{
    GtkWidget *overlay;
    GtkWidget *notebook;
    GtkWidget *box;
    GtkWidget *spinner;
    GtkWidget *label;

    notebook = gtk_notebook_new ();
    gtk_notebook_set_scrollable (GTK_NOTEBOOK (notebook), TRUE);
    self->notebook = GTK_NOTEBOOK (notebook);

    overlay = gtk_overlay_new ();
    gtk_container_add (GTK_CONTAINER (overlay), notebook);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    /* Centred horizontally + vertically so the spinner lands in the
     * middle of the body regardless of dialog size. */
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all (box, TRUE);

    spinner = gtk_spinner_new ();
    /* GtkSpinner has no intrinsic size beyond the theme's
     * "spinner-size" -- bump it to something a user notices across the
     * whole body. */
    gtk_widget_set_size_request (spinner, 64, 64);
    gtk_widget_show (spinner);
    gtk_box_pack_start (GTK_BOX (box), spinner, FALSE, FALSE, 0);

    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label),
            "<b><span size=\"large\">Talking to device…</span></b>");
    gtk_widget_show (label);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), box);

    self->loading_overlay = box;
    self->loading_spinner = GTK_SPINNER (spinner);
    self->body            = overlay;
    return overlay;
}

static void
device_dialog_free (gpointer data)
{
    g_slice_free (PnDeviceDialog, data);
}

PnDeviceDialog *
pn_device_dialog_new (GtkWindow *parent, const gchar *title)
{
    PnDeviceDialog *self;
    GtkWidget      *dialog;
    GtkWidget      *content;

    g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

    self = g_slice_new0 (PnDeviceDialog);

    /* Modeless so the user keeps access to whatever is behind the
     * dialog (useful when comparing the device against the app). */
    dialog = gtk_dialog_new_with_buttons (
            title, parent, GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);
    self->dialog = dialog;

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content), 0);
    self->content = content;

    /* Body (notebook + spinner overlay) on top, a separator, then the
     * status bar.  A sidebar -- if set later -- lifts the body into the
     * left/right panes of a GtkPaned in place. */
    gtk_box_pack_start (GTK_BOX (content), build_body (self), TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), build_status_bar (self),
                        FALSE, FALSE, 0);

    /* Own the shell from the dialog so it is freed on destroy. */
    g_object_set_data_full (G_OBJECT (dialog), PN_DEVICE_DIALOG_CTX_QDATA,
                            self, device_dialog_free);
    return self;
}

GtkWidget *
pn_device_dialog_get_dialog (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->dialog;
}
