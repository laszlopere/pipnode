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
/*  Pipnode Panel — XFCE panel applet                                 */
/*                                                                     */
/*  A panel button that *runs* a pipnode worksheet, not just a         */
/*  launcher.  Each applet instance owns one worksheet (a .json        */
/*  document) under ~/.config/pipnode/panel/<unique-id>.json, executed */
/*  by the background engine — pipnode-editor running as a D-Bus       */
/*  service (org.pipas.pipnode), auto-started on first use.            */
/*                                                                     */
/*  The applet is a live mirror of the worksheet's panel layout: every */
/*  Countdown / LED node the user snapped onto the panel band in the   */
/*  editor shows here as a counterpart widget — a seven-segment        */
/*  PnLedDisplay readout or a PnLedLamp indicator.  The applet asks    */
/*  the engine for the widget set and order (GetLayout) and follows it */
/*  (LayoutChanged); each node's live value arrives keyed by node UUID */
/*  (WidgetChanged) as JSON and is pushed into its widget.             */
/*                                                                     */
/*  It is a thin D-Bus client: it links GTK / GLib / json-glib /       */
/*  libxfce4panel only (GDBus comes via gio) and NO pipnode runtime    */
/*  library, so an engine/node crash can never take down xfce4-panel.  */
/*  The only pipnode code it embeds is the GTK/Cairo-only panel-widgets */
/*  convenience library (PnLedDisplay, PnLedLamp), statically linked.   */
/*    - a mouse click drives the PnPanelInput node(s): the click is     */
/*      forwarded (SendEvent) tagged with the button;                  */
/*    - a click on a Switch toggle flips that one node, addressed by    */
/*      UUID (ActivateWidget), instead of the generic input click;      */
/*    - right-click "Properties" opens the worksheet for editing       */
/*      (PresentEditor) — the same running flow, shown.                */
/* ------------------------------------------------------------------ */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libxfce4panel/libxfce4panel.h>

#include "pn-widget-mirror.h"

/* The freedesktop/themed icon name shipped by the main app
 * (data/icons/hicolor/.../org.pipas.pipnode.png). */
#define PIPNODE_ICON_NAME "org.pipas.pipnode"

/* Flatten the panel button in every state: no border, background or
 * box-shadow on hover/focus/active, so the applet shows just its widgets
 * with no frame or highlight when the mouse is over it. */
static const gchar BUTTON_CSS[] =
    "button, button:hover, button:active, button:checked, button:focus {"
    "  background: none;"
    "  background-image: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  outline: none;"
    "  padding: 0;"
    "}";

static void
flatten_button (GtkWidget *button)
{
    GtkCssProvider  *provider;
    GtkStyleContext *context;

    provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, BUTTON_CSS, -1, NULL);

    context = gtk_widget_get_style_context (button);
    gtk_style_context_add_provider (context,
                                    GTK_STYLE_PROVIDER (provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
}

/* The background engine's well-known bus name, object path and the
 * panel control interface (see src/pn-application.c).  The name is
 * D-Bus-activatable, so the first call auto-starts the engine. */
#define PN_ENGINE_BUS    "org.pipas.pipnode"
#define PN_ENGINE_OBJECT "/org/pipas/pipnode"
#define PN_ENGINE_IFACE  "org.pipas.pipnode.Engine"

/* Starter worksheet shipped with the applet: a new applet instance is
 * seeded with a copy of this so the user begins with a working flow
 * rather than a blank sheet.  Installed to pipnode's datadir; see
 * ensure_worksheet(). */
#define PN_DEFAULT_WORKSHEET  PKGDATADIR "/pipnode-xfce-applet-default.json"

/* A blank pipnode document, matching the on-disk format produced by
 * lib/pn-flow.c (PN_FLOW_FILE_VERSION 1).  Used as a fallback when the
 * shipped starter worksheet is missing.  The engine reparses and
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
    GtkWidget       *button;   /* panel button (owned by the plugin)     */
    GtkWidget       *fixed;    /* free-positioning row of widgets + icon  */
    GtkWidget       *image;    /* icon, shown only when no widgets        */
    gint             icon_size;/* panel icon size, applied to new widgets */
    gint             row_h;    /* fixed's allocated height; centres the row */

    /* The mirrored widget set, driven by the engine's layout JSON.  The
     * mirror owns the widgets and their state; the applet owns only where
     * they go — it packs them into a strip (see relayout), ignoring the
     * layout's x so the panel wastes no space on the editor's spacing. */
    PnWidgetMirror  *mirror;

    gchar           *path;     /* this instance's worksheet file          */
    GDBusProxy      *engine;   /* org.pipas.pipnode.Engine proxy          */
    GCancellable    *cancel;   /* cancels in-flight async D-Bus work      */
} PipnodeDeadline;

