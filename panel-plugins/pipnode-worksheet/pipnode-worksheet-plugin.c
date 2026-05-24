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
/*  Pipnode Worksheet — XFCE panel applet                             */
/*                                                                     */
/*  A panel button showing the pipnode icon.  Each applet instance    */
/*  owns one pipnode worksheet (a .json document) stored under         */
/*  ~/.config/pipnode/panel/<unique-id>.json.  Left-clicking the       */
/*  button, or picking "Properties" from the panel's right-click       */
/*  menu, launches pipnode-editor on that worksheet, creating an       */
/*  empty worksheet first if it does not exist yet.                    */
/*                                                                     */
/*  This is a desktop-integration launcher, not a pipnode node         */
/*  plugin: it links only GTK / GLib / libxfce4panel and spawns the    */
/*  editor much like a .desktop "Exec=" entry would.                   */
/* ------------------------------------------------------------------ */

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libxfce4panel/libxfce4panel.h>

/* The freedesktop/themed icon name shipped by the main app
 * (data/icons/hicolor/.../org.pipas.pipnode.png). */
#define PIPNODE_ICON_NAME "org.pipas.pipnode"

/* A blank pipnode document, matching the on-disk format produced by
 * lib/pn-flow.c (PN_FLOW_FILE_VERSION 1).  pipnode-editor reparses and
 * rewrites this on first save, so only the structure has to be valid. */
static const gchar EMPTY_WORKSHEET[] =
    "{\n"
    "  \"format\" : \"pipnode\",\n"
    "  \"version\" : 1,\n"
    "  \"nodes\" : [],\n"
    "  \"connections\" : [],\n"
    "  \"sheets\" : [\n"
    "    \"Worksheet\"\n"
    "  ],\n"
    "  \"active_sheet\" : \"Worksheet\"\n"
    "}\n";

typedef struct
{
    XfcePanelPlugin *plugin;
    GtkWidget       *button;   /* panel button (owned by the plugin) */
    GtkWidget       *image;    /* icon inside the button             */
} PipnodeWorksheet;

/* ------------------------------------------------------------------ */
/*  Worksheet location / creation                                      */
/* ------------------------------------------------------------------ */

/* ~/.config/pipnode/panel/<unique-id>.json — one file per applet
 * instance, so two panel buttons keep distinct worksheets. */
static gchar *
worksheet_path (PipnodeWorksheet *self)
{
    gchar    *base;
    gchar    *path;
    gint      id;

    id = xfce_panel_plugin_get_unique_id (self->plugin);
    base = g_strdup_printf ("%d.json", id);
    path = g_build_filename (g_get_user_config_dir (),
                             "pipnode", "panel", base, NULL);
    g_free (base);

    return path;
}

/* Make sure the worksheet file exists, creating an empty one (and its
 * parent directory) on first use.  Returns FALSE and sets *error on
 * failure. */
static gboolean
ensure_worksheet (const gchar *path,
                  GError     **error)
{
    gchar *dir;

    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;

    dir = g_path_get_dirname (path);
    if (g_mkdir_with_parents (dir, 0700) != 0)
    {
        g_set_error (error, G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Could not create %s: %s",
                     dir, g_strerror (errno));
        g_free (dir);
        return FALSE;
    }
    g_free (dir);

    return g_file_set_contents (path,
                                EMPTY_WORKSHEET, sizeof EMPTY_WORKSHEET - 1,
                                error);
}

/* ------------------------------------------------------------------ */
/*  Open the worksheet in pipnode-editor                               */
/* ------------------------------------------------------------------ */

static void
report_error (PipnodeWorksheet *self,
              const gchar      *summary,
              const gchar      *detail)
{
    GtkWidget *dialog;
    GtkWidget *toplevel;

    toplevel = gtk_widget_get_toplevel (self->button);

    dialog = gtk_message_dialog_new (
        GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", summary);
    if (detail != NULL)
        gtk_message_dialog_format_secondary_text (
            GTK_MESSAGE_DIALOG (dialog), "%s", detail);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show_all (dialog);
}

static void
open_worksheet (PipnodeWorksheet *self)
{
    GError *error = NULL;
    gchar  *path;
    gchar  *editor;
    gchar  *argv[3];

    path = worksheet_path (self);

    if (!ensure_worksheet (path, &error))
    {
        report_error (self, "Could not create the worksheet.",
                      error->message);
        g_clear_error (&error);
        g_free (path);
        return;
    }

    /* Prefer an editor on $PATH; fall back to the install location baked
     * in at build time. */
    editor = g_find_program_in_path ("pipnode-editor");
    if (editor == NULL)
        editor = g_strdup (PIPNODE_EDITOR_PATH);

    argv[0] = editor;
    argv[1] = path;
    argv[2] = NULL;

    if (!g_spawn_async (NULL, argv, NULL,
                        G_SPAWN_DO_NOT_REAP_CHILD,
                        NULL, NULL, NULL, &error))
    {
        report_error (self, "Could not launch pipnode-editor.",
                      error->message);
        g_clear_error (&error);
    }

    g_free (editor);
    g_free (path);
}

static void
on_button_clicked (GtkWidget        *button,
                   PipnodeWorksheet *self)
{
    (void) button;
    open_worksheet (self);
}

static void
on_configure_plugin (XfcePanelPlugin  *plugin,
                     PipnodeWorksheet *self)
{
    (void) plugin;
    open_worksheet (self);
}

/* ------------------------------------------------------------------ */
/*  Panel lifecycle                                                    */
/* ------------------------------------------------------------------ */

static gboolean
on_size_changed (XfcePanelPlugin  *plugin,
                 gint              size,
                 PipnodeWorksheet *self)
{
    (void) size;
    gtk_image_set_pixel_size (GTK_IMAGE (self->image),
                              xfce_panel_plugin_get_icon_size (plugin));
    return TRUE;   /* handled */
}

static void
on_free_data (XfcePanelPlugin  *plugin,
              PipnodeWorksheet *self)
{
    (void) plugin;
    g_slice_free (PipnodeWorksheet, self);
}

static void
pipnode_worksheet_construct (XfcePanelPlugin *plugin)
{
    PipnodeWorksheet *self;

    self = g_slice_new0 (PipnodeWorksheet);
    self->plugin = plugin;

    /* Flat, panel-styled button holding the icon. */
    self->button = xfce_panel_create_button ();
    self->image  = gtk_image_new_from_icon_name (PIPNODE_ICON_NAME,
                                                 GTK_ICON_SIZE_BUTTON);
    gtk_container_add (GTK_CONTAINER (self->button), self->image);

    gtk_widget_set_tooltip_text (self->button,
                                 "Open the Pipnode worksheet");

    gtk_container_add (GTK_CONTAINER (plugin), self->button);

    /* Let the panel's own context menu pop up over the button. */
    xfce_panel_plugin_add_action_widget (plugin, self->button);

    /* Add the "Properties" entry to the right-click menu (the settings
     * menu) — fires "configure-plugin" when chosen. */
    xfce_panel_plugin_menu_show_configure (plugin);

    g_signal_connect (self->button, "clicked",
                      G_CALLBACK (on_button_clicked), self);
    g_signal_connect (plugin, "configure-plugin",
                      G_CALLBACK (on_configure_plugin), self);
    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed), self);
    g_signal_connect (plugin, "free-data",
                      G_CALLBACK (on_free_data), self);

    gtk_widget_show_all (self->button);
}

/* Generates the xfce_panel_module_* entry points the panel dlopen()s. */
XFCE_PANEL_PLUGIN_REGISTER (pipnode_worksheet_construct)
