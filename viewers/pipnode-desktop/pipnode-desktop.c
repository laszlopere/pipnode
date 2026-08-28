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
/*  pipnode-desktop — the desktop viewer                               */
/*                                                                     */
/*  A plain window that *runs* a pipnode worksheet, not just shows a   */
/*  file.  It is the desktop twin of the XFCE panel applet and is      */
/*  engineered the same way, differing only in the surface it draws    */
/*  on: where the applet mirrors the worksheet's PANEL band into a     */
/*  strip in the desktop panel, this mirrors its DESKTOP layout into a */
/*  window — every widget the user placed in the Desktop Layout editor */
/*  appears here at the same window-relative (x, y), in a window of    */
/*  the size and title that editor set.                                */
/*                                                                     */
/*  The worksheet is not executed here.  It runs in the background     */
/*  engine — pipnode-editor as a D-Bus service (org.pipas.pipnode),    */
/*  auto-started on first use — which holds it in a hidden window whose */
/*  flow ticks for as long as the viewer is up.  The viewer asks the   */
/*  engine for the layout (GetDesktopLayout) and follows it            */
/*  (DesktopLayoutChanged); each node's live state arrives keyed by    */
/*  node UUID (WidgetChanged) and is pushed into its widget.           */
/*                                                                     */
/*  Like the applet it is a thin D-Bus client: it links GTK / GLib /   */
/*  json-glib only (GDBus comes via gio) and NO pipnode runtime        */
/*  library, so an engine or node crash can never take the viewer down */
/*  with it — the window simply goes stale until the engine returns,   */
/*  at which point the viewer re-runs the worksheet by itself.  The    */
/*  only pipnode code it embeds is the GTK/Cairo-only panel-widgets    */
/*  library and its PnWidgetMirror, statically linked, shared with the */
/*  applet and the editor.                                             */
/*                                                                     */
/*  Interaction mirrors the applet, one for one:                       */
/*    - a mouse click drives the PnPanelInput node(s): the click is    */
/*      forwarded (SendEvent) tagged with the button;                  */
/*    - a click on a Switch toggle or an Inject button acts on that    */
/*      one node, addressed by UUID (ActivateWidget);                  */
/*    - right-click → "Edit Worksheet" opens the worksheet for editing */
/*      (PresentEditor) — the same running flow, shown.                */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "pn-widget-mirror.h"

/* The freedesktop/themed icon name shipped by the main app
 * (data/icons/hicolor/.../org.pipas.pipnode.png). */
#define PIPNODE_ICON_NAME "org.pipas.pipnode"

/* The background engine's well-known bus name, object path and control
 * interface (see src/pn-application.c).  The name is D-Bus-activatable, so
 * the first call auto-starts the engine. */
#define PN_ENGINE_BUS    "org.pipas.pipnode"
#define PN_ENGINE_IFACE  "org.pipas.pipnode.Engine"

/* Fallbacks for a layout that names no window (an engine older than this
 * viewer, or a worksheet the editor has never opened a desktop tab on). */
#define PN_DESKTOP_FALLBACK_WIDTH   640
#define PN_DESKTOP_FALLBACK_HEIGHT  400
#define PN_DESKTOP_FALLBACK_WIDGET_H 48

typedef struct
{
    GtkWidget      *window;
    GtkWidget      *canvas;   /* GtkFixed the mirrored widgets sit on   */
    GtkWidget      *message;  /* shown INSTEAD of the canvas: the empty  */
                              /*   hint, or an engine error              */
    GtkWidget      *menu;     /* right-click menu (owned by this)       */

    PnWidgetMirror *mirror;   /* owns the widgets; we own their places  */

    gchar          *path;     /* the worksheet file being shown         */
    gchar          *bus_name; /* engine bus name (overridable, testing) */
    gchar          *obj_path; /* its object path, derived from the name  */
    GDBusProxy     *engine;
    GCancellable   *cancel;   /* cancels in-flight async D-Bus work     */

    gint            widget_h; /* height the engine draws widgets at     */
    gint            win_w;    /* the window size last applied from a    */
    gint            win_h;    /*   layout; 0 before the first one       */
} PipnodeDesktop;