/* ------------------------------------------------------------------ */
/*  Worksheet location / creation                                      */
/* ------------------------------------------------------------------ */

/* ~/.config/pipnode/panel/<unique-id>.json — one file per applet
 * instance, so two panel buttons keep distinct worksheets. */
static gchar *
worksheet_path (PipnodeDeadline *self)
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

/* Make sure the worksheet file exists, seeding a new one (and its parent
 * directory) on first use.  The seed is a copy of the shipped starter
 * worksheet (PN_DEFAULT_WORKSHEET) so the user begins with a working flow;
 * if that file is missing we fall back to the embedded empty document.
 * Returns FALSE and sets *error on failure. */
static gboolean
ensure_worksheet (const gchar *path,
                  GError     **error)
{
    gchar  *dir;
    gchar  *seed     = NULL;
    gsize   seed_len = 0;
    gboolean ok;

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

    /* Prefer the shipped starter worksheet; the embedded empty document is
     * the fallback when it is not installed. */
    if (g_file_get_contents (PN_DEFAULT_WORKSHEET, &seed, &seed_len, NULL))
    {
        ok = g_file_set_contents (path, seed, seed_len, error);
        g_free (seed);
    }
    else
    {
        ok = g_file_set_contents (path, EMPTY_WORKSHEET,
                                  sizeof EMPTY_WORKSHEET - 1, error);
    }

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Mirrored widgets                                                    */
/* ------------------------------------------------------------------ */

/* Show the icon only while the worksheet contributes no widgets. */
static void
update_empty_state (PipnodeDeadline *self)
{
    gtk_widget_set_visible (self->image,
                            pn_widget_mirror_get_n_widgets (self->mirror) == 0);
}

/* Mirrored readouts render half again the panel's icon size: a panel-height
 * countdown is hard to read, so we trade a little vertical slack for legibility
 * (the row centres the taller widget, see relayout()). */
#define PN_APPLET_WIDGET_SIZE(self) ((self)->icon_size * 3 / 2)

/* Size one widget the right way for its kind: the text-shaped kinds take the
 * full panel row height so their font fills the panel like a clock applet;
 * the other readouts keep the legibility-tuned icon×3/2 size. */
static void
apply_widget_size (PipnodeDeadline *self, guint i)
{
    gboolean fills = pn_widget_mirror_fills_height (self->mirror, i);
    gint     size  = (fills && self->row_h > 0) ? self->row_h
                                                : PN_APPLET_WIDGET_SIZE (self);

    pn_widget_mirror_set_height (self->mirror, i, size);
}

/* A click on a mirrored interactive widget: ask the engine to act on that
 * one node, addressed by its UUID.  For a switch the engine toggles it and
 * echoes the new "on" state back as a WidgetChanged, which the mirror
 * pushes into the toggle.  For an injector the engine fires the node (a
 * one-shot message); there is no state to echo back.  Either way, the
 * widget follows the node — it never tries to guess. */
static void
on_widget_activated (const gchar *uuid, gpointer user_data)
{
    PipnodeDeadline *self = user_data;

    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "ActivateWidget",
                       g_variant_new ("(ss)", self->path, uuid),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* Vertically centre @child within the row, returning the y it was placed at.
 * Shorter-than-row widgets sit mid-band rather than clinging to the top. */
static void
center_in_row (PipnodeDeadline *self, GtkWidget *child, gint x)
{
    gint h = 0;

    gtk_widget_get_preferred_height (child, NULL, &h);
    gtk_fixed_move (GTK_FIXED (self->fixed), child, x,
                    self->row_h > h ? (self->row_h - h) / 2 : 0);
}

/* Small horizontal gap between adjacent mirrored widgets, so the readouts
 * sit beside each other with a little breathing room rather than touching. */
#define PN_APPLET_WIDGET_GAP 4

/* Place the row's widgets in layout order, each a few pixels past where the
 * previous ended (PN_APPLET_WIDGET_GAP), so the panel shows one strip with a
 * little space between widgets, however the editor spaced or grouped them on
 * the band.  The applet mirrors only the order, not the editor's spacing.
 * Each widget — and the fallback icon — is centred vertically in the row's
 * allocated height (see on_fixed_allocate). */
static void
relayout (PipnodeDeadline *self)
{
    guint n = pn_widget_mirror_get_n_widgets (self->mirror);
    guint i;
    gint  pack_x = 0;

    for (i = 0; i < n; i++)
    {
        GtkWidget *w   = pn_widget_mirror_get_widget (self->mirror, i);
        gint       nat = 0;

        if (i > 0)
            pack_x += PN_APPLET_WIDGET_GAP;

        gtk_widget_get_preferred_width (w, NULL, &nat);
        center_in_row (self, w, pack_x);
        pack_x += nat;
    }

    center_in_row (self, self->image, 0);
}

/* Apply the panel's sizes to every mirrored widget. */
static void
resize_widgets (PipnodeDeadline *self)
{
    guint n = pn_widget_mirror_get_n_widgets (self->mirror);
    guint i;

    for (i = 0; i < n; i++)
        apply_widget_size (self, i);
}

/* A fresh layout from the engine: let the mirror reconcile the widget set,
 * then size and pack the row it left us. */
static void
reconcile_layout (PipnodeDeadline *self, const gchar *layout_json)
{
    if (!pn_widget_mirror_reconcile (self->mirror, layout_json))
        return;

    resize_widgets (self);
    relayout (self);
    update_empty_state (self);
}

/* ------------------------------------------------------------------ */
/*  Engine D-Bus calls                                                  */
/* ------------------------------------------------------------------ */

static void
on_get_layout_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    PipnodeDeadline *self  = user_data;
    GVariant        *reply;
    GError          *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: GetLayout failed: %s",
                       error->message);
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
    PipnodeDeadline *self = user_data;
    GVariant         *reply;
    GError           *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: RunWorksheet failed: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }
    g_variant_unref (reply);   /* legacy display value — unused now */

    /* The worksheet is running; pull its panel layout to build the row. */
    g_dbus_proxy_call (self->engine, "GetLayout",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_get_layout_done, self);
}

