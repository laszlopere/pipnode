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
/*  A panel button that *runs* a pipnode worksheet, not just a         */
/*  launcher.  Each applet instance owns one worksheet (a .json        */
/*  document) under ~/.config/pipnode/panel/<unique-id>.json, executed */
/*  by the background engine — pipnode-editor running as a D-Bus       */
/*  service (org.pipas.pipnode), auto-started on first use.            */
/*                                                                     */
/*  The applet is a thin D-Bus client (no pipnode libraries, so an     */
/*  editor/node crash can never take down xfce4-panel):                */
/*    - on load it asks the engine to RunWorksheet its file;           */
/*    - it shows a PnPanelDisplay node's value on the button, updated  */
/*      live via the engine's ValueChanged signal;                     */
/*    - a left-click drives a PnPanelInput node (SetInput);            */
/*    - the right-click "Properties" item opens the worksheet for      */
/*      editing (PresentEditor) — the same running flow, shown.        */
/* ------------------------------------------------------------------ */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libxfce4panel/libxfce4panel.h>

/* The freedesktop/themed icon name shipped by the main app
 * (data/icons/hicolor/.../org.pipas.pipnode.png). */
#define PIPNODE_ICON_NAME "org.pipas.pipnode"

/* The background engine's well-known bus name, object path and the
 * panel control interface (see src/pn-application.c).  The name is
 * D-Bus-activatable, so the first call auto-starts the engine. */
#define PN_ENGINE_BUS    "org.pipas.pipnode"
#define PN_ENGINE_OBJECT "/org/pipas/pipnode"
#define PN_ENGINE_IFACE  "org.pipas.pipnode.Engine"

/* Value a left-click feeds to the worksheet's PnPanelInput.  A plain
 * "pressed" pulse; the worksheet decides what to do with it. */
#define PN_PANEL_CLICK_VALUE 1.0

/* A blank pipnode document, matching the on-disk format produced by
 * lib/pn-flow.c (PN_FLOW_FILE_VERSION 1).  The engine reparses and
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
    GtkWidget       *label;    /* Panel Display value, shown when set */

    gchar           *path;     /* this instance's worksheet file     */
    GDBusProxy      *engine;   /* org.pipas.pipnode.Engine proxy     */
    GCancellable    *cancel;   /* cancels in-flight async D-Bus work */
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
/*  Button display                                                      */
/* ------------------------------------------------------------------ */

/* Show @text from the worksheet's Panel Display next to the icon; an
 * empty string hides the label so a display-less worksheet keeps the
 * button icon-only. */
static void
set_display_text (PipnodeWorksheet *self, const gchar *text)
{
    if (text != NULL && *text != '\0')
    {
        gtk_label_set_text (GTK_LABEL (self->label), text);
        gtk_widget_show (self->label);
    }
    else
    {
        gtk_widget_hide (self->label);
    }
}

/* ------------------------------------------------------------------ */
/*  Engine D-Bus calls                                                  */
/* ------------------------------------------------------------------ */

static void
on_run_worksheet_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    PipnodeWorksheet *self = user_data;
    GVariant         *reply;
    GError           *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-worksheet: RunWorksheet failed: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }

    {
        const gchar *value = NULL;
        g_variant_get (reply, "(&s)", &value);
        set_display_text (self, value);
    }
    g_variant_unref (reply);
}

/* Engine signals arrive here; we act on ValueChanged for our own
 * worksheet (the engine multiplexes every applet on one interface, so
 * the path argument disambiguates). */
static void
on_engine_signal (GDBusProxy *proxy,
                  const gchar *sender,
                  const gchar *signal_name,
                  GVariant    *parameters,
                  gpointer     user_data)
{
    PipnodeWorksheet *self = user_data;

    (void) proxy;
    (void) sender;

    if (g_strcmp0 (signal_name, "ValueChanged") != 0)
        return;

    {
        const gchar *path  = NULL;
        const gchar *value = NULL;
        g_variant_get (parameters, "(&s&s)", &path, &value);
        if (g_strcmp0 (path, self->path) == 0)
            set_display_text (self, value);
    }
}