/* ------------------------------------------------------------------ */
/*  Engine calls                                                        */
/* ------------------------------------------------------------------ */

/* Fire-and-forget engine call carrying just the worksheet path. */
static void
engine_call_path (PipnodeDesktop *self, const gchar *method)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, method,
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* A click on a mirrored interactive widget: ask the engine to act on that
 * one node.  The widget follows the node — the engine echoes the new state
 * back as a WidgetChanged — so the display can never drift from the flow. */
static void
on_widget_activated (const gchar *uuid, gpointer user_data)
{
    PipnodeDesktop *self = user_data;

    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "ActivateWidget",
                       g_variant_new ("(ss)", self->path, uuid),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Layout                                                              */
/* ------------------------------------------------------------------ */

/* Put @markup in the message area instead of the widget canvas.  This is
 * the ONLY place the viewer can explain itself: it is a desktop
 * application, normally launched from a menu or a file manager with no
 * terminal attached, so a warning on stderr would reach nobody. */
static void
show_message (PipnodeDesktop *self, const gchar *markup)
{
    gtk_label_set_markup (GTK_LABEL (self->message), markup);
    gtk_widget_set_visible (self->message, TRUE);
    gtk_widget_set_visible (self->canvas,  FALSE);
}

/* Report a failed engine call in the window.  The cure for much of what
 * can go wrong here — above all an engine still running an older pipnode
 * than the worksheet needs, which is what a bare `make install` leaves
 * behind, the old service process keeping the old code mapped — is to
 * bounce the engine, so name the menu item that does it. */
static void
show_engine_error (PipnodeDesktop *self,
                   const gchar    *what,
                   const gchar    *detail)
{
    gchar *esc    = g_markup_escape_text (detail, -1);
    gchar *markup = g_strdup_printf (
            "<span size='large'>%s</span>\n\n"
            "<span foreground='#888888'>%s</span>\n\n"
            "<span foreground='#888888'>The worksheet runs in the background "
            "pipnode engine.  If that engine has been running since before "
            "this worksheet's nodes existed — after installing a new pipnode, "
            "say — right-click here and choose <b>Reload Worksheet</b> to "
            "restart it.</span>",
            what, esc);

    show_message (self, markup);
    g_free (markup);
    g_free (esc);
}

/* Show the hint only while the worksheet places no widgets in the window. */
static void
update_empty_state (PipnodeDesktop *self)
{
    if (pn_widget_mirror_get_n_widgets (self->mirror) > 0)
    {
        gtk_widget_set_visible (self->message, FALSE);
        gtk_widget_set_visible (self->canvas,  TRUE);
        return;
    }

    show_message (self,
                  "<span foreground='#888888'>"
                  "Nothing placed in this worksheet's desktop layout yet — "
                  "open it in the editor and arrange some widgets"
                  "</span>");
}

/* Title the window after the worksheet's own file name — the fallback the
 * editor's empty-title placeholder promises, and also what the window is
 * called before any layout has arrived (or when none ever does, because
 * the engine could not run the worksheet). */
static void
set_title_from_path (PipnodeDesktop *self)
{
    gchar *base = g_path_get_basename (self->path);
    gchar *dot  = g_strrstr (base, ".json");

    if (dot != NULL)
        *dot = '\0';
    gtk_window_set_title (GTK_WINDOW (self->window), base);
    g_free (base);
}

/* Adopt the window properties the layout carries: title (falling back to
 * the worksheet's own file name, which is what the editor's placeholder
 * promises) and size.  The size is applied only when it actually CHANGES,
 * so a window the user has since dragged bigger survives every unrelated
 * layout change — but an actual resize in the editor still moves it, which
 * is the point of setting it there. */
static void
apply_window_properties (PipnodeDesktop *self, JsonObject *win)
{
    const gchar *title = NULL;
    gint         w = PN_DESKTOP_FALLBACK_WIDTH;
    gint         h = PN_DESKTOP_FALLBACK_HEIGHT;

    self->widget_h = PN_DESKTOP_FALLBACK_WIDGET_H;

    if (win != NULL)
    {
        if (json_object_has_member (win, "title"))
            title = json_object_get_string_member (win, "title");
        if (json_object_has_member (win, "width"))
            w = (gint) json_object_get_int_member (win, "width");
        if (json_object_has_member (win, "height"))
            h = (gint) json_object_get_int_member (win, "height");
        if (json_object_has_member (win, "widget_height"))
            self->widget_h =
                    (gint) json_object_get_int_member (win, "widget_height");
    }

    if (title == NULL || *title == '\0')
        set_title_from_path (self);
    else
        gtk_window_set_title (GTK_WINDOW (self->window), title);

    if (w != self->win_w || h != self->win_h)
    {
        self->win_w = w;
        self->win_h = h;

        /* set_default_size covers the not-yet-realized case (the layout
         * beat the window on screen); resize covers the usual one, where
         * the window is already up and the default is ignored. */
        gtk_window_set_default_size (GTK_WINDOW (self->window), w, h);
        if (gtk_widget_get_realized (self->window))
            gtk_window_resize (GTK_WINDOW (self->window), w, h);

        /* The canvas asks for the layout's size, so the window can never
         * be shrunk to where a placed widget would fall outside it. */
        gtk_widget_set_size_request (self->canvas, w, h);
    }
}

/* Place every mirrored widget where the layout says, at the height the
 * engine draws them at — the editor placed them at that same size against
 * the same origin, so the window reproduces the arrangement exactly. */
static void
place_widgets (PipnodeDesktop *self)
{
    guint n = pn_widget_mirror_get_n_widgets (self->mirror);
    guint i;

    for (i = 0; i < n; i++)
    {
        GtkWidget *w = pn_widget_mirror_get_widget (self->mirror, i);
        gdouble    x = 0.0, y = 0.0;

        pn_widget_mirror_set_height (self->mirror, i, self->widget_h);
        pn_widget_mirror_get_position (self->mirror, i, &x, &y);
        gtk_fixed_move (GTK_FIXED (self->canvas), w, (gint) x, (gint) y);
    }
}

/* A fresh layout from the engine: let the mirror reconcile the widget set,
 * then adopt the window it describes and place what it left us. */
static void
reconcile_layout (PipnodeDesktop *self, const gchar *layout_json)
{
    JsonParser *parser;

    if (!pn_widget_mirror_reconcile (self->mirror, layout_json))
        return;

    /* The window block is ours, not the mirror's — it describes the
     * surface, not the widgets. */
    parser = json_parser_new ();
    if (json_parser_load_from_data (parser, layout_json, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        {
            JsonObject *obj = json_node_get_object (root);

            apply_window_properties (
                    self,
                    json_object_has_member (obj, "window")
                        ? json_object_get_object_member (obj, "window")
                        : NULL);
        }
    }
    g_object_unref (parser);

    place_widgets (self);
    update_empty_state (self);
}

/* ------------------------------------------------------------------ */
/*  Engine plumbing                                                     */
/* ------------------------------------------------------------------ */

static void
on_get_layout_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    PipnodeDesktop *self  = user_data;
    GVariant       *reply;
    GError         *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_warning ("pipnode-desktop: GetDesktopLayout failed: %s",
                       error->message);
            show_engine_error (self, "Could not read this worksheet's "
                                     "desktop layout", error->message);
        }
        g_clear_error (&error);
        return;
    }

    {
        const gchar *layout = NULL;
        g_variant_get (reply, "(&s)", &layout);
        reconcile_layout (self, layout);
    }
    g_variant_unref (reply);
}