/* Engine signals arrive here; the engine multiplexes every applet on one
 * interface, so the path argument disambiguates which worksheet they
 * concern.  LayoutChanged rebuilds the widget row; WidgetChanged pushes a
 * single node's fresh state into its widget. */
static void
on_engine_signal (GDBusProxy *proxy,
                  const gchar *sender,
                  const gchar *signal_name,
                  GVariant    *parameters,
                  gpointer     user_data)
{
    PipnodeDeadline *self = user_data;

    (void) proxy;
    (void) sender;

    if (g_strcmp0 (signal_name, "LayoutChanged") == 0)
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
    /* ValueChanged is the legacy single-value channel — ignored here, the
     * per-widget WidgetChanged path carries displays now. */
}

/* Start (or restart) the worksheet on the engine.  A call on a proxy whose
 * name has no owner auto-activates the service file, so this both kicks off
 * the first run and respawns a freshly installed engine after a Quit.  The
 * reply callback then pulls the layout. */
static void
engine_run_worksheet (PipnodeDeadline *self)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "RunWorksheet",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_run_worksheet_done, self);
}

/* The engine owns org.pipas.pipnode.  When that owner vanishes — it quit
 * (our "Reload Worksheet" item), was killed, or crashed — re-run the
 * worksheet: the method call auto-activates the service, starting whatever
 * pipnode-editor binary is now installed, and the reply rebuilds our row.
 * When the owner instead (re)appears there is nothing to do; the pending
 * RunWorksheet already covers it.  This makes every applet self-heal across
 * an engine restart without touching the panel. */
