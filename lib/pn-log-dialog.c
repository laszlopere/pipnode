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
/*  PnLogDialog                                                         */
/*                                                                     */
/*  A per-node log viewer.  Nodes report diagnostics through            */
/*  pn_node_log() into an in-memory ring (#PnLogEntry list) rather than */
/*  printing to stdout / stderr, which is invisible when pipnode is     */
/*  launched from a desktop icon.  This dialog tails that ring as a     */
/*  read-only console — one line per entry, colour-coded by severity —  */
/*  and refreshes live as the node logs more (it follows               */
/*  #PnNode::log-changed).  It is reachable from the node's right-click  */
/*  menu ("Log…", just below "Configure…").                             */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-log-dialog.h"

struct _PnLogDialog
{
    GtkWindow      parent_instance;

    PnNode        *node;           /* (owned) the node being tailed     */
    gulong         log_handler;    /* "log-changed" connection on @node */

    GtkTextView   *view;
    GtkTextBuffer *buffer;
    GtkTextTag    *tag_time;
    GtkTextTag    *tag_info;
    GtkTextTag    *tag_warning;
    GtkTextTag    *tag_error;
    GtkTextTag    *tag_empty;
};

G_DEFINE_FINAL_TYPE (PnLogDialog, pn_log_dialog, GTK_TYPE_WINDOW)

/* g_object_set_data key on the #PnNode pointing at its live dialog, so a
 * second "Log…" click raises the existing window instead of opening a
 * duplicate. */
#define DIALOG_DATA_KEY "pn-log-dialog"

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

static GtkTextTag *
tag_for_level (PnLogDialog *self,
               PnLogLevel   level)
{
    switch (level)
    {
    case PN_LOG_LEVEL_WARNING: return self->tag_warning;
    case PN_LOG_LEVEL_ERROR:   return self->tag_error;
    case PN_LOG_LEVEL_INFO:
    default:                   return self->tag_info;
    }
}

/** Rebuild the whole console from the node's current log ring and scroll
 *  to the newest line.  Cheap: the ring is capped at a couple hundred
 *  entries and this only runs on log-changed / dialog open. */
static void
rebuild (PnLogDialog *self)
{
    GtkTextBuffer *buf = self->buffer;
    GPtrArray     *log = pn_node_get_log (self->node);
    GtkTextIter    end;
    guint          i;

    gtk_text_buffer_set_text (buf, "", -1);

    if (log == NULL || log->len == 0)
    {
        gtk_text_buffer_get_end_iter (buf, &end);
        gtk_text_buffer_insert_with_tags (buf, &end,
                                          "No log entries yet.", -1,
                                          self->tag_empty, NULL);
        return;
    }

    for (i = 0; i < log->len; i++)
    {
        const PnLogEntry *entry = g_ptr_array_index (log, i);
        gint64            usec  = pn_log_entry_get_time (entry);
        GDateTime        *when  = g_date_time_new_from_unix_local (
                                      usec / G_USEC_PER_SEC);
        gchar            *stamp = when != NULL
                                    ? g_date_time_format (when, "%H:%M:%S")
                                    : g_strdup ("--:--:--");
        gchar            *head  = g_strdup_printf (
                "[%s] %-7s  ", stamp,
                pn_log_level_to_string (pn_log_entry_get_level (entry)));

        gtk_text_buffer_get_end_iter (buf, &end);
        gtk_text_buffer_insert_with_tags (buf, &end, head, -1,
                                          self->tag_time, NULL);

        gtk_text_buffer_get_end_iter (buf, &end);
        gtk_text_buffer_insert_with_tags (
                buf, &end, pn_log_entry_get_message (entry), -1,
                tag_for_level (self, pn_log_entry_get_level (entry)), NULL);

        gtk_text_buffer_get_end_iter (buf, &end);
        gtk_text_buffer_insert (buf, &end, "\n", -1);

        g_free (head);
        g_free (stamp);
        if (when != NULL)
            g_date_time_unref (when);
    }

    /* Keep the freshest line in view. */
    gtk_text_buffer_get_end_iter (buf, &end);
    gtk_text_view_scroll_to_iter (self->view, &end, 0.0, FALSE, 0.0, 0.0);
}

static void
on_log_changed (PnNode      *node,
                PnLogDialog *self)
{
    (void) node;
    rebuild (self);
}