static void
on_run_worksheet_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    PipnodeDesktop *self  = user_data;
    GVariant       *reply;
    GError         *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            /* The engine could not load or start the worksheet — a
             * malformed file, a missing plugin, or a node type the engine
             * does not know.  There is nothing to show, so the window says
             * so rather than sitting there blank. */
            g_warning ("pipnode-desktop: RunWorksheet failed: %s",
                       error->message);
            show_engine_error (self, "Could not run this worksheet",
                               error->message);
        }
        g_clear_error (&error);
        return;
    }
    g_variant_unref (reply);   /* legacy display value — unused here */

    /* The worksheet is running; pull its desktop layout to build the
     * window. */
    g_dbus_proxy_call (self->engine, "GetDesktopLayout",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_get_layout_done, self);
}

/* Start (or restart) the worksheet on the engine.  A call on a proxy whose
 * name has no owner auto-activates the service file, so this both kicks off
 * the first run and respawns a freshly installed engine after a Quit. */
static void
engine_run_worksheet (PipnodeDesktop *self)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "RunWorksheet",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_run_worksheet_done, self);
}

/* Engine signals arrive here; the engine multiplexes every viewer and
 * applet on one interface, so the path argument disambiguates which
 * worksheet they concern. */
static void
on_engine_signal (GDBusProxy  *proxy,
                  const gchar *sender,
                  const gchar *signal_name,
                  GVariant    *parameters,
                  gpointer     user_data)
{
    PipnodeDesktop *self = user_data;

    (void) proxy;
    (void) sender;

    if (g_strcmp0 (signal_name, "DesktopLayoutChanged") == 0)
    {
        const gchar *path   = NULL;
        const gchar *layout = NULL;

        g_variant_get (parameters, "(&s&s)", &path, &layout);
        if (g_strcmp0 (path, self->path) == 0)
            reconcile_layout (self, layout);
    }
    else if (g_strcmp0 (signal_name, "WidgetChanged") == 0)
    {
        const gchar *path  = NULL;
        const gchar *uuid  = NULL;
        const gchar *state = NULL;

        g_variant_get (parameters, "(&s&s&s)", &path, &uuid, &state);
        if (g_strcmp0 (path, self->path) == 0)
            pn_widget_mirror_update (self->mirror, uuid, state);
    }
    /* LayoutChanged is the panel band's channel and ValueChanged the legacy
     * single-value one; neither concerns this window. */
}