static void
on_engine_owner_changed (GObject    *proxy,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
    PipnodeDeadline *self  = user_data;
    gchar            *owner;

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
    PipnodeDeadline *self = user_data;
    GError           *error = NULL;

    (void) source;

    self->engine = g_dbus_proxy_new_for_bus_finish (result, &error);
    if (self->engine == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: cannot reach the pipnode engine: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }

    g_signal_connect (self->engine, "g-signal",
                      G_CALLBACK (on_engine_signal), self);
    g_signal_connect (self->engine, "notify::g-name-owner",
                      G_CALLBACK (on_engine_owner_changed), self);

    /* Start the worksheet running (auto-activates the engine); the reply
     * callback then pulls the layout. */
    engine_run_worksheet (self);
}

/* Fire-and-forget engine call carrying just the worksheet path. */
static void
engine_call_path (PipnodeDeadline *self, const gchar *method)
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

/* A mouse click on the applet: forward it to the worksheet's Panel Input
 * node(s), tagged with the button, as one "click" event.  Fired on button
 * press (which reaches us for every button, before a right-click hands off
 * to the panel's context menu).  Returns FALSE so the panel still gets the
 * event (the button's own click visuals, and the right-click menu). */
static gboolean
on_button_press (GtkWidget        *button,
                 GdkEventButton   *event,
                 PipnodeDeadline *self)
{
    (void) button;

    if (self->engine == NULL)
        return FALSE;

    /* One click per press; ignore the synthetic GDK_2BUTTON_PRESS /
     * GDK_3BUTTON_PRESS so a double-click counts as two clicks, not four. */
    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    g_dbus_proxy_call (self->engine, "SendEvent",
                       g_variant_new ("(ssu)", self->path, "click",
                                      (guint32) event->button),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
    return FALSE;
}

/* Right-click "Properties": open the running worksheet for editing. */
static void
on_configure_plugin (XfcePanelPlugin  *plugin,
                     PipnodeDeadline *self)
{
    (void) plugin;
    engine_call_path (self, "PresentEditor");
}

/* Right-click "Reload Worksheet": ask the shared engine to quit.  A panel
 * restart reloads the applet but never the long-lived engine, so after
 * `make install` the old binary keeps running; this bounces it.  Every
 * applet watches the bus name (on_engine_owner_changed) and re-runs its
 * worksheet when the owner drops, so one quit cleanly respawns the new
 * binary and reconnects all of them. */
static void
on_restart_engine (GtkMenuItem      *item,
                   PipnodeDeadline *self)
{
    (void) item;

    /* Not connected yet (or a past connect failed): just (re)connect, which
     * auto-activates the engine. */
    if (self->engine == NULL)
    {
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL, PN_ENGINE_BUS, PN_ENGINE_OBJECT,
                                  PN_ENGINE_IFACE, self->cancel,
                                  on_engine_ready, self);
        return;
    }

    g_dbus_proxy_call (self->engine, "Quit", NULL,
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Panel lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* The row's height is only known once the panel allocates it, so re-centre
 * the widgets here whenever it changes.  gtk_fixed_move() queues a resize, so
 * we must act only on a genuine height change or we would loop forever. */
static void
on_fixed_allocate (GtkWidget     *fixed,
                   GtkAllocation *alloc,
                   PipnodeDeadline *self)
{
    (void) fixed;

    if (alloc->height == self->row_h)
        return;

    self->row_h = alloc->height;
    /* The text label is sized to the full row height, so re-apply sizes now
     * that the row height is known, then re-centre. */
    resize_widgets (self);
    relayout (self);
}

static gboolean
on_size_changed (XfcePanelPlugin  *plugin,
                 gint              size,
                 PipnodeDeadline *self)
{
    (void) size;
    self->icon_size = xfce_panel_plugin_get_icon_size (plugin);
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), self->icon_size);
    resize_widgets (self);
    relayout (self);   /* widths changed → repack / refit positions */
    return TRUE;       /* handled */
}

static void
on_free_data (XfcePanelPlugin  *plugin,
              PipnodeDeadline *self)
{
    (void) plugin;

    g_cancellable_cancel (self->cancel);
    g_clear_object (&self->cancel);
    g_clear_object (&self->engine);
    g_clear_pointer (&self->mirror, pn_widget_mirror_free);
    g_clear_pointer (&self->path, g_free);

    g_slice_free (PipnodeDeadline, self);
}

