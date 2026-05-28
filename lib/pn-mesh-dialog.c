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
#include "pn-mesh-page-channels.h"
#include "pn-mesh-page-ext-notification.h"
#include "pn-mesh-page-identity.h"
#include "pn-mesh-page-known-nodes.h"
#include "pn-mesh-page-mqtt.h"
#include "pn-mesh-page-region.h"
#include "pn-mesh-page-share.h"
#include "pn-mesh-page-telemetry.h"
#include "pn-mesh-page-test.h"

#define PN_MESH_DIALOG_WIDTH   880
#define PN_MESH_DIALOG_HEIGHT  560
#define PN_MESH_DIALOG_DIVIDER 250
#define PN_MESH_DIALOG_QDATA   "pn-mesh-dialog"

typedef struct
{
    GtkWidget        *dialog;
    GtkWidget        *device_list;
    GtkNotebook      *notebook;        /* always visible; insensitive
                                        * until a device is picked so
                                        * the user can see what tabs
                                        * exist but cannot navigate
                                        * into empty forms */
    GtkWidget        *loading_overlay; /* big spinner + label layered
                                        * over the notebook via a
                                        * GtkOverlay; visible whenever
                                        * the dialog is waiting on the
                                        * device (the want_config_id
                                        * handshake, an admin write,
                                        * the verify-cycle re-read
                                        * after a write) so the user
                                        * cannot switch tabs or
                                        * trigger a second write while
                                        * the serial port is in use */
    GtkSpinner       *loading_spinner;

    /* Reference-counted busy state.  Bumped by the handshake (entered
     * in on_device_activated, dropped in on_connection_ready) and by
     * every page's set_writing transition.  The overlay is shown on
     * the 0→1 edge and hidden on the 1→0 edge so concurrent waiters
     * (theoretical: one handshake + one stale-conn write completing)
     * keep the spinner up until the last one settles. */
    gint              busy_count;

    GtkWidget        *identity_page;
    GtkWidget        *region_page;
    GtkWidget        *channels_page;
    GtkWidget        *share_page;
    GtkWidget        *known_nodes_page;
    GtkWidget        *ext_notification_page;
    GtkWidget        *mqtt_page;
    GtkWidget        *telemetry_page;
    GtkWidget        *test_page;
    GtkLabel         *status_label;

    /* Main-loop timer that drives the Test page's live receive log
     * (Phase 7).  Active only while a connection is live -- started
     * in on_connection_ready, stopped in drop_connection.  The tick
     * handler always drains pending text events from the connection
     * queue (so worker-thread parses during verify-cycles still land
     * in the UI), but only calls pn_mesh_connection_pump_monitor()
     * when busy_count == 0 -- otherwise the timer and a worker would
     * race on the same fd.  0 means "no timer installed". */
    guint             monitor_timer_id;

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

/* Adapter for the page's status callback: the page passes its
 * message first and the user_data second; our internal set_status
 * takes them the other way round.  Bridging via this small thunk
 * is cleaner than a function-pointer cast that would silently
 * swap the arguments and crash on the first call. */
static void
on_page_status (const gchar *msg, gpointer user_data)
{
    set_status ((MeshDialogCtx *) user_data, msg);
}

/* ------------------------------------------------------------------ */
/*  Loading overlay                                                     */
/* ------------------------------------------------------------------ */

/* Show the big spinner over the notebook and lock the notebook so
 * the user cannot switch tabs (or read a half-populated form) while
 * a device round-trip is in flight.  Driven by the busy counter
 * (busy_inc / busy_dec); pages and the handshake share the same
 * overlay so concurrent waiters do not flicker the spinner. */
static void
show_loading (MeshDialogCtx *ctx)
{
    if (ctx->notebook != NULL)
        gtk_widget_set_sensitive (GTK_WIDGET (ctx->notebook), FALSE);
    if (ctx->loading_overlay != NULL)
    {
        gtk_widget_show (ctx->loading_overlay);
        if (ctx->loading_spinner != NULL)
            gtk_spinner_start (ctx->loading_spinner);
    }
}

static void
hide_loading (MeshDialogCtx *ctx)
{
    if (ctx->loading_overlay != NULL)
    {
        if (ctx->loading_spinner != NULL)
            gtk_spinner_stop (ctx->loading_spinner);
        gtk_widget_hide (ctx->loading_overlay);
    }
    /* Re-enable the notebook when there's a live connection.  Earlier
     * pages got away without this because every Apply re-ran the full
     * 3-5 s handshake, leaving the user with the impression that the
     * spinner -> motion -> updated values cycle had handed control
     * back.  The Phase 7 Send path is one frame with no verify cycle
     * (~tens of ms total), so the stuck-insensitive notebook is
     * immediately visible.  Skipping when ctx->connection is NULL
     * keeps the initial drop_connection during build from lighting
     * the notebook up before a device is picked. */
    if (ctx->connection != NULL && ctx->notebook != NULL)
        gtk_widget_set_sensitive (GTK_WIDGET (ctx->notebook), TRUE);
}

/* Bump the busy refcount.  Shows the overlay on the 0→1 edge. */
static void
busy_inc (MeshDialogCtx *ctx)
{
    if (ctx->busy_count++ == 0)
        show_loading (ctx);
}

/* Drop the busy refcount.  Hides the overlay on the 1→0 edge.
 * Clamps at zero so a stray FALSE never drives the counter negative. */
static void
busy_dec (MeshDialogCtx *ctx)
{
    if (ctx->busy_count == 0)
        return;
    if (--ctx->busy_count == 0)
        hide_loading (ctx);
}

/* Page-side busy callback: pages emit this from set_writing() on each
 * TRUE/FALSE transition while a write is in flight.  The dialog turns
 * those into refcount bumps so multiple pages can be (theoretically)
 * waiting simultaneously without one hiding the overlay early. */
static void
on_page_busy (gboolean busy, gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    if (busy)
        busy_inc (ctx);
    else
        busy_dec (ctx);
}

/* ------------------------------------------------------------------ */
/*  Test page monitor timer                                             */
/* ------------------------------------------------------------------ */

#define PN_MESH_DIALOG_MONITOR_INTERVAL_MS 300

static gboolean
on_monitor_tick (gpointer user_data)
{
    MeshDialogCtx   *ctx = user_data;
    PnMeshTextEvent *event;

    /* Always drain queued events: parses that happened on a worker
     * thread during a verify-cycle handshake will have pushed onto
     * the queue and need to surface in the UI even while busy.  */
    if (ctx->connection != NULL && ctx->test_page != NULL)
    {
        while ((event = pn_mesh_connection_take_text_event (ctx->connection))
               != NULL)
        {
            pn_mesh_page_test_drain_event (ctx->test_page, event);
        }
    }

    /* Pump the serial fd only when nothing else is reading from it.
     * busy_count > 0 means a write/handshake worker thread owns the
     * fd; skipping the pump here avoids a same-fd read race that
     * would corrupt the frame reader's running state. */
    if (ctx->busy_count == 0 && ctx->connection != NULL)
        pn_mesh_connection_pump_monitor (ctx->connection);

    return G_SOURCE_CONTINUE;
}

static void
start_monitor_timer (MeshDialogCtx *ctx)
{
    if (ctx->monitor_timer_id != 0)
        return;
    ctx->monitor_timer_id = g_timeout_add (
            PN_MESH_DIALOG_MONITOR_INTERVAL_MS, on_monitor_tick, ctx);
}

static void
stop_monitor_timer (MeshDialogCtx *ctx)
{
    if (ctx->monitor_timer_id == 0)
        return;
    g_source_remove (ctx->monitor_timer_id);
    ctx->monitor_timer_id = 0;
}

/* ------------------------------------------------------------------ */
/*  Connection lifecycle                                                */
/* ------------------------------------------------------------------ */

/* Tear down the live session (if any) and reset the right pane to
 * the empty stack page.  Used on device switch and on dialog close. */
static void
drop_connection (MeshDialogCtx *ctx)
{
    /* Stop the monitor timer before closing the connection so no
     * tick fires against a freed PnMeshConnection. */
    stop_monitor_timer (ctx);

    if (ctx->connection != NULL)
    {
        pn_mesh_connection_close (ctx->connection);
        ctx->connection = NULL;
    }
    g_clear_pointer (&ctx->connection_kind, g_free);
    g_clear_pointer (&ctx->connection_tty,  g_free);
    pn_mesh_page_identity_set_state         (ctx->identity_page,         NULL, NULL, NULL, NULL);
    pn_mesh_page_region_set_state           (ctx->region_page,           NULL, NULL);
    pn_mesh_page_channels_set_state         (ctx->channels_page,         NULL, NULL);
    pn_mesh_page_share_set_state            (ctx->share_page,            NULL);
    pn_mesh_page_known_nodes_set_state      (ctx->known_nodes_page,      NULL);
    pn_mesh_page_ext_notification_set_state (ctx->ext_notification_page, NULL, NULL);
    pn_mesh_page_mqtt_set_state             (ctx->mqtt_page,             NULL, NULL);
    pn_mesh_page_telemetry_set_state        (ctx->telemetry_page,        NULL, NULL);
    pn_mesh_page_test_set_state             (ctx->test_page,             NULL, NULL);
    /* Grey out the notebook (tabs + content) until a device connects.
     * The user still sees every tab so they know what's available
     * once they pick one, but they cannot click into an empty form.
     * The overlay is driven by the busy counter -- pages emit
     * busy(FALSE) from their set_writing(FALSE) transitions during
     * the set_state(NULL) calls above, balancing any in-flight write
     * the previous connection had on the books. */
    if (ctx->notebook != NULL)
        gtk_widget_set_sensitive (GTK_WIDGET (ctx->notebook), FALSE);
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
        /* Balance the busy_inc the cancelled on_device_activated
         * paid in -- on_device_activated for the *new* device has
         * already paid its own inc, so the counter stays at 1 until
         * that new handshake completes. */
        busy_dec (ctx);
        return;
    }

    if (conn == NULL)
    {
        set_statusf (ctx, "Could not connect to %s: %s",
                     ctx->connection_tty != NULL ? ctx->connection_tty : "(?)",
                     error != NULL ? error->message : "(unknown error)");
        g_clear_error (&error);
        /* Tabs stay visible but their content is empty -- each page's
         * set_state(NULL) repaints itself as a "no device" placeholder.
         * Drop the loading overlay so the failure status is readable
         * underneath; notebook stays insensitive (drop_connection set
         * it that way and we never flipped it back). */
        busy_dec (ctx);
        return;
    }

    ctx->connection = conn;
    pn_mesh_page_identity_set_state (
            ctx->identity_page,
            ctx->connection_kind,
            ctx->connection_tty,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_region_set_state (
            ctx->region_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_channels_set_state (
            ctx->channels_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_share_set_state (
            ctx->share_page,
            pn_mesh_connection_get_state (conn));
    pn_mesh_page_known_nodes_set_state (
            ctx->known_nodes_page,
            pn_mesh_connection_get_state (conn));
    pn_mesh_page_ext_notification_set_state (
            ctx->ext_notification_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_mqtt_set_state (
            ctx->mqtt_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_telemetry_set_state (
            ctx->telemetry_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_test_set_state (
            ctx->test_page,
            pn_mesh_connection_get_state (conn),
            conn);
    /* Light up the notebook now that there's actually content to
     * navigate to.  drop_connection() reverses this on the next
     * device switch or dialog close. */
    busy_dec (ctx);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->notebook), TRUE);

    /* Start the Test page receive-log timer.  Ticks every 300 ms,
     * pumping the serial fd whenever no write is in flight, and
     * draining the connection's text-event queue to the Test page
     * unconditionally. */
    start_monitor_timer (ctx);
    /* Jump to the Device tab (index 0) so the Identity expander
     * shows the just-completed handshake. */
    gtk_notebook_set_current_page (ctx->notebook, 0);

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
                                     device->kind, device->tty, NULL, NULL);
    /* The handshake takes 3-5 s and the pages would otherwise paint
     * with em-dashes for everything.  Worse, the user could switch to
     * a tab whose state hasn't arrived yet (Channels/Region/MQTT/...)
     * and read it as "this device has no channels / no MQTT".  Lock
     * the notebook and float a big spinner on top until on_connection
     * _ready (or its cancellation/failure twin) unlocks it.  The
     * counter mirrors the same overlay that pages float during a
     * write, so a stale write completing during a fresh handshake
     * does not hide the spinner out from under the new handshake. */
    busy_inc (ctx);
    /* Snap to the Device tab (index 0) so when the handshake completes
     * the Identity row is what the user sees first. */
    gtk_notebook_set_current_page (ctx->notebook, 0);

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

/* Build one top-level tab: a scrolled vertical box of GtkExpanders.
 * The caller appends one expander per sub-section by passing each
 * (title, child) pair to add_expander() below. */
static GtkWidget *
build_tab (GtkWidget **inner_box_out)
{
    GtkWidget *scrolled;
    GtkWidget *box;

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);
    gtk_widget_set_margin_top    (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_container_add (GTK_CONTAINER (scrolled), box);

    *inner_box_out = box;
    return scrolled;
}

/* Wrap @child in a GtkExpander with @title in its header; pack into
 * @parent.  Defaults to expanded so a freshly-opened tab reveals
 * everything; the user can collapse what they don't want. */
static void
add_expander (GtkWidget *parent, const gchar *title, GtkWidget *child)
{
    GtkWidget *expander = gtk_expander_new (title);

    /* Bolded label looks more like a section heading than a plain
     * expander caption and stays legible alongside the scroll arrows. */
    {
        GtkWidget     *label = gtk_label_new (NULL);
        PangoAttrList *attrs = pango_attr_list_new ();
        gchar         *markup;

        markup = g_markup_printf_escaped ("<b>%s</b>", title);
        gtk_label_set_markup (GTK_LABEL (label), markup);
        g_free (markup);
        pango_attr_list_unref (attrs);
        gtk_expander_set_label_widget (GTK_EXPANDER (expander), label);
    }
    gtk_expander_set_expanded (GTK_EXPANDER (expander), TRUE);
    gtk_container_add (GTK_CONTAINER (expander), child);
    gtk_box_pack_start (GTK_BOX (parent), expander, FALSE, FALSE, 0);
}

/* Placeholder text for a tab whose modules haven't been ported yet
 * (Phases 10/11/12+).  Visible as a dim row so the user knows the
 * tab exists but the contents are pending. */
static GtkWidget *
build_placeholder (const gchar *phase_note)
{
    GtkWidget *label = gtk_label_new (phase_note);
    GtkStyleContext *sc;

    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_widget_set_margin_top    (label, 12);
    gtk_widget_set_margin_bottom (label, 12);
    sc = gtk_widget_get_style_context (label);
    gtk_style_context_add_class (sc, "dim-label");
    return label;
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
    GtkWidget *notebook;

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

    /* Right pane: a GtkNotebook with seven tabs grouped by what the
     * user is trying to do (not by which protobuf tree the setting
     * lives in); each tab is a vertical box of GtkExpanders, one per
     * sub-section.  Tabs / expanders that are empty in this phase
     * carry a placeholder pointing at the phase number that fills
     * them in.  GtkNotebook (rather than GtkStack + GtkStackSwitcher)
     * matches the worksheet's tab visual in the main window: tabs
     * size to their labels, no homogeneous-button stretching, no
     * heavy "view switcher" padding. */
    {
        notebook = gtk_notebook_new ();
        gtk_notebook_set_scrollable (GTK_NOTEBOOK (notebook), TRUE);
        ctx->notebook = GTK_NOTEBOOK (notebook);

        /* Build each page widget once; it is reusable across the
         * dialog's lifetime, repainting via set_state on every
         * device switch. */
        ctx->identity_page         = pn_mesh_page_identity_new ();
        ctx->region_page           = pn_mesh_page_region_new ();
        ctx->channels_page         = pn_mesh_page_channels_new ();
        ctx->share_page            = pn_mesh_page_share_new ();
        ctx->known_nodes_page      = pn_mesh_page_known_nodes_new ();
        ctx->ext_notification_page = pn_mesh_page_ext_notification_new ();
        ctx->mqtt_page             = pn_mesh_page_mqtt_new ();
        ctx->telemetry_page        = pn_mesh_page_telemetry_new ();
        ctx->test_page             = pn_mesh_page_test_new ();

        pn_mesh_page_identity_set_status_callback (
                ctx->identity_page,         on_page_status, ctx);
        pn_mesh_page_region_set_status_callback (
                ctx->region_page,           on_page_status, ctx);
        pn_mesh_page_channels_set_status_callback (
                ctx->channels_page,         on_page_status, ctx);
        pn_mesh_page_ext_notification_set_status_callback (
                ctx->ext_notification_page, on_page_status, ctx);
        pn_mesh_page_mqtt_set_status_callback (
                ctx->mqtt_page,             on_page_status, ctx);
        pn_mesh_page_telemetry_set_status_callback (
                ctx->telemetry_page,        on_page_status, ctx);

        /* Same wiring for the busy sink so every page's write
         * round-trip raises the dialog-wide spinner overlay -- the
         * per-Apply spinner inside the page is too easy to miss and
         * leaves the rest of the notebook clickable while the device
         * is mid-write. */
        pn_mesh_page_identity_set_busy_callback (
                ctx->identity_page,         on_page_busy, ctx);
        pn_mesh_page_region_set_busy_callback (
                ctx->region_page,           on_page_busy, ctx);
        pn_mesh_page_channels_set_busy_callback (
                ctx->channels_page,         on_page_busy, ctx);
        pn_mesh_page_ext_notification_set_busy_callback (
                ctx->ext_notification_page, on_page_busy, ctx);
        pn_mesh_page_mqtt_set_busy_callback (
                ctx->mqtt_page,             on_page_busy, ctx);
        pn_mesh_page_telemetry_set_busy_callback (
                ctx->telemetry_page,        on_page_busy, ctx);
        pn_mesh_page_test_set_busy_callback (
                ctx->test_page,             on_page_busy, ctx);

        /* Tab 1: Device  (Identity now; Device-role + Power in Phase 14/12). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "Identity", ctx->identity_page);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Device"));
        }

        /* Tab 2: Radio  (Region+LoRa, Channels, Share; Position + Security later). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "Region & LoRa", ctx->region_page);
            add_expander (inner, "Channels",      ctx->channels_page);
            add_expander (inner, "Share",         ctx->share_page);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Radio"));
        }

        /* Tab 3: Network  (MQTT now; Serial / Bluetooth / WiFi / Ethernet later). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "MQTT", ctx->mqtt_page);
            gtk_box_pack_start (GTK_BOX (inner),
                    build_placeholder (
                        "Serial, Bluetooth, WiFi and Ethernet land in "
                        "Phases 11/13/14."),
                    FALSE, FALSE, 0);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Network"));
        }

        /* Tab 4: Notifications  (External Notification now; Canned / Status /
         * Audio / Ambient Lighting in Phase 10/11). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "External Notification",
                          ctx->ext_notification_page);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Notifications"));
        }

        /* Tab 5: Telemetry  (Telemetry now; NeighborInfo / DetectionSensor /
         * RangeTest / Paxcounter land in Phase 11+). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "Telemetry", ctx->telemetry_page);
            gtk_box_pack_start (GTK_BOX (inner),
                    build_placeholder (
                        "Neighbor Info, Detection Sensor, Range Test "
                        "and Paxcounter land in later phases."),
                    FALSE, FALSE, 0);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Telemetry"));
        }

        /* Tab 6: Mesh tools  (Phase 11/13). */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            gtk_box_pack_start (GTK_BOX (inner),
                    build_placeholder (
                        "Store & Forward, Traffic Management, Remote "
                        "Hardware, Display config, and TAK land in "
                        "Phase 11/13."),
                    FALSE, FALSE, 0);
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Mesh tools"));
        }

        /* Tab 7: Diagnostics  (Known Nodes + Test, Phase 7).
         * Known Nodes packs FALSE/FALSE so its tree takes natural
         * height; the Test expander packs TRUE/TRUE so the live
         * receive log fills the remaining vertical space below it. */
        {
            GtkWidget *inner, *tab = build_tab (&inner);
            add_expander (inner, "Known Nodes", ctx->known_nodes_page);
            {
                GtkWidget *expander = gtk_expander_new (NULL);
                GtkWidget *label    = gtk_label_new (NULL);
                gchar     *markup;

                markup = g_markup_printf_escaped ("<b>%s</b>", "Test");
                gtk_label_set_markup (GTK_LABEL (label), markup);
                g_free (markup);
                gtk_expander_set_label_widget (GTK_EXPANDER (expander),
                                               label);
                gtk_expander_set_expanded (GTK_EXPANDER (expander), TRUE);
                gtk_container_add (GTK_CONTAINER (expander), ctx->test_page);
                gtk_box_pack_start (GTK_BOX (inner), expander,
                                    TRUE, TRUE, 0);
            }
            gtk_notebook_append_page (GTK_NOTEBOOK (notebook), tab,
                                      gtk_label_new ("Diagnostics"));
        }

        gtk_notebook_set_current_page (GTK_NOTEBOOK (notebook), 0);

        /* Wrap the notebook in a GtkOverlay so we can float a big
         * spinner on top while the dialog is waiting on the device
         * (handshake or any write's verify-cycle).  The notebook
         * stays the bottom (interactive) layer; the spinner+label
         * box is the top (passive) overlay child, set no_show_all so
         * gtk_widget_show_all on the dialog does NOT reveal it --
         * show_loading() flips its visibility. */
        {
            GtkWidget *overlay;
            GtkWidget *box;
            GtkWidget *spinner;
            GtkWidget *label;

            overlay = gtk_overlay_new ();
            gtk_container_add (GTK_CONTAINER (overlay), notebook);

            box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
            /* Centred horizontally + vertically so the spinner lands
             * in the middle of the right pane regardless of dialog
             * size.  halign/valign on the overlay child decides
             * placement; without them the child would fill the whole
             * overlay and the spinner would be top-left. */
            gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
            gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
            gtk_widget_set_no_show_all (box, TRUE);

            spinner = gtk_spinner_new ();
            /* GtkSpinner has no intrinsic size beyond the theme's
             * "spinner-size" — bump it to something a user notices
             * across the whole pane. */
            gtk_widget_set_size_request (spinner, 64, 64);
            gtk_widget_show (spinner);
            gtk_box_pack_start (GTK_BOX (box), spinner, FALSE, FALSE, 0);

            label = gtk_label_new ("Talking to device…");
            gtk_widget_show (label);
            gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

            gtk_overlay_add_overlay (GTK_OVERLAY (overlay), box);

            ctx->loading_overlay = box;
            ctx->loading_spinner = GTK_SPINNER (spinner);

            gtk_paned_pack1 (GTK_PANED (paned), ctx->device_list, FALSE, FALSE);
            gtk_paned_pack2 (GTK_PANED (paned), overlay,          TRUE,  FALSE);
        }
    }

    gtk_box_pack_start (GTK_BOX (content), paned, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), build_status_bar (ctx),
                        FALSE, FALSE, 0);

    gtk_widget_show_all (content);
    /* set_state(NULL) on every page right after build so the initial
     * "no device" placeholders paint immediately; otherwise the user
     * sees stale labels until the first device activation. */
    drop_connection (ctx);
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
