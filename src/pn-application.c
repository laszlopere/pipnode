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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-application.h"
#include "pn-window.h"
#include "pn-worksheet.h"

#include <gio/gio.h>

struct _PnApplication
{
    GtkApplication parent_instance;

    gboolean opt_version;
    gchar   *opt_file;
    gchar   *opt_dbus_name;
    gchar  **opt_remaining;

    /** Path of the worksheet file selected on the command line (via
     *  --file or as a positional argument), to be loaded once the
     *  initial window is shown.  Owned by the application. */
    gchar *startup_file;

    /** Registration id for the org.pipas.pipnode.Worksheet D-Bus
     *  interface installed at the application's object path.  Zero
     *  while no interface is registered. */
    guint dbus_worksheet_reg_id;
};

G_DEFINE_TYPE (PnApplication, pn_application, GTK_TYPE_APPLICATION)

/* ------------------------------------------------------------------ */
/*  D-Bus interface for test access to PnWorksheet                     */
/*                                                                     */
/*  Mirrors the pattern pipevolution uses: the application registers   */
/*  a single object at its own object path and installs an interface  */
/*  per test-controlled widget.  Method handlers resolve the active   */
/*  widget at call time, so the interface tracks whichever worksheet  */
/*  is hosted by the application's currently active window.           */
/* ------------------------------------------------------------------ */

static const gchar worksheet_introspection_xml[] =
    "<node>"
    "  <interface name='org.pipas.pipnode.Worksheet'>"
    "    <method name='GetNodeCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetWireCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetNode'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='class_name' direction='out'/>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='icon' direction='out'/>"
    "      <arg type='d' name='x' direction='out'/>"
    "      <arg type='d' name='y' direction='out'/>"
    "      <arg type='b' name='has_input' direction='out'/>"
    "      <arg type='b' name='has_output' direction='out'/>"
    "    </method>"
    "    <method name='GetNodes'>"
    "      <arg type='a(sssddbb)' name='nodes' direction='out'/>"
    "    </method>"
    "    <method name='GetWire'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='i' name='source_index' direction='out'/>"
    "      <arg type='i' name='target_index' direction='out'/>"
    "    </method>"
    "    <method name='GetWires'>"
    "      <arg type='a(ii)' name='wires' direction='out'/>"
    "    </method>"
    "    <method name='Clear'>"
    "    </method>"
    "    <method name='LoadFromFile'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='SaveToFile'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='GetNodeUuid'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='uuid'  direction='out'/>"
    "    </method>"
    "    <method name='InjectDebugMessage'>"
    "      <arg type='s' name='text'  direction='in'/>"
    "    </method>"
    "    <method name='GetDebugRowCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetDebugRowFromId'>"
    "      <arg type='u' name='index'   direction='in'/>"
    "      <arg type='s' name='from_id' direction='out'/>"
    "    </method>"
    "    <method name='ClickDebugFromButton'>"
    "      <arg type='u' name='index'   direction='in'/>"
    "      <arg type='b' name='clicked' direction='out'/>"
    "    </method>"
    "    <method name='GetFocusPulseUuid'>"
    "      <arg type='s' name='uuid' direction='out'/>"
    "    </method>"
    "    <method name='GetWorksheetScroll'>"
    "      <arg type='d' name='hval'   direction='out'/>"
    "      <arg type='d' name='vval'   direction='out'/>"
    "      <arg type='d' name='hpage'  direction='out'/>"
    "      <arg type='d' name='vpage'  direction='out'/>"
    "      <arg type='d' name='hupper' direction='out'/>"
    "      <arg type='d' name='vupper' direction='out'/>"
    "    </method>"
    "    <method name='GrabWorksheetFocus'>"
    "    </method>"
    "    <method name='ResizeWindow'>"
    "      <arg type='u' name='width'  direction='in'/>"
    "      <arg type='u' name='height' direction='in'/>"
    "    </method>"
    "    <method name='GetDebugPaneAllocation'>"
    "      <arg type='i' name='width'  direction='out'/>"
    "      <arg type='i' name='height' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static PnWorksheet *
get_worksheet (GApplication *app)
{
    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (!win || !PN_IS_WINDOW (win))
        return NULL;
    return pn_window_get_worksheet (PN_WINDOW (win));
}

static gint
node_index_of (PnNodeStore *nodes, PnNode *node)
{
    guint i;
    guint n = pn_node_store_get_length (nodes);

    for (i = 0; i < n; i++)
    {
        if (pn_node_store_get_node (nodes, i) == node)
            return (gint) i;
    }
    return -1;
}