static void
pipnode_xfce_applet_construct (XfcePanelPlugin *plugin)
{
    PipnodeDeadline *self;
    GError           *error = NULL;

    self = g_slice_new0 (PipnodeDeadline);
    self->plugin    = plugin;
    self->cancel    = g_cancellable_new ();
    self->path      = worksheet_path (self);
    self->icon_size = xfce_panel_plugin_get_icon_size (plugin);

    /* Flat, panel-styled button holding a free-positioned row: the icon
     * (shown only when the worksheet has no panel widgets) plus one widget
     * per mirrored Countdown / LED node, placed by relayout() to mirror the
     * editor's band spacing. */
    self->button = xfce_panel_create_button ();
    flatten_button (self->button);
    self->fixed = gtk_fixed_new ();
    self->image = gtk_image_new_from_icon_name (PIPNODE_ICON_NAME,
                                                GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), self->icon_size);
    gtk_fixed_put (GTK_FIXED (self->fixed), self->image, 0, 0);
    gtk_container_add (GTK_CONTAINER (self->button), self->fixed);

    /* The shared mirror owns the widgets on that row; clicks on the
     * interactive ones come back through on_widget_activated. */
    self->mirror = pn_widget_mirror_new (GTK_FIXED (self->fixed),
                                         on_widget_activated, self);

    gtk_widget_set_tooltip_text (self->button,
                                 "Pipnode panel — click to send, "
                                 "right-click → Properties to edit");

    gtk_container_add (GTK_CONTAINER (plugin), self->button);

    /* Let the panel's own context menu pop up over the button. */
    xfce_panel_plugin_add_action_widget (plugin, self->button);

    /* Add the "Properties" entry to the right-click menu (the settings
     * menu) — fires "configure-plugin", which opens the editor. */
    xfce_panel_plugin_menu_show_configure (plugin);

    /* Custom "Reload Worksheet" entry: bounce the shared background engine so
     * a freshly installed pipnode-editor binary takes over (a panel restart
     * reloads only the applet, never the engine).  "Engine" is internal
     * jargon, so the user-facing label talks about the worksheet instead. */
    {
        /* Use a GtkImageMenuItem so the icon sits in the same left-hand
         * column as the panel's own items (Properties, Move, Remove…) and
         * lines up with them — a hand-built box would indent the icon to
         * where their labels start.  The type is deprecated and GTK hides
         * menu images by default, so guard the warning and force the icon
         * on with set_always_show_image.  "view-refresh" is the standard
         * two-arrows-in-a-circle reload glyph. */
        GtkWidget *restart;
        GtkWidget *icon = gtk_image_new_from_icon_name (
                              "view-refresh", GTK_ICON_SIZE_MENU);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        restart = gtk_image_menu_item_new_with_label ("Reload Worksheet");
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (restart), icon);
        gtk_image_menu_item_set_always_show_image (
                GTK_IMAGE_MENU_ITEM (restart), TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS

        gtk_widget_show (restart);
        g_signal_connect (restart, "activate",
                          G_CALLBACK (on_restart_engine), self);
        xfce_panel_plugin_menu_insert_item (plugin, GTK_MENU_ITEM (restart));
    }

    g_signal_connect (self->button, "button-press-event",
                      G_CALLBACK (on_button_press), self);
    g_signal_connect (plugin, "configure-plugin",
                      G_CALLBACK (on_configure_plugin), self);
    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed), self);
    g_signal_connect (self->fixed, "size-allocate",
                      G_CALLBACK (on_fixed_allocate), self);
    g_signal_connect (plugin, "free-data",
                      G_CALLBACK (on_free_data), self);

    gtk_widget_show_all (self->button);  /* image hidden again below */
    update_empty_state (self);

    /* Make sure the file exists, then connect to the engine
     * asynchronously so the panel never blocks on D-Bus. */
    if (!ensure_worksheet (self->path, &error))
    {
        g_warning ("pipnode-xfce-applet: could not create worksheet '%s': %s",
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
XFCE_PANEL_PLUGIN_REGISTER (pipnode_xfce_applet_construct)