static void
on_engine_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    PipnodeWorksheet *self = user_data;
    GError           *error = NULL;

    (void) source;

    self->engine = g_dbus_proxy_new_for_bus_finish (result, &error);
    if (self->engine == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-worksheet: cannot reach the pipnode engine: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }

    g_signal_connect (self->engine, "g-signal",
                      G_CALLBACK (on_engine_signal), self);

    /* Start the worksheet running (auto-activates the engine) and pick
     * up the initial display value from the reply. */
    g_dbus_proxy_call (self->engine, "RunWorksheet",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_run_worksheet_done, self);
}

/* Fire-and-forget engine call carrying just the worksheet path. */
static void
engine_call_path (PipnodeWorksheet *self, const gchar *method)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, method,
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                         */
/* ------------------------------------------------------------------ */

/* Left-click: drive the worksheet's Panel Input. */
static void
on_button_clicked (GtkWidget        *button,
                   PipnodeWorksheet *self)
{
    (void) button;

    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "SetInput",
                       g_variant_new ("(sd)", self->path,
                                      (gdouble) PN_PANEL_CLICK_VALUE),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* Right-click "Properties": open the running worksheet for editing. */
static void
on_configure_plugin (XfcePanelPlugin  *plugin,
                     PipnodeWorksheet *self)
{
    (void) plugin;
    engine_call_path (self, "PresentEditor");
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

    g_cancellable_cancel (self->cancel);
    g_clear_object (&self->cancel);
    g_clear_object (&self->engine);
    g_clear_pointer (&self->path, g_free);

    g_slice_free (PipnodeWorksheet, self);
}

static void
pipnode_worksheet_construct (XfcePanelPlugin *plugin)
{
    PipnodeWorksheet *self;
    GtkWidget        *box;
    GError           *error = NULL;

    self = g_slice_new0 (PipnodeWorksheet);
    self->plugin = plugin;
    self->cancel = g_cancellable_new ();
    self->path   = worksheet_path (self);

    /* Flat, panel-styled button holding the icon plus a live value
     * label (hidden until the worksheet's Panel Display emits). */
    self->button = xfce_panel_create_button ();
    box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    self->image = gtk_image_new_from_icon_name (PIPNODE_ICON_NAME,
                                                GTK_ICON_SIZE_BUTTON);
    self->label = gtk_label_new (NULL);
    gtk_box_pack_start (GTK_BOX (box), self->image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), self->label, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (self->button), box);

    gtk_widget_set_tooltip_text (self->button,
                                 "Pipnode worksheet — click to send, "
                                 "right-click → Properties to edit");

    gtk_container_add (GTK_CONTAINER (plugin), self->button);

    /* Let the panel's own context menu pop up over the button. */
    xfce_panel_plugin_add_action_widget (plugin, self->button);

    /* Add the "Properties" entry to the right-click menu (the settings
     * menu) — fires "configure-plugin", which opens the editor. */
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
    gtk_widget_hide (self->label);   /* shown once a value arrives */

    /* Make sure the file exists, then connect to the engine
     * asynchronously so the panel never blocks on D-Bus. */
    if (!ensure_worksheet (self->path, &error))
    {
        g_warning ("pipnode-worksheet: could not create worksheet '%s': %s",
                   self->path, error->message);
        g_clear_error (&error);
        return;
    }

    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                              NULL,
                              PN_ENGINE_BUS, PN_ENGINE_OBJECT, PN_ENGINE_IFACE,
                              self->cancel, on_engine_ready, self);
}

/* Generates the xfce_panel_module_* entry points the panel dlopen()s. */
XFCE_PANEL_PLUGIN_REGISTER (pipnode_worksheet_construct)