static void
handle_worksheet_method_call (
        GDBusConnection       *connection,
        const gchar           *sender,
        const gchar           *object_path,
        const gchar           *interface_name,
        const gchar           *method_name,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation,
        gpointer               user_data)
{
    GApplication *app       = G_APPLICATION (user_data);
    PnWorksheet  *worksheet = get_worksheet (app);
    PnNodeStore  *nodes;
    PnWireStore  *wires;

    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    if (!worksheet)
    {
        g_dbus_method_invocation_return_error (
                invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                "No active worksheet");
        return;
    }

    nodes = pn_worksheet_get_nodes (worksheet);
    wires = pn_worksheet_get_wires (worksheet);

    if (g_strcmp0 (method_name, "GetNodeCount") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(u)", pn_node_store_get_length (nodes)));
    }
    else if (g_strcmp0 (method_name, "GetWireCount") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(u)", pn_wire_store_get_length (wires)));
    }
    else if (g_strcmp0 (method_name, "GetNode") == 0)
    {
        guint  index;
        PnNode *node;
        const gchar   *class_name;
        const gchar   *name;
        const gchar   *icon;
        const PnPoint *pos;
        gboolean has_input, has_output;

        g_variant_get (parameters, "(u)", &index);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Node index %u out of range", index);
            return;
        }

        class_name = pn_node_get_class_name (node);
        name       = pn_node_get_name       (node);
        icon       = pn_node_get_icon       (node);
        pos        = pn_node_get_position   (node);
        has_input  = pn_node_get_has_input  (node);
        has_output = pn_node_get_has_output (node);

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(sssddbb)",
                               class_name ? class_name : "",
                               name       ? name       : "",
                               icon       ? icon       : "",
                               pos ? pos->x : 0.0,
                               pos ? pos->y : 0.0,
                               has_input,
                               has_output));
    }
    else if (g_strcmp0 (method_name, "GetNodes") == 0)
    {
        GVariantBuilder builder;
        guint i;
        guint n = pn_node_store_get_length (nodes);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssddbb)"));

        for (i = 0; i < n; i++)
        {
            PnNode        *node       = pn_node_store_get_node (nodes, i);
            const gchar   *class_name = pn_node_get_class_name (node);
            const gchar   *name       = pn_node_get_name       (node);
            const gchar   *icon       = pn_node_get_icon       (node);
            const PnPoint *pos        = pn_node_get_position   (node);

            g_variant_builder_add (
                    &builder, "(sssddbb)",
                    class_name ? class_name : "",
                    name       ? name       : "",
                    icon       ? icon       : "",
                    pos ? pos->x : 0.0,
                    pos ? pos->y : 0.0,
                    pn_node_get_has_input  (node),
                    pn_node_get_has_output (node));
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sssddbb))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetWire") == 0)
    {
        guint   index;
        PnWire *wire;

        g_variant_get (parameters, "(u)", &index);

        wire = pn_wire_store_get_wire (wires, index);
        if (!wire)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Wire index %u out of range", index);
            return;
        }

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(ii)",
                               node_index_of (nodes, pn_wire_get_source (wire)),
                               node_index_of (nodes, pn_wire_get_target (wire))));
    }
    else if (g_strcmp0 (method_name, "GetWires") == 0)
    {
        GVariantBuilder builder;
        guint i;
        guint n = pn_wire_store_get_length (wires);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ii)"));

        for (i = 0; i < n; i++)
        {
            PnWire *wire = pn_wire_store_get_wire (wires, i);
            g_variant_builder_add (
                    &builder, "(ii)",
                    node_index_of (nodes, pn_wire_get_source (wire)),
                    node_index_of (nodes, pn_wire_get_target (wire)));
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(ii))", &builder));
    }
    else if (g_strcmp0 (method_name, "Clear") == 0)
    {
        pn_worksheet_clear (worksheet);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "LoadFromFile") == 0)
    {
        const gchar *path = NULL;
        GError      *error = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_worksheet_load_from_file (worksheet, path, &error))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Load failed: %s",
                    error ? error->message : "(no error message)");
            g_clear_error (&error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SaveToFile") == 0)
    {
        const gchar *path = NULL;
        GError      *error = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_worksheet_save_to_file (worksheet, path, &error))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Save failed: %s",
                    error ? error->message : "(no error message)");
            g_clear_error (&error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetNodeUuid") == 0)
    {
        guint        index;
        PnNode      *node;
        const gchar *uuid;

        g_variant_get (parameters, "(u)", &index);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Node index %u out of range", index);
            return;
        }

        uuid = pn_node_get_uuid (node);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
    }
    else if (g_strcmp0 (method_name, "InjectDebugMessage") == 0)
    {
        GtkWindow   *win  = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *text = NULL;

        g_variant_get (parameters, "(&s)", &text);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        pn_window_inject_debug_message (PN_WINDOW (win), text);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetDebugRowCount") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      count;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        count = pn_window_get_debug_row_count (PN_WINDOW (win));
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(u)", count));
    }
    else if (g_strcmp0 (method_name, "GetDebugRowFromId") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gchar     *uuid;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        uuid = pn_window_get_debug_row_from_id (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
        g_free (uuid);
    }
    else if (g_strcmp0 (method_name, "ClickDebugFromButton") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gboolean   ok;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        ok = pn_window_click_debug_from_button (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "GetFocusPulseUuid") == 0)
    {
        const gchar *uuid = pn_worksheet_get_focus_pulse_uuid (worksheet);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
    }
    else if (g_strcmp0 (method_name, "GetWorksheetScroll") == 0)
    {
        /* Read the enclosing scrolled window's adjustments so a
         * test can verify focus_node_by_uuid actually moved the
         * viewport.  Reports zeros (rather than failing) when the
         * worksheet is somehow not packed inside a scrolled window
         * — keeps the call safe to make from any test. */
        GtkWidget     *anc;
        GtkAdjustment *hadj = NULL, *vadj = NULL;
        double         hval = 0, vval = 0;
        double         hpage = 0, vpage = 0;
        double         hupper = 0, vupper = 0;

        anc = gtk_widget_get_ancestor (
                GTK_WIDGET (worksheet), GTK_TYPE_SCROLLED_WINDOW);
        if (anc != NULL)
        {
            hadj = gtk_scrolled_window_get_hadjustment (
                    GTK_SCROLLED_WINDOW (anc));
            vadj = gtk_scrolled_window_get_vadjustment (
                    GTK_SCROLLED_WINDOW (anc));
        }
        if (hadj != NULL)
        {
            hval   = gtk_adjustment_get_value     (hadj);
            hpage  = gtk_adjustment_get_page_size (hadj);
            hupper = gtk_adjustment_get_upper     (hadj);
        }
        if (vadj != NULL)
        {
            vval   = gtk_adjustment_get_value     (vadj);
            vpage  = gtk_adjustment_get_page_size (vadj);
            vupper = gtk_adjustment_get_upper     (vadj);
        }

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(dddddd)",
                               hval, vval, hpage, vpage, hupper, vupper));
    }
    else if (g_strcmp0 (method_name, "GrabWorksheetFocus") == 0)
    {
        /* Mirror the gtk_widget_grab_focus call that on_button_press
         * makes in pn-worksheet.c: a regression test can use this to
         * exercise GtkScrolledWindow's "ensure focused widget
         * visible" path without synthesising real pointer events. */
        gtk_widget_grab_focus (GTK_WIDGET (worksheet));
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ResizeWindow") == 0)
    {
        /* Resize the active window through GTK so the size-allocate
         * cycle runs and downstream widgets (the debug paned in
         * particular) see a fresh allocation — going through the
         * window manager via xdotool/wmctrl can leave the GTK widget
         * tree out of sync, which is exactly the case the wide-window
         * Debug View regression test needs to exclude. */
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint w, h;

        g_variant_get (parameters, "(uu)", &w, &h);

        if (win == NULL)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        gtk_window_resize (win, (gint) w, (gint) h);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetDebugPaneAllocation") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        gint       w = 0, h = 0;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "No active window");
            return;
        }

        pn_window_get_debug_pane_allocation (PN_WINDOW (win), &w, &h);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(ii)", w, h));
    }
    else
    {
        g_dbus_method_invocation_return_error (
                invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                "Unknown method '%s'", method_name);
    }
}

