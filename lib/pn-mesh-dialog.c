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
#include "pn-device-dialog.h"
#include "pn-device-form.h"
#include "pn-device-provider.h"
#include "pn-foldable.h"
#include "pn-mesh-connection.h"
#include "pn-mesh-discover.h"
#include "pn-mesh-page-ambient-lighting.h"
#include "pn-mesh-page-bluetooth.h"
#include "pn-mesh-page-channels.h"
#include "pn-mesh-page-device.h"
#include "pn-mesh-page-display.h"
#include "pn-mesh-page-ext-notification.h"
#include "pn-mesh-page-firmware.h"
#include "pn-mesh-page-gps.h"
#include "pn-mesh-page-identity.h"
#include "pn-mesh-page-known-nodes.h"
#include "pn-mesh-page-mqtt.h"
#include "pn-mesh-page-network.h"
#include "pn-mesh-page-position.h"
#include "pn-mesh-page-power.h"
#include "pn-mesh-page-region.h"
#include "pn-mesh-page-security.h"
#include "pn-mesh-page-share.h"
#include "pn-mesh-page-store-forward.h"
#include "pn-mesh-page-telemetry.h"
#include "pn-mesh-page-test.h"

#define PN_MESH_DIALOG_WIDTH   880
#define PN_MESH_DIALOG_HEIGHT  560
#define PN_MESH_DIALOG_QDATA   "pn-mesh-dialog"

typedef struct
{
    GtkWidget        *dialog;

    /* The reusable dialog frame: notebook (insensitive until a device
     * is picked so the user can see what tabs exist but cannot navigate
     * into empty forms), its ref-counted busy-spinner overlay, and the
     * status bar.  The busy overlay is raised by the handshake (entered
     * in on_device_selected, dropped in on_connection_ready) and by
     * every page's set_writing transition; nesting keeps the spinner up
     * until the last waiter settles. */
    PnDeviceDialog   *shell;

    GtkWidget        *identity_page;
    GtkWidget        *region_page;
    GtkWidget        *channels_page;
    GtkWidget        *share_page;
    GtkWidget        *known_nodes_page;
    GtkWidget        *ext_notification_page;
    GtkWidget        *ambient_lighting_page;
    GtkWidget        *store_forward_page;
    GtkWidget        *mqtt_page;
    GtkWidget        *gps_page;
    GtkWidget        *position_page;
    GtkWidget        *power_page;
    GtkWidget        *telemetry_page;
    GtkWidget        *test_page;
    GtkWidget        *device_page;
    GtkWidget        *network_page;
    GtkWidget        *bluetooth_page;
    GtkWidget        *display_page;
    GtkWidget        *security_page;
    GtkWidget        *firmware_page;

    /* Main-loop timer that drives the Test page's live receive log
     * (Phase 7).  Active only while a connection is live -- started
     * in on_connection_ready, stopped in drop_connection.  The tick
     * handler always drains pending text events from the connection
     * queue (so worker-thread parses during verify-cycles still land
     * in the UI), but only calls pn_mesh_connection_pump_monitor()
     * when the shell is not busy -- otherwise the timer and a worker
     * would race on the same fd.  0 means "no timer installed". */
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

    /* Device-list scan (sysfs discovery the shell's list pane asks for
     * via on_device_scan).  scanning guards against a Reload starting a
     * second walk; scan_cancellable cancels an in-flight walk on close. */
    gboolean          scanning;
    GCancellable     *scan_cancellable;
} MeshDialogCtx;

/* ------------------------------------------------------------------ */
/*  Status bar + busy overlay (delegated to the dialog shell)           */
/* ------------------------------------------------------------------ */