/* The engine owns the bus name.  When that owner vanishes — it quit (our
 * "Reload Worksheet" item), was killed, or crashed — re-run the worksheet:
 * the method call auto-activates the service, starting whatever
 * pipnode-editor binary is now installed, and the reply rebuilds the
 * window.  This makes the viewer self-heal across an engine restart. */
static void
on_engine_owner_changed (GObject    *proxy,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
    PipnodeDesktop *self = user_data;
    gchar          *owner;

    (void) pspec;

    owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy));
    if (owner == NULL)
        engine_run_worksheet (self);
    g_free (owner);
}

static void
on_engine_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    PipnodeDesktop *self  = user_data;
    GError         *error = NULL;

    (void) source;

    self->engine = g_dbus_proxy_new_for_bus_finish (result, &error);
    if (self->engine == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_warning ("pipnode-desktop: cannot reach the pipnode engine: %s",
                       error->message);
            show_engine_error (self, "Cannot reach the pipnode engine",
                               error->message);
        }
        g_clear_error (&error);
        return;
    }

    g_signal_connect (self->engine, "g-signal",
                      G_CALLBACK (on_engine_signal), self);
    g_signal_connect (self->engine, "notify::g-name-owner",
                      G_CALLBACK (on_engine_owner_changed), self);

    engine_run_worksheet (self);
}