static const GDBusInterfaceVTable worksheet_vtable = {
    handle_worksheet_method_call,
    NULL,
    NULL
};

static GDBusNodeInfo *worksheet_introspection_data = NULL;

static gboolean
pn_application_dbus_register (
        GApplication    *app,
        GDBusConnection *connection,
        const gchar     *object_path,
        GError         **error)
{
    PnApplication *self = PN_APPLICATION (app);

    if (!G_APPLICATION_CLASS (pn_application_parent_class)->
                dbus_register (app, connection, object_path, error))
        return FALSE;

    if (!worksheet_introspection_data)
        worksheet_introspection_data =
                g_dbus_node_info_new_for_xml (
                        worksheet_introspection_xml, NULL);

    self->dbus_worksheet_reg_id =
        g_dbus_connection_register_object (
                connection,
                object_path,
                worksheet_introspection_data->interfaces[0],
                &worksheet_vtable,
                app,
                NULL,
                error);

    return self->dbus_worksheet_reg_id > 0;
}

static void
pn_application_dbus_unregister (
        GApplication    *app,
        GDBusConnection *connection,
        const gchar     *object_path)
{
    PnApplication *self = PN_APPLICATION (app);

    if (self->dbus_worksheet_reg_id)
    {
        g_dbus_connection_unregister_object (
                connection, self->dbus_worksheet_reg_id);
        self->dbus_worksheet_reg_id = 0;
    }

    G_APPLICATION_CLASS (pn_application_parent_class)->
            dbus_unregister (app, connection, object_path);
}