static void
on_clear_clicked (GtkButton   *button,
                  PnLogDialog *self)
{
    (void) button;
    pn_node_clear_log (self->node);   /* emits log-changed → rebuild */
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_log_dialog_dispose (GObject *object)
{
    PnLogDialog *self = PN_LOG_DIALOG (object);

    if (self->node != NULL)
    {
        if (self->log_handler != 0)
        {
            g_signal_handler_disconnect (self->node, self->log_handler);
            self->log_handler = 0;
        }
        if (g_object_get_data (G_OBJECT (self->node), DIALOG_DATA_KEY) == self)
            g_object_set_data (G_OBJECT (self->node), DIALOG_DATA_KEY, NULL);
        g_clear_object (&self->node);
    }

    G_OBJECT_CLASS (pn_log_dialog_parent_class)->dispose (object);
}

static void
pn_log_dialog_class_init (PnLogDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = pn_log_dialog_dispose;
}

static void
pn_log_dialog_init (PnLogDialog *self)
{
    GtkWidget *outer;
    GtkWidget *scroll;
    GtkWidget *button_box;
    GtkWidget *clear_button;
    GtkWidget *close_button;

    gtk_window_set_default_size (GTK_WINDOW (self), 560, 360);
    gtk_window_set_modal        (GTK_WINDOW (self), FALSE);
    gtk_window_set_position     (GTK_WINDOW (self),
                                 GTK_WIN_POS_CENTER_ON_PARENT);

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), outer);

    self->view   = GTK_TEXT_VIEW (gtk_text_view_new ());
    self->buffer = gtk_text_view_get_buffer (self->view);
    gtk_text_view_set_editable      (self->view, FALSE);
    gtk_text_view_set_cursor_visible(self->view, FALSE);
    gtk_text_view_set_monospace     (self->view, TRUE);
    gtk_text_view_set_wrap_mode     (self->view, GTK_WRAP_WORD_CHAR);
    g_object_set (self->view,
                  "left-margin", 6, "right-margin", 6,
                  "top-margin",  4, "bottom-margin", 4,
                  NULL);

    /* Severity colours.  Foreground only so they read on either a light
     * or dark theme; the timestamp/level prefix is dimmed. */
    self->tag_time = gtk_text_buffer_create_tag (
            self->buffer, NULL, "foreground", "#888888", NULL);
    self->tag_info = gtk_text_buffer_create_tag (
            self->buffer, NULL, NULL);
    self->tag_warning = gtk_text_buffer_create_tag (
            self->buffer, NULL, "foreground", "#c07000", NULL);
    self->tag_error = gtk_text_buffer_create_tag (
            self->buffer, NULL, "foreground", "#cc2222",
            "weight", PANGO_WEIGHT_BOLD, NULL);
    self->tag_empty = gtk_text_buffer_create_tag (
            self->buffer, NULL, "foreground", "#888888",
            "style", PANGO_STYLE_ITALIC, NULL);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (scroll), GTK_WIDGET (self->view));
    gtk_box_pack_start (GTK_BOX (outer), scroll, TRUE, TRUE, 0);

    button_box = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box),
                               GTK_BUTTONBOX_END);
    gtk_box_set_spacing (GTK_BOX (button_box), 6);
    g_object_set (button_box,
                  "margin-start", 8, "margin-end", 8,
                  "margin-top",   6, "margin-bottom", 6,
                  NULL);
    gtk_box_pack_start (GTK_BOX (outer), button_box, FALSE, FALSE, 0);

    clear_button = gtk_button_new_with_mnemonic ("C_lear");
    g_signal_connect (clear_button, "clicked",
                      G_CALLBACK (on_clear_clicked), self);
    gtk_container_add (GTK_CONTAINER (button_box), clear_button);
    /* Clear sits at the left of the END-laid box. */
    gtk_button_box_set_child_secondary (GTK_BUTTON_BOX (button_box),
                                        clear_button, TRUE);

    close_button = gtk_button_new_with_mnemonic ("_Close");
    g_signal_connect_swapped (close_button, "clicked",
                              G_CALLBACK (gtk_widget_destroy), self);
    gtk_container_add (GTK_CONTAINER (button_box), close_button);
}

/* ------------------------------------------------------------------ */
/*  Public surface                                                     */
/* ------------------------------------------------------------------ */

void
pn_log_dialog_present (GtkWindow *parent, PnNode *node)
{
    PnLogDialog *dialog;

    g_return_if_fail (node == NULL || PN_IS_NODE (node));
    if (node == NULL)
        return;

    dialog = g_object_get_data (G_OBJECT (node), DIALOG_DATA_KEY);
    if (dialog == NULL)
    {
        const gchar *name = pn_node_get_name (node);
        gchar       *title;

        dialog = g_object_new (PN_TYPE_LOG_DIALOG, NULL);
        dialog->node = g_object_ref (node);
        g_object_set_data (G_OBJECT (node), DIALOG_DATA_KEY, dialog);

        title = g_strdup_printf ("%s — Log",
                                 (name != NULL && *name != '\0')
                                     ? name
                                     : pn_node_get_class_name (node));
        gtk_window_set_title (GTK_WINDOW (dialog), title);
        g_free (title);

        dialog->log_handler = g_signal_connect (
                node, "log-changed", G_CALLBACK (on_log_changed), dialog);

        rebuild (dialog);
    }

    if (parent != NULL)
    {
        gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
    }

    gtk_widget_show_all (GTK_WIDGET (dialog));
    gtk_window_present  (GTK_WINDOW (dialog));
}
