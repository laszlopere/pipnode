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
/*  PnRewrite — gui tier.                                               */
/*                                                                     */
/*  The settings-dialog editor for the Rewrite node: a GtkSourceView    */
/*  with JSON syntax highlighting bound to the node's `template`         */
/*  property.  The node's GType, properties and the placeholder-         */
/*  expansion + receive() rewrite logic live in the GTK-free core file  */
/*  pn-rewrite.c; this file installs the build_class_tab vfunc slot onto */
/*  that class at editor startup (pn_rewrite_gui_install).  The editor   */
/*  reads and writes the template purely through the GObject property,   */
/*  so the headless runtime never loads this GtkSourceView-pulling half  */
/*  and the rewrite logic runs without GTK.                             */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rewrite-gui.h"
#include "pn-rewrite.h"

#include <gtksourceview/gtksource.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: GtkSourceView editor with JSON syntax             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject       *target;          /* borrowed PnRewrite */
    GtkTextBuffer *buffer;
    gulong         notify_handler;
    gboolean       updating;        /* re-entrancy guard */
} PnRewriteBinding;

static void
rewrite_binding_free (gpointer data)
{
    PnRewriteBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);
    g_free (bind);
}

static void
rewrite_binding_pull (PnRewriteBinding *bind)
{
    gchar *value = NULL;

    g_object_get (bind->target, "template", &value, NULL);

    {
        GtkTextIter start, end;
        gchar      *current;

        gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
        current = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

        if (g_strcmp0 (current, value != NULL ? value : "") != 0)
        {
            bind->updating = TRUE;
            gtk_text_buffer_set_text (bind->buffer,
                                      value != NULL ? value : "", -1);
            bind->updating = FALSE;
        }

        g_free (current);
    }

    g_free (value);
}

static void
rewrite_binding_push (PnRewriteBinding *bind)
{
    GtkTextIter start, end;
    gchar      *text;

    if (bind->updating)
        return;

    gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
    text = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

    bind->updating = TRUE;
    g_object_set (bind->target, "template", text, NULL);
    bind->updating = FALSE;

    g_free (text);
}

static void
on_rewrite_target_notify (GObject    *object G_GNUC_UNUSED,
                          GParamSpec *pspec  G_GNUC_UNUSED,
                          gpointer    user_data)
{
    rewrite_binding_pull (user_data);
}

static void
on_rewrite_buffer_changed (GtkTextBuffer *buffer G_GNUC_UNUSED,
                           gpointer       user_data)
{
    rewrite_binding_push (user_data);
}

static GtkWidget *
pn_rewrite_build_class_tab (PnNode    *self,
                            GtkWindow *parent G_GNUC_UNUSED)
{
    GObject                  *target = G_OBJECT (self);
    GtkSourceLanguageManager *langs;
    GtkSourceLanguage        *json_lang;
    GtkSourceBuffer          *buffer;
    GtkWidget                *view;
    GtkWidget                *scrolled;
    PnRewriteBinding         *bind;

    langs     = gtk_source_language_manager_get_default ();
    json_lang = gtk_source_language_manager_get_language (langs, "json");

    buffer = gtk_source_buffer_new_with_language (json_lang);
    gtk_source_buffer_set_highlight_syntax    (buffer, TRUE);
    gtk_source_buffer_set_highlight_matching_brackets (buffer, TRUE);

    view = gtk_source_view_new_with_buffer (buffer);
    gtk_source_view_set_show_line_numbers   (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_auto_indent         (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_indent_on_tab       (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_tab_width           (GTK_SOURCE_VIEW (view), 2);
    gtk_source_view_set_insert_spaces_instead_of_tabs (
            GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_highlight_current_line (GTK_SOURCE_VIEW (view), TRUE);
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
    g_object_unref (buffer);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    /* Give the editor a comfortable canvas for a multi-line JSON
     * object — twelve lines is roughly the size of the default
     * template plus a couple of extra fields the user typically adds
     * before the dialog feels cramped. */
    gtk_widget_set_size_request (scrolled, -1, 240);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), view);

    bind = g_new0 (PnRewriteBinding, 1);
    bind->target = target;
    bind->buffer = GTK_TEXT_BUFFER (gtk_text_view_get_buffer (
            GTK_TEXT_VIEW (view)));

    rewrite_binding_pull (bind);

    bind->notify_handler = g_signal_connect (
            target, "notify::template",
            G_CALLBACK (on_rewrite_target_notify), bind);
    g_signal_connect (bind->buffer, "changed",
                      G_CALLBACK (on_rewrite_buffer_changed), bind);

    g_object_set_data_full (G_OBJECT (scrolled),
                            "pn-rewrite-binding",
                            bind, rewrite_binding_free);

    return scrolled;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_rewrite_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_REWRITE));

    node_class->build_class_tab = pn_rewrite_build_class_tab;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