/* The object path a GApplication registers for @bus_name: its own rule is
 * the bus name with the dots turned into slashes, leading slash added.
 * Deriving it means --bus-name alone is enough to point the viewer at a
 * test engine (pipnode-editor --dbus-name), which registers under a
 * matching path. */
static gchar *
engine_object_path (const gchar *bus_name)
{
    gchar *path = g_strconcat ("/", bus_name, NULL);
    gchar *p;

    for (p = path; *p != '\0'; p++)
        if (*p == '.')
            *p = '/';

    return path;
}

static void
engine_connect (PipnodeDesktop *self)
{
    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                              NULL,
                              self->bus_name, self->obj_path,
                              PN_ENGINE_IFACE,
                              self->cancel, on_engine_ready, self);
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                         */
/* ------------------------------------------------------------------ */

/* Right-click "Edit Worksheet": open the running worksheet in the editor.
 * The same flow, shown — not a copy. */
static void
on_edit_worksheet (GtkMenuItem *item, gpointer user_data)
{
    (void) item;
    engine_call_path ((PipnodeDesktop *) user_data, "PresentEditor");
}

/* Right-click "Reload Worksheet": ask the shared engine to quit.  After a
 * `make install` the old engine binary keeps running; this bounces it.
 * Every viewer and applet watches the bus name and re-runs its worksheet
 * when the owner drops, so one quit cleanly respawns the new binary and
 * reconnects all of them. */