static void
set_status (MeshDialogCtx *ctx, const gchar *text)
{
    pn_device_dialog_set_status (ctx->shell, text);
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

/* Page-side busy callback: pages emit this from set_writing() on each
 * TRUE/FALSE transition while a write is in flight.  The shell's
 * ref-counted overlay floats the spinner and locks the notebook for
 * the duration, so multiple pages can be (theoretically) waiting at
 * once without one hiding the overlay early.
 *
 * The shell re-enables the notebook on the 1->0 edge unconditionally;
 * the two cases where it must stay disabled (a fresh open during build,
 * and a connection failure) are handled by an explicit
 * set_pages_sensitive(FALSE) at those sites. */
static void
on_page_busy (gboolean busy, gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    if (busy)
        pn_device_dialog_push_busy (ctx->shell);
    else
        pn_device_dialog_pop_busy (ctx->shell);
}

/* Channels-page changed callback: a channel was added / edited /
 * deleted and the connection's snapshot now differs from what the
 * Share page captured when the device connected.  Re-push the live
 * state so its channel checklist (and the QR / URL it drives) match
 * the Channels page again.  Guard on a live connection: the Channels
 * page only fires this after a verified write, so one is always
 * present, but staying defensive keeps a late callback from handing
 * the Share page a freed state. */
static void
on_channels_changed (gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    if (ctx->connection == NULL)
        return;

    pn_mesh_page_share_set_state (
            ctx->share_page,
            pn_mesh_connection_get_state (ctx->connection));
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
     * A busy shell means a write/handshake worker thread owns the
     * fd; skipping the pump here avoids a same-fd read race that
     * would corrupt the frame reader's running state. */
    if (!pn_device_dialog_is_busy (ctx->shell) && ctx->connection != NULL)
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

    /* Repaint the pages to their "no device" placeholders -- but only
     * while the dialog is live.  On final teardown mesh_dialog_ctx_free
     * NULLs ctx->shell *before* calling us, and by then GTK has already
     * destroyed the page widgets; calling set_state on those freed
     * pointers just spams GTK_IS_WIDGET criticals (the connection close
     * above is the only part that must still run at teardown). */
    if (ctx->shell == NULL)
        return;

    pn_mesh_page_identity_set_state         (ctx->identity_page,         NULL, NULL, NULL, NULL);
    pn_mesh_page_region_set_state           (ctx->region_page,           NULL, NULL);
    pn_mesh_page_channels_set_state         (ctx->channels_page,         NULL, NULL);
    pn_mesh_page_share_set_state            (ctx->share_page,            NULL);
    pn_mesh_page_known_nodes_set_state      (ctx->known_nodes_page,      NULL);
    pn_mesh_page_ext_notification_set_state (ctx->ext_notification_page, NULL, NULL);
    pn_mesh_page_ambient_lighting_set_state (ctx->ambient_lighting_page, NULL, NULL);
    pn_mesh_page_store_forward_set_state    (ctx->store_forward_page,    NULL, NULL);
    pn_mesh_page_mqtt_set_state             (ctx->mqtt_page,             NULL, NULL);
    pn_mesh_page_gps_set_state              (ctx->gps_page,              NULL, NULL);
    pn_mesh_page_position_set_state         (ctx->position_page,         NULL, NULL);
    pn_mesh_page_power_set_state            (ctx->power_page,            NULL, NULL);
    pn_mesh_page_telemetry_set_state        (ctx->telemetry_page,        NULL, NULL);
    pn_mesh_page_test_set_state             (ctx->test_page,             NULL, NULL);
    pn_mesh_page_device_set_state           (ctx->device_page,           NULL, NULL);
    pn_mesh_page_network_set_state          (ctx->network_page,          NULL, NULL);
    pn_mesh_page_bluetooth_set_state        (ctx->bluetooth_page,        NULL, NULL);
    pn_mesh_page_display_set_state          (ctx->display_page,          NULL, NULL);
    pn_mesh_page_security_set_state         (ctx->security_page,         NULL, NULL);
    pn_mesh_page_firmware_set_state         (ctx->firmware_page,         NULL);
    /* Grey out the notebook (tabs + content) until a device connects.
     * The user still sees every tab so they know what's available
     * once they pick one, but they cannot click into an empty form.
     * The shell's overlay is driven by the busy counter -- pages emit
     * busy(FALSE) from their set_writing(FALSE) transitions during
     * the set_state(NULL) calls above, balancing any in-flight write
     * the previous connection had on the books.  This explicit disable
     * at the tail wins over any 1->0 pop that re-enabled the notebook
     * while those balancing transitions ran. */
    pn_device_dialog_set_pages_sensitive (ctx->shell, FALSE);
}

/* Firmware page handed the serial port to the browser flasher: close the
 * whole Meshtastic Devices dialog so the browser's WebSerial can claim the
 * port.  Destroying the dialog runs mesh_dialog_ctx_free -> drop_connection,
 * which closes our fd and frees /dev/tty*.  The user reopens Devices ->
 * Meshtastic and Scans to reconnect once flashing is done. */
static void
on_firmware_close (gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    if (ctx->dialog != NULL)
        gtk_widget_destroy (ctx->dialog);
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
        /* Balance the push the cancelled on_device_selected paid in
         * -- on_device_selected for the *new* device has already paid
         * its own push, so the counter stays at 1 until that new
         * handshake completes. */
        pn_device_dialog_pop_busy (ctx->shell);
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
         * underneath.  The shell re-enables the notebook on this 1->0
         * pop, so disable it again explicitly: a failed connect must
         * leave the empty tabs unnavigable. */
        pn_device_dialog_pop_busy (ctx->shell);
        pn_device_dialog_set_pages_sensitive (ctx->shell, FALSE);
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
    pn_mesh_page_ambient_lighting_set_state (
            ctx->ambient_lighting_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_store_forward_set_state (
            ctx->store_forward_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_mqtt_set_state (
            ctx->mqtt_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_gps_set_state (
            ctx->gps_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_position_set_state (
            ctx->position_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_power_set_state (
            ctx->power_page,
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
    pn_mesh_page_device_set_state (
            ctx->device_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_network_set_state (
            ctx->network_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_bluetooth_set_state (
            ctx->bluetooth_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_display_set_state (
            ctx->display_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_security_set_state (
            ctx->security_page,
            pn_mesh_connection_get_state (conn),
            conn);
    pn_mesh_page_firmware_set_state (
            ctx->firmware_page,
            pn_mesh_connection_get_state (conn));
    /* Light up the notebook now that there's actually content to
     * navigate to.  drop_connection() reverses this on the next
     * device switch or dialog close.  The pop already re-enables on
     * the 1->0 edge; the explicit set keeps it lit even if a stray
     * waiter held the counter above zero. */
    pn_device_dialog_pop_busy (ctx->shell);
    pn_device_dialog_set_pages_sensitive (ctx->shell, TRUE);

    /* Start the Test page receive-log timer.  Ticks every 300 ms,
     * pumping the serial fd whenever no write is in flight, and
     * draining the connection's text-event queue to the Test page
     * unconditionally. */
    start_monitor_timer (ctx);
    /* Jump to the Device tab (index 0) so the Identity expander
     * shows the just-completed handshake. */
    pn_device_dialog_set_current_page (ctx->shell, 0);

    {
        const PnMeshState *st = pn_mesh_connection_get_state (conn);
        set_statusf (ctx, "Connected to %s (%s%s)",
                     ctx->connection_tty,
                     st->config_complete ? "handshake complete"
                                         : "handshake timed out",
                     st->my_node_num != 0 ? ", state read" : "");
    }
}

/* Shell selected callback: a connectable device-list row was activated
 * or auto-selected.  The PnDeviceRow's id is the tty path, its primary
 * line the human-friendly kind -- everything the connect path needs.
 * Tear down whatever is live, set up the Identity page placeholder,
 * kick off the async open. */
static void
on_device_selected (const PnDeviceRow *row, gpointer user_data)
{
    MeshDialogCtx *ctx  = user_data;
    const gchar   *tty  = row->id;
    const gchar   *kind = row->primary;

    /* If this is the same device we are already on (or trying to
     * reach), do nothing -- a rescan re-selecting the live device, or
     * an accidental double-click. */
    if (ctx->connection_tty != NULL
        && g_strcmp0 (ctx->connection_tty, tty) == 0
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

    ctx->connection_kind = g_strdup (kind);
    ctx->connection_tty  = g_strdup (tty);

    /* Show the Identity page in "connecting" form -- kind and tty are
     * known immediately; the rest of the fields stay as em-dashes
     * until the handshake completes. */
    pn_mesh_page_identity_set_state (ctx->identity_page,
                                     kind, tty, NULL, NULL);
    /* The handshake takes 3-5 s and the pages would otherwise paint
     * with em-dashes for everything.  Worse, the user could switch to
     * a tab whose state hasn't arrived yet (Channels/Region/MQTT/...)
     * and read it as "this device has no channels / no MQTT".  Lock
     * the notebook and float a big spinner on top until on_connection
     * _ready (or its cancellation/failure twin) unlocks it.  The
     * counter mirrors the same overlay that pages float during a
     * write, so a stale write completing during a fresh handshake
     * does not hide the spinner out from under the new handshake. */
    pn_device_dialog_push_busy (ctx->shell);
    /* Snap to the Device tab (index 0) so when the handshake completes
     * the Identity row is what the user sees first. */
    pn_device_dialog_set_current_page (ctx->shell, 0);

    set_statusf (ctx, "Connecting to %s on %s…", kind, tty);

    ctx->open_cancellable = g_cancellable_new ();
    pn_mesh_connection_open_async (tty, ctx->open_cancellable,
                                   on_connection_ready, ctx);
}

/* Shell scan callback: build the PnDeviceRow list from the sysfs
 * discovery.  A fresh cancellable per scan; the previous one (if any)
 * is left to settle harmlessly. */
static void on_discover_done (GObject *source, GAsyncResult *result,
                              gpointer user_data);

static void
on_device_scan (gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;

    /* Guard re-entrancy: a Reload while a discover is already running
     * would start a second sysfs walk and double-populate the list. */
    if (ctx->scanning)
        return;
    ctx->scanning = TRUE;

    g_clear_object (&ctx->scan_cancellable);
    ctx->scan_cancellable = g_cancellable_new ();
    pn_mesh_discover_async (ctx->scan_cancellable, on_discover_done, ctx);
}

/* Map a discovered PnMeshDevice onto a PnDeviceRow: tty -> id, kind ->
 * primary, tty -> secondary (shown dim), and the in-use state -> the
 * disabled reason that greys the row out. */
static void
on_discover_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    MeshDialogCtx *ctx = user_data;
    GPtrArray     *devices;
    GPtrArray     *rows;
    GError        *error = NULL;
    guint          i;

    (void) source;

    devices = pn_mesh_discover_finish (result, &error);

    ctx->scanning = FALSE;

    /* Cancelled (dialog closing) or failed: leave the current list
     * untouched -- a transient sysfs hiccup should not blank the rows
     * the user is looking at. */
    if (error != NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pn-mesh-discover: %s", error->message);
        g_clear_error (&error);
        if (devices != NULL)
            g_ptr_array_unref (devices);
        return;
    }

    rows = pn_device_row_array_new ();
    for (i = 0; i < devices->len; i++)
    {
        const PnMeshDevice *d = g_ptr_array_index (devices, i);
        gchar              *reason = NULL;

        if (d->in_use)
            reason = g_strdup_printf (
                    "In use by %s — cannot connect",
                    d->in_use_by != NULL ? d->in_use_by : "another process");
        g_ptr_array_add (rows,
                         pn_device_row_new (d->tty, d->kind, d->tty, reason));
        g_free (reason);
    }
    g_ptr_array_unref (devices);

    pn_device_dialog_set_device_rows (ctx->shell, rows);
    g_ptr_array_unref (rows);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

/* The status bar, busy overlay and per-tab scrolled box / expander
 * sections are now provided by the dialog shell (pn-device-dialog.h)
 * and the form helpers (pn_device_form_new_tab / _add_section).  Only
 * the placeholder below stays mesh-specific. */

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

    /* The reusable shell carries the modeless dialog, Close button,
     * notebook + busy overlay, status bar AND the left device-list pane
     * (PN_DEVICE_DIALOG_WITH_DEVICE_LIST).  We populate its notebook
     * with the seven mesh tabs and feed the list pane from sysfs
     * discovery via the scan / selected callbacks. */
    ctx->shell = pn_device_dialog_new (parent, "Meshtastic Devices",
                                       PN_DEVICE_DIALOG_WITH_DEVICE_LIST);
    dialog = pn_device_dialog_get_dialog (ctx->shell);
    gtk_window_set_default_size (GTK_WINDOW (dialog),
                                 PN_MESH_DIALOG_WIDTH,
                                 PN_MESH_DIALOG_HEIGHT);

    /* Seven tabs grouped by what the user is trying to do (not by which
     * protobuf tree the setting lives in); each tab is a scrolled
     * vertical box of GtkExpanders, one per sub-section.  Tabs /
     * expanders that are empty in this phase carry a placeholder
     * pointing at the phase number that fills them in. */
    {
        /* Build each page widget once; it is reusable across the
         * dialog's lifetime, repainting via set_state on every
         * device switch. */
        ctx->identity_page         = pn_mesh_page_identity_new ();
        ctx->region_page           = pn_mesh_page_region_new ();
        ctx->channels_page         = pn_mesh_page_channels_new ();
        ctx->share_page            = pn_mesh_page_share_new ();
        ctx->known_nodes_page      = pn_mesh_page_known_nodes_new ();
        ctx->ext_notification_page = pn_mesh_page_ext_notification_new ();
        ctx->ambient_lighting_page = pn_mesh_page_ambient_lighting_new ();
        ctx->store_forward_page    = pn_mesh_page_store_forward_new ();
        ctx->mqtt_page             = pn_mesh_page_mqtt_new ();
        ctx->gps_page              = pn_mesh_page_gps_new ();
        ctx->position_page         = pn_mesh_page_position_new ();
        ctx->power_page            = pn_mesh_page_power_new ();
        ctx->telemetry_page        = pn_mesh_page_telemetry_new ();
        ctx->test_page             = pn_mesh_page_test_new ();
        ctx->device_page           = pn_mesh_page_device_new ();
        ctx->network_page          = pn_mesh_page_network_new ();
        ctx->bluetooth_page        = pn_mesh_page_bluetooth_new ();
        ctx->display_page          = pn_mesh_page_display_new ();
        ctx->security_page         = pn_mesh_page_security_new ();
        ctx->firmware_page         = pn_mesh_page_firmware_new ();

        pn_mesh_page_identity_set_status_callback (
                ctx->identity_page,         on_page_status, ctx);
        pn_mesh_page_region_set_status_callback (
                ctx->region_page,           on_page_status, ctx);
        pn_mesh_page_channels_set_status_callback (
                ctx->channels_page,         on_page_status, ctx);
        pn_mesh_page_ext_notification_set_status_callback (
                ctx->ext_notification_page, on_page_status, ctx);
        pn_mesh_page_ambient_lighting_set_status_callback (
                ctx->ambient_lighting_page, on_page_status, ctx);
        pn_mesh_page_store_forward_set_status_callback (
                ctx->store_forward_page,    on_page_status, ctx);
        pn_mesh_page_mqtt_set_status_callback (
                ctx->mqtt_page,             on_page_status, ctx);
        pn_mesh_page_gps_set_status_callback (
                ctx->gps_page,              on_page_status, ctx);
        pn_mesh_page_position_set_status_callback (
                ctx->position_page,         on_page_status, ctx);
        pn_mesh_page_power_set_status_callback (
                ctx->power_page,            on_page_status, ctx);
        pn_mesh_page_telemetry_set_status_callback (
                ctx->telemetry_page,        on_page_status, ctx);
        pn_mesh_page_device_set_status_callback (
                ctx->device_page,           on_page_status, ctx);
        pn_mesh_page_network_set_status_callback (
                ctx->network_page,          on_page_status, ctx);
        pn_mesh_page_bluetooth_set_status_callback (
                ctx->bluetooth_page,        on_page_status, ctx);
        pn_mesh_page_display_set_status_callback (
                ctx->display_page,          on_page_status, ctx);
        pn_mesh_page_security_set_status_callback (
                ctx->security_page,         on_page_status, ctx);

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
        /* Keep the Share page's channel checklist in step with edits
         * made on the Channels page (same Radio tab). */
        pn_mesh_page_channels_set_changed_callback (
                ctx->channels_page,         on_channels_changed, ctx);
        pn_mesh_page_ext_notification_set_busy_callback (
                ctx->ext_notification_page, on_page_busy, ctx);
        pn_mesh_page_ambient_lighting_set_busy_callback (
                ctx->ambient_lighting_page, on_page_busy, ctx);
        pn_mesh_page_store_forward_set_busy_callback (
                ctx->store_forward_page,    on_page_busy, ctx);
        pn_mesh_page_mqtt_set_busy_callback (
                ctx->mqtt_page,             on_page_busy, ctx);
        pn_mesh_page_gps_set_busy_callback (
                ctx->gps_page,              on_page_busy, ctx);
        pn_mesh_page_position_set_busy_callback (
                ctx->position_page,         on_page_busy, ctx);
        pn_mesh_page_power_set_busy_callback (
                ctx->power_page,            on_page_busy, ctx);
        pn_mesh_page_telemetry_set_busy_callback (
                ctx->telemetry_page,        on_page_busy, ctx);
        pn_mesh_page_test_set_busy_callback (
                ctx->test_page,             on_page_busy, ctx);
        pn_mesh_page_device_set_busy_callback (
                ctx->device_page,           on_page_busy, ctx);
        pn_mesh_page_network_set_busy_callback (
                ctx->network_page,          on_page_busy, ctx);
        pn_mesh_page_bluetooth_set_busy_callback (
                ctx->bluetooth_page,        on_page_busy, ctx);
        pn_mesh_page_display_set_busy_callback (
                ctx->display_page,          on_page_busy, ctx);
        pn_mesh_page_security_set_busy_callback (
                ctx->security_page,         on_page_busy, ctx);
        /* Firmware page hands the serial port to the browser flasher;
         * once it has opened the flasher it asks us to close the dialog,
         * whose teardown frees the port. */
        pn_mesh_page_firmware_set_close_callback (
                ctx->firmware_page,         on_firmware_close, ctx);

        /* Tab 1: Device  (Identity + Device role/GPIO + Power + Security). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "Identity",
                    "What the device reported during the configuration "
                    "handshake.  Long / short name changes are written "
                    "live and verified by a round-trip read.",
                    ctx->identity_page);
            pn_device_form_add_section (inner, "Device role & GPIO",
                    "Device role determines how this node behaves on the mesh "
                    "(client, router, repeater, …).  Rebroadcast mode controls "
                    "which packets are forwarded.  Apply writes the whole "
                    "DeviceConfig block; tzdef, buzzer mode and the other "
                    "advanced flags are kept verbatim from the device.",
                    ctx->device_page);
            pn_device_form_add_section (inner, "Power",
                    "Battery-saving behaviour, sleep timings, on-battery "
                    "shutdown threshold and ADC calibration.  The sleep/wake "
                    "timings apply only while power saving is on.  Apply writes "
                    "the whole PowerConfig block at once.  Leave intervals "
                    "at 0 to use the firmware's hardware-specific defaults.",
                    ctx->power_page);
            pn_device_form_add_section (inner, "Security",
                    "Admin and serial-API permissions, plus a read-only view of "
                    "the X25519 keypair the device uses for admin authentication. "
                    " A mistake here can lock you out: enabling \"Managed\" "
                    "without an admin key in the list below disables local "
                    "admin until the device is re-flashed.",
                    ctx->security_page);
            pn_device_form_add_section (inner, "Firmware",
                    "Meshtastic firmware is flashed from the official web "
                    "flasher, which talks to the device directly from your "
                    "browser (Chrome or Edge).  Opening it closes this dialog "
                    "so the browser can use the serial port; reopen "
                    "Devices → Meshtastic and Scan to reconnect once flashing "
                    "is done.",
                    ctx->firmware_page);
            pn_device_dialog_append_page (ctx->shell, tab, "Device");
        }

        /* Tab 2: Radio  (Region+LoRa, Channels, Share, Position). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "Region & LoRa",
                    "Regulatory region, radio preset, hop limit and "
                    "transmit power.  Apply writes the whole LoRa block at "
                    "once; changes to the region or modem preset commonly "
                    "reboot the device for the radio to re-initialise.",
                    ctx->region_page);
            pn_device_form_add_section (inner, "Channels",
                    "Every channel slot the device exposes.  Select a slot to "
                    "edit it below; the primary channel is what nodes talk on "
                    "by default, secondary channels are private groups your "
                    "peers must also configure.  Select an empty slot to add a "
                    "new channel.  Changes affect the device live -- no reboot.",
                    ctx->channels_page);
            pn_device_form_add_section (inner, "Share",
                    "Share this node's channel set with others via a URL or "
                    "QR code.", ctx->share_page);
            pn_device_form_add_section (inner, "GPS Settings",
                    "How this device's GPS is set up: GPS mode, how often it "
                    "samples a fix, the RX/TX/enable GPIO pins that wire the "
                    "GPS module, and which fields each position report "
                    "carries.  A device-specific note above says whether this "
                    "board has GPS and which pins drive it.  Apply writes the "
                    "whole PositionConfig block at once; the broadcast "
                    "settings below are kept verbatim from the device.",
                    ctx->gps_page);
            pn_device_form_add_section (inner, "Position",
                    "How often the device broadcasts its position on the "
                    "mesh: the broadcast interval, smart broadcast and its "
                    "movement thresholds, and whether to use a fixed "
                    "position.  Apply writes the whole PositionConfig block "
                    "at once; the GPS settings above are kept verbatim from "
                    "the device.",
                    ctx->position_page);
            pn_device_dialog_append_page (ctx->shell, tab, "Radio");
        }

        /* Tab 3: Network  (WiFi / Ethernet / IPv6 + Bluetooth + MQTT).
         * Serial deferred to Phase 11 (no donor code). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "WiFi, Ethernet & IPv6",
                    "WiFi, Ethernet and IPv6 settings.  WiFi requires firmware "
                    "built with WiFi support (most ESP32 boards).  Apply writes "
                    "the whole NetworkConfig block; NTP / syslog / static IP "
                    "settings are kept verbatim from the device.",
                    ctx->network_page);
            pn_device_form_add_section (inner, "Bluetooth",
                    "How phones and apps pair with the device over Bluetooth.  "
                    "RANDOM_PIN shows a fresh PIN on the screen each time, "
                    "FIXED_PIN always uses the PIN you set, NO_PIN pairs without "
                    "confirmation.  Apply writes the whole BluetoothConfig block "
                    "at once.",
                    ctx->bluetooth_page);
            pn_device_form_add_section (inner, "MQTT",
                    "Bridges the device's mesh traffic to an MQTT broker so "
                    "internet-connected nodes can forward messages off-radio.  "
                    "Leave Address blank to use the upstream default broker.",
                    ctx->mqtt_page);
            pn_device_dialog_append_page (ctx->shell, tab, "Network");
        }

        /* Tab 4: Notifications  (External Notification + Ambient Lighting;
         * Canned Message / Audio in Phase 11+). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "External Notification",
                    "Drives the device's onboard LED / buzzer / vibration "
                    "motor on incoming traffic.  Set Nag timeout to 0 to "
                    "stop the device repeating notifications.",
                    ctx->ext_notification_page);
            pn_device_form_add_section (inner, "Ambient Lighting",
                    "Drives an addressable RGB status LED (NeoPixel / "
                    "WS2812).  Turn the LED on and pick a colour + drive "
                    "current.",
                    ctx->ambient_lighting_page);
            pn_device_dialog_append_page (ctx->shell, tab, "Notifications");
        }

        /* Tab 5: Telemetry  (Telemetry now; NeighborInfo / DetectionSensor /
         * RangeTest / Paxcounter land in Phase 11+). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "Telemetry",
                    "How often the device broadcasts its own metrics.  Each "
                    "sub-system has its own enable + interval; intervals are "
                    "in seconds (0 disables periodic broadcasts for that "
                    "sub-system).",
                    ctx->telemetry_page);
            gtk_box_pack_start (GTK_BOX (inner),
                    build_placeholder (
                        "Neighbor Info, Detection Sensor, Range Test "
                        "and Paxcounter land in later phases."),
                    FALSE, FALSE, 0);
            pn_device_dialog_append_page (ctx->shell, tab, "Telemetry");
        }

        /* Tab 6: Mesh tools  (Display + Store & Forward now; the rest land
         * in later phases). */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "Display",
                    "On-device screen behaviour: screen timeout, the auto "
                    "carousel, units and coordinate format, display mode and "
                    "OLED driver, plus the orientation and wake toggles.  Apply "
                    "writes the whole DisplayConfig block at once; the compass "
                    "orientation is kept verbatim from the device.",
                    ctx->display_page);
            pn_device_form_add_section (inner, "Store & Forward",
                    "Let a mains-powered, always-on node buffer mesh traffic "
                    "and replay it on request, so nodes that were asleep or "
                    "out of range can catch up.  Enable the module, then turn "
                    "on Is server to make this node the store; the server "
                    "knobs below bound how much it keeps and replays.",
                    ctx->store_forward_page);
            gtk_box_pack_start (GTK_BOX (inner),
                    build_placeholder (
                        "Remote Hardware lands in a later phase."),
                    FALSE, FALSE, 0);
            pn_device_dialog_append_page (ctx->shell, tab, "Mesh tools");
        }

        /* Tab 7: Diagnostics  (Known Nodes + Test, Phase 7).
         * Known Nodes packs FALSE/FALSE so its tree takes natural
         * height; the Test expander packs TRUE/TRUE so the live
         * receive log fills the remaining vertical space below it. */
        {
            GtkWidget *inner, *tab = pn_device_form_new_tab (&inner);
            pn_device_form_add_section (inner, "Known Nodes",
                    "Every node this device has heard from, with signal and "
                    "last-seen details.", ctx->known_nodes_page);
            {
                /* Packed TRUE/TRUE (not via pn_device_form_add_section,
                 * which packs FALSE/FALSE) so the live receive log fills
                 * the vertical space left below Known Nodes. */
                GtkWidget *foldable = pn_foldable_new (
                        "Test",
                        "Send a test message and watch packets arrive live "
                        "from the mesh.");
                gtk_container_add (GTK_CONTAINER (foldable), ctx->test_page);
                gtk_box_pack_start (GTK_BOX (inner), foldable,
                                    TRUE, TRUE, 0);
            }
            pn_device_dialog_append_page (ctx->shell, tab, "Diagnostics");
        }

        pn_device_dialog_set_current_page (ctx->shell, 0);
    }

    /* Wire the list pane: Meshtastic-specific empty-state hints, the
     * sysfs scan + connect callbacks, and the scan-on-open behaviour
     * (reproducing the previous device list's auto-scan). */
    pn_device_dialog_set_list_hints (
            ctx->shell,
            "network-wireless-disabled", "Looking for devices…",
            "Right-click to reload.",
            "dialog-question", "No Meshtastic devices found.",
            "Plug one in and right-click → Reload.");
    pn_device_dialog_set_scan_callback     (ctx->shell, on_device_scan, ctx);
    pn_device_dialog_set_selected_callback (ctx->shell, on_device_selected, ctx);
    set_status (ctx, "Ready. Press Scan to look for connected devices.");
    pn_device_dialog_set_auto_scan (ctx->shell, TRUE);

    gtk_widget_show_all (gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
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
    if (ctx->scan_cancellable != NULL)
    {
        g_cancellable_cancel (ctx->scan_cancellable);
        g_clear_object (&ctx->scan_cancellable);
    }
    /* The shell is a sibling qdata on the same dialog; its free order
     * relative to ours is not guaranteed, so drop our reference before
     * tearing the connection down.  drop_connection's shell calls then
     * become safe no-ops (every pn_device_dialog_* call NULL-checks),
     * and no page's set_state(NULL) busy/status callback reaches a
     * possibly-freed shell. */
    ctx->shell = NULL;
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

/* Provider adapter: the registry's present callback carries a user_data
 * we do not need, so trampoline to the plain present(). */
static void
mesh_provider_present (GtkWindow *parent, gpointer user_data)
{
    (void) user_data;
    pn_mesh_dialog_present (parent);
}

void
pn_mesh_dialog_register_provider (void)
{
    /* "network-wireless" reads as "talk to a radio device" and ships
     * with every common icon theme; the dialog shows the per-device
     * branding once open.  (Matches the icon the hardcoded menu entry
     * used before the registry existed.) */
    pn_device_provider_register ("meshtastic", "Meshtastic",
                                 "network-wireless",
                                 mesh_provider_present, NULL, NULL);
}