/* --- Application lifecycle --- */

static gint
pn_application_handle_local_options (
        GApplication *app,
        GVariantDict *options)
{
    PnApplication *self = PN_APPLICATION (app);

    (void) options;

    if (self->opt_version)
    {
#ifdef GIT_VERSION
        g_print ("pipnode-editor %s (%s)\n", PACKAGE_VERSION, GIT_VERSION);
#else
        g_print ("pipnode-editor %s\n", PACKAGE_VERSION);
#endif
        return 0;  /* exit with success */
    }

    /*  Allow the functional tests (and the user) to run several
     *  pipnode instances side by side on the same session bus by
     *  overriding the well-known name they register under.  The
     *  GApplication object path follows the application id, so
     *  callers must derive it the same way (replace '.' with '/',
     *  prepend a leading '/'). */
    if (self->opt_dbus_name && *self->opt_dbus_name)
    {
        if (!g_application_id_is_valid (self->opt_dbus_name))
        {
            g_printerr ("pipnode-editor: invalid D-Bus name '%s'\n",
                        self->opt_dbus_name);
            return 1;
        }
        g_application_set_application_id (app, self->opt_dbus_name);
    }

    g_clear_pointer (&self->startup_file, g_free);

    if (self->opt_file && *self->opt_file)
        self->startup_file = g_strdup (self->opt_file);
    else if (self->opt_remaining && self->opt_remaining[0])
    {
        if (self->opt_remaining[1])
        {
            g_printerr ("pipnode-editor: too many arguments\n");
            return 1;
        }
        self->startup_file = g_strdup (self->opt_remaining[0]);
    }

    return -1;  /* continue normal processing */
}

static void
pn_application_activate (GApplication *app)
{
    PnApplication *self = PN_APPLICATION (app);
    PnWindow *window;

    window = pn_window_new (PN_APPLICATION (app));
    gtk_window_present (GTK_WINDOW (window));

    if (self->startup_file)
    {
        GError *error = NULL;
        if (!pn_window_load_file (window, self->startup_file, &error))
        {
            g_printerr ("pipnode-editor: could not open '%s': %s\n",
                        self->startup_file,
                        error ? error->message : "(no error message)");
            g_clear_error (&error);
        }
        g_clear_pointer (&self->startup_file, g_free);
    }
}

static void
pn_application_finalize (GObject *object)
{
    PnApplication *self = PN_APPLICATION (object);

    g_clear_pointer (&self->opt_file,      g_free);
    g_clear_pointer (&self->opt_dbus_name, g_free);
    g_clear_pointer (&self->opt_remaining, g_strfreev);
    g_clear_pointer (&self->startup_file,  g_free);

    G_OBJECT_CLASS (pn_application_parent_class)->finalize (object);
}

static void
pn_application_class_init (PnApplicationClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    GApplicationClass *app_class    = G_APPLICATION_CLASS (klass);

    object_class->finalize = pn_application_finalize;

    app_class->handle_local_options = pn_application_handle_local_options;
    app_class->activate             = pn_application_activate;
    app_class->dbus_register        = pn_application_dbus_register;
    app_class->dbus_unregister      = pn_application_dbus_unregister;
}

static void
pn_application_init (PnApplication *self)
{
    (void) self;
}

PnApplication *
pn_application_new (void)
{
    PnApplication *app;

    app = g_object_new (PN_TYPE_APPLICATION,
                        "application-id", "org.pipas.pipnode",
                        "flags", G_APPLICATION_DEFAULT_FLAGS,
                        NULL);

    {
        static GOptionEntry entries[] = {
            { "version", 'V', 0, G_OPTION_ARG_NONE, NULL,
              "Print version and exit", NULL },
            { "file", 'f', 0, G_OPTION_ARG_FILENAME, NULL,
              "Open the worksheet at FILE on startup", "FILE" },
            { "dbus-name", 'D', 0, G_OPTION_ARG_STRING, NULL,
              "Register the application under NAME on the session bus "
              "instead of the default 'org.pipas.pipnode' "
              "(useful for running several instances in parallel, "
              "e.g. from the functional tests)", "NAME" },
            { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL,
              NULL, "[FILE]" },
            { NULL }
        };

        entries[0].arg_data = &app->opt_version;
        entries[1].arg_data = &app->opt_file;
        entries[2].arg_data = &app->opt_dbus_name;
        entries[3].arg_data = &app->opt_remaining;

        g_application_add_main_option_entries (G_APPLICATION (app), entries);
    }

    return app;
}