static void
on_reload_worksheet (GtkMenuItem *item, gpointer user_data)
{
    PipnodeDesktop *self = user_data;

    (void) item;

    if (self->engine == NULL)
    {
        engine_connect (self);   /* auto-activates the engine */
        return;
    }

    g_dbus_proxy_call (self->engine, "Quit", NULL,
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* A mouse click anywhere on the window's background: forward it to the
 * worksheet's Panel Input node(s), tagged with the button, as one "click"
 * event — the same event the applet sends, so one worksheet can drive both
 * surfaces.  Button 3 additionally opens the viewer's own menu. */
static gboolean
on_button_press (GtkWidget      *widget,
                 GdkEventButton *event,
                 gpointer        user_data)
{
    PipnodeDesktop *self = user_data;

    (void) widget;

    /* One click per press; ignore the synthetic GDK_2BUTTON_PRESS /
     * GDK_3BUTTON_PRESS so a double-click counts as two clicks, not four. */
    if (event->type != GDK_BUTTON_PRESS)
        return GDK_EVENT_PROPAGATE;

    if (self->engine != NULL)
        g_dbus_proxy_call (self->engine, "SendEvent",
                           g_variant_new ("(ssu)", self->path, "click",
                                          (guint32) event->button),
                           G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                           NULL, NULL);

    if (event->button == GDK_BUTTON_SECONDARY)
        gtk_menu_popup_at_pointer (GTK_MENU (self->menu), (GdkEvent *) event);

    return GDK_EVENT_PROPAGATE;
}

/* Build the right-click menu: the viewer's counterpart of the applet's two
 * panel-menu entries. */
static GtkWidget *
build_menu (PipnodeDesktop *self)
{
    GtkWidget *menu = gtk_menu_new ();
    GtkWidget *item;

    item = gtk_menu_item_new_with_mnemonic ("_Edit Worksheet");
    g_signal_connect (item, "activate", G_CALLBACK (on_edit_worksheet), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    item = gtk_menu_item_new_with_mnemonic ("_Reload Worksheet");
    g_signal_connect (item, "activate", G_CALLBACK (on_reload_worksheet), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    gtk_widget_show_all (menu);
    return g_object_ref_sink (menu);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* The window closed: stop the worksheet in the engine before quitting.
 * The engine is a shared service that would otherwise keep the flow
 * ticking with nothing to show it — CloseWorksheet autosaves and drops it.
 * (The panel applet deliberately does NOT do this: an applet's worksheet
 * is meant to keep running for as long as the panel is up.) */
static void
on_window_destroy (GtkWidget *widget, gpointer user_data)
{
    PipnodeDesktop *self = user_data;

    (void) widget;

    if (self->engine != NULL)
    {
        /* Synchronous, with a short timeout: the reply must land before the
         * main loop stops, or the call dies with the process. */
        g_dbus_proxy_call_sync (self->engine, "CloseWorksheet",
                                g_variant_new ("(s)", self->path),
                                G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    }

    gtk_main_quit ();
}

/* Ask the user for a worksheet when none was named on the command line, so
 * a menu launcher with no argument still lands somewhere useful. */
static gchar *
choose_worksheet (void)
{
    GtkWidget     *dialog;
    GtkFileFilter *filter;
    gchar         *path = NULL;

    dialog = gtk_file_chooser_dialog_new ("Open Worksheet", NULL,
                                          GTK_FILE_CHOOSER_ACTION_OPEN,
                                          "_Cancel", GTK_RESPONSE_CANCEL,
                                          "_Open",   GTK_RESPONSE_ACCEPT,
                                          NULL);

    filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, "Pipnode worksheets");
    gtk_file_filter_add_pattern (filter, "*.json");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

    gtk_widget_destroy (dialog);
    return path;
}

/* Report a fatal startup problem where the user can see it.  Both ways:
 * this is a desktop application, usually launched from a menu with no
 * terminal attached, but it also takes a worksheet on the command line —
 * and someone who typed the path deserves the answer where they typed. */
static void
fatal_dialog (const gchar *message)
{
    GtkWidget *dialog;

    g_printerr ("pipnode-desktop: %s\n", message);

    dialog = gtk_message_dialog_new (NULL, 0,
                                     GTK_MESSAGE_ERROR,
                                     GTK_BUTTONS_CLOSE,
                                     "%s", message);

    gtk_window_set_title (GTK_WINDOW (dialog), "Pipnode Desktop");
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

int
main (int argc, char **argv)
{
    PipnodeDesktop  self = { 0 };
    GOptionContext *ctx;
    GError         *error    = NULL;
    gchar          *bus_name = NULL;
    gchar          *opt_file = NULL;
    gchar         **files    = NULL;
    GtkWidget      *events;

    const GOptionEntry entries[] = {
        { "file", 'f', 0, G_OPTION_ARG_FILENAME, &opt_file,
          "Show the worksheet at FILE", "FILE" },
        { "bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name,
          "Bus name of the engine to talk to (default "
          PN_ENGINE_BUS ")", "NAME" },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files,
          NULL, "[WORKSHEET.json]" },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

    /* No parameter string: the remaining-args entry above supplies
     * "[WORKSHEET.json]", so the usage line reads the way
     * pipnode-editor's does rather than naming the argument twice. */
    ctx = g_option_context_new (NULL);
    g_option_context_set_summary (
            ctx, "Show a worksheet's desktop layout in a window.  The flow "
                 "itself runs in the\nbackground pipnode engine, which this "
                 "window mirrors live.");
    g_option_context_add_main_entries (ctx, entries, NULL);
    g_option_context_add_group (ctx, gtk_get_option_group (TRUE));
    if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
        g_printerr ("pipnode-desktop: %s\n", error->message);
        g_clear_error (&error);
        g_option_context_free (ctx);
        return 1;
    }
    g_option_context_free (ctx);

    self.bus_name = bus_name != NULL ? bus_name : g_strdup (PN_ENGINE_BUS);
    self.obj_path = engine_object_path (self.bus_name);
    self.cancel   = g_cancellable_new ();
    self.widget_h = PN_DESKTOP_FALLBACK_WIDGET_H;

    /* Naming the worksheet: --file wins over a positional argument, and
     * more than one positional is an error — the same precedence
     * pipnode-editor applies, so the two binaries take a worksheet the
     * same way whichever you reach for.  Neither given, we ask. */
    if (opt_file != NULL && *opt_file != '\0')
    {
        self.path = g_strdup (opt_file);
    }
    else if (files != NULL && files[0] != NULL)
    {
        if (files[1] != NULL)
        {
            g_printerr ("pipnode-desktop: too many arguments\n");
            g_strfreev (files);
            g_free (opt_file);
            return 1;
        }
        self.path = g_strdup (files[0]);
    }
    else
    {
        self.path = choose_worksheet ();
    }
    g_strfreev (files);
    g_free (opt_file);

    if (self.path == NULL)
        return 0;                       /* the chooser was cancelled */

    /* An absolute path: the engine keys its running worksheets by the
     * string it is given, so a relative one would be a different worksheet
     * to an applet showing the very same file. */
    if (!g_path_is_absolute (self.path))
    {
        gchar *cwd  = g_get_current_dir ();
        gchar *full = g_build_filename (cwd, self.path, NULL);

        g_free (self.path);
        g_free (cwd);
        self.path = full;
    }

    if (!g_file_test (self.path, G_FILE_TEST_EXISTS))
    {
        gchar *msg = g_strdup_printf ("No such worksheet:\n%s", self.path);

        fatal_dialog (msg);
        g_free (msg);
        return 1;
    }

    self.window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name (GTK_WINDOW (self.window), PIPNODE_ICON_NAME);
    set_title_from_path (&self);

    gtk_window_set_default_size (GTK_WINDOW (self.window),
                                 PN_DESKTOP_FALLBACK_WIDTH,
                                 PN_DESKTOP_FALLBACK_HEIGHT);
    g_signal_connect (self.window, "destroy",
                      G_CALLBACK (on_window_destroy), &self);

    /* An event box under everything catches clicks on the background: the
     * mirrored widgets are plain drawing areas, so anything not landing on
     * an interactive one falls through to here and becomes a Panel Input
     * click. */
    events = gtk_event_box_new ();
    gtk_container_add (GTK_CONTAINER (self.window), events);
    g_signal_connect (events, "button-press-event",
                      G_CALLBACK (on_button_press), &self);

    {
        GtkWidget *stack = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

        self.canvas = gtk_fixed_new ();
        gtk_widget_set_no_show_all (self.canvas, TRUE);
        gtk_box_pack_start (GTK_BOX (stack), self.canvas, TRUE, TRUE, 0);

        /* One label serves both the empty hint and any engine error;
         * update_empty_state / show_engine_error fill it in. */
        self.message = gtk_label_new (NULL);
        gtk_label_set_line_wrap (GTK_LABEL (self.message), TRUE);
        gtk_label_set_justify (GTK_LABEL (self.message), GTK_JUSTIFY_CENTER);
        gtk_label_set_max_width_chars (GTK_LABEL (self.message), 52);
        gtk_widget_set_margin_start  (self.message, 24);
        gtk_widget_set_margin_end    (self.message, 24);
        gtk_widget_set_no_show_all (self.message, TRUE);
        gtk_box_pack_start (GTK_BOX (stack), self.message, TRUE, FALSE, 0);

        gtk_container_add (GTK_CONTAINER (events), stack);
    }

    self.mirror = pn_widget_mirror_new (GTK_FIXED (self.canvas),
                                        on_widget_activated, &self);
    self.menu   = build_menu (&self);

    gtk_widget_show_all (self.window);
    update_empty_state (&self);

    /* Connect asynchronously so the window is up before the engine is,
     * however long the engine takes to start. */
    engine_connect (&self);

    gtk_main ();

    g_cancellable_cancel (self.cancel);
    g_clear_object (&self.cancel);
    g_clear_object (&self.engine);
    g_clear_pointer (&self.mirror, pn_widget_mirror_free);
    g_clear_object (&self.menu);
    g_free (self.path);
    g_free (self.bus_name);
    g_free (self.obj_path);
    return 0;
}
