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
/*  GPS Settings page -- the hardware half of PositionConfig.          */
/*                                                                     */
/*  GPS mode combo, the GPS sampling interval, the RX/TX/enable GPIO   */
/*  pins and the position-report flag checkboxes, with a single Apply  */
/*  button that ships the whole PositionConfig at once.  The device-   */
/*  specific "does this board have GPS, and which pins drive it" note   */
/*  lives at the top.                                                   */
/*                                                                     */
/*  PositionConfig is one protobuf message but its settings are split   */
/*  across two sibling pages: this one owns the GPS-hardware fields,    */
/*  the Position page owns the broadcast-behaviour fields.  Each page   */
/*  mirrors the OTHER page's fields at set_state-time and ships them    */
/*  back verbatim on Apply -- proto3-zeroing them would clobber the     */
/*  half the user did not touch.  After either Apply the connection     */
/*  re-handshakes and set_state runs on both pages, so they refresh     */
/*  together.                                                           */
/*                                                                     */
/*  gps_mode supersedes the deprecated gps_enabled boolean upstream;    */
/*  Apply syncs them (mode=ENABLED -> enabled=TRUE) so older firmware   */
/*  still sees a consistent pair.                                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-gps.h"
#include "pn-action-button.h"
#include "pn-mesh-formats.h"

#include "pn-device-combo.h"
#include "pn-device-form.h"
#include "pn-device-spin.h"

#define PN_MESH_GPS_CTX_QDATA "pn-mesh-page-gps-ctx"

/* PositionFlags bits this UI knows; unknown high bits are preserved. */
#define PN_MESH_POSITION_FLAGS_KNOWN_MASK 0x3FFu

typedef struct
{
    GtkComboBoxText *gps_mode_combo;
    GtkSpinButton   *gps_update_secs_spin;
    GtkSpinButton   *rx_gpio_spin;
    GtkSpinButton   *tx_gpio_spin;
    GtkSpinButton   *gps_en_gpio_spin;
    GtkButton       *apply_button;

    /* One toggle per known PositionFlags bit, parallel to POSITION_FLAGS[]. */
    GtkCheckButton  *flag_checks[10];

    /* Device-specific GPS note, derived from the connected hardware
     * model; continues the section help text, hidden until a device
     * connects. */
    GtkLabel        *gps_note_label;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* The Position page's fields, mirrored at set_state-time so this
     * page's Apply ships them back unchanged. */
    gboolean         last_broadcast_smart_enabled;
    gboolean         last_fixed_position;
    guint32          last_broadcast_secs;
    gint32           last_smart_min_distance;
    guint32          last_smart_min_interval_secs;

    /* Unknown PositionFlags bits (outside KNOWN_MASK), preserved across
     * Apply so newer-firmware flags are never silently dropped. */
    guint32          last_unknown_flags;

    gboolean         writing;

    PnMeshGpsStatusFunc status_cb;
    gpointer            status_ud;

    PnMeshPageBusyFunc  busy_cb;
    gpointer            busy_ud;
} GpsCtx;

static void
gps_ctx_free (gpointer data)
{
    g_slice_free (GpsCtx, data);
}

/* ------------------------------------------------------------------ */
/*  GPS mode enum + PositionFlags table                                 */
/* ------------------------------------------------------------------ */

/* Meshtastic GpsMode.  DISABLED is the "no GPS, do not try" mode;
 * NOT_PRESENT is for boards that have no GPS hardware at all (the
 * firmware uses it to suppress the on-screen "no fix" diagnostics). */
static const PnDeviceEnumEntry GPS_MODES[] = {
    { 0, "DISABLED" },
    { 1, "ENABLED" },
    { 2, "NOT_PRESENT" },
};

/* Meshtastic PositionFlags -- which fields each Position packet carries.
 * Order matters: it is parallel to GpsCtx.flag_checks[]. */
typedef struct { guint32 bit; const char *label; const char *tooltip; } FlagEntry;

static const FlagEntry POSITION_FLAGS[] = {
    { 1u << 0, "Altitude",
      "Include altitude in the position report." },
    { 1u << 1, "Altitude is MSL",
      "Report altitude above mean sea level rather than the WGS84 "
      "ellipsoid." },
    { 1u << 2, "Geoidal separation",
      "Include the geoidal separation (ellipsoid-to-MSL offset)." },
    { 1u << 3, "Dilution of precision",
      "Include the combined position DOP (PDOP)." },
    { 1u << 4, "HDOP/VDOP",
      "Report horizontal and vertical DOP separately instead of the "
      "combined PDOP." },
    { 1u << 5, "Satellites in view",
      "Include the number of satellites used for the fix." },
    { 1u << 6, "Sequence number",
      "Include an incrementing sequence number on each report." },
    { 1u << 7, "Timestamp",
      "Include the GPS timestamp of the fix." },
    { 1u << 8, "Heading",
      "Include the course/heading over ground." },
    { 1u << 9, "Speed",
      "Include the ground speed." },
};

/* ------------------------------------------------------------------ */
/*  Status + writing flag                                               */
/* ------------------------------------------------------------------ */

static void
emit_status (GpsCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* Set the device-specific GPS note from the connected hardware model:
 * whether THIS board has GPS and the GPIO wiring that makes it work.
 * Best-effort "internet sources" data (firmware variant.h), openly
 * flagged as possibly wrong.  Hidden when no device is connected. */
static void
update_gps_note (GpsCtx *ctx, const PnMeshState *state)
{
    guint32      hw;
    gchar       *model;
    const gchar *note;
    const gchar *gpio;
    gchar       *text;

    if (state == NULL)
    {
        gtk_widget_hide (GTK_WIDGET (ctx->gps_note_label));
        return;
    }

    /* Same resolution the Identity page uses: prefer DeviceMetadata's
     * hw_model, fall back to the owner User record's. */
    hw = state->have_metadata && state->hw_model != 0
            ? state->hw_model : state->owner_hw_model;
    if (hw == 0)
    {
        gtk_widget_hide (GTK_WIDGET (ctx->gps_note_label));
        return;
    }

    model = pn_mesh_format_hw_model (hw);
    note  = pn_mesh_hw_gps_note (hw);
    gpio  = pn_mesh_hw_gps_gpio (hw);

    switch (pn_mesh_hw_gps (hw))
    {
    case PN_MESH_GPS_BUILTIN:
        text = g_strdup_printf (
                "According to internet sources, this device (%s) has %s. For "
                "the GPS to work the firmware drives %s; leave the GPS GPIO "
                "fields below at 0 to keep these defaults. "
                "Internet sources can be wrong.",
                model, note, gpio != NULL ? gpio : "its built-in GPS pins");
        break;
    case PN_MESH_GPS_HEADER:
        if (gpio != NULL)
            text = g_strdup_printf (
                    "According to internet sources, this device (%s) has %s — "
                    "no GPS chip is soldered on, but you can attach an external "
                    "module. The board pre-wires %s; otherwise set the GPS "
                    "RX/TX GPIO fields below to match your wiring. "
                    "Internet sources can be wrong.",
                    model, note, gpio);
        else
            text = g_strdup_printf (
                    "According to internet sources, this device (%s) has %s — "
                    "no GPS chip is soldered on. Attach an external GPS module "
                    "and set the GPS RX/TX GPIO fields below to match your "
                    "wiring. Internet sources can be wrong.",
                    model, note);
        break;
    case PN_MESH_GPS_NONE:
        text = g_strdup_printf (
                "According to internet sources, this device (%s) has %s, so "
                "live GPS positioning is not available — only a manually set "
                "fixed position (on the Position section below) will work. "
                "Internet sources can be wrong.",
                model, note);
        break;
    case PN_MESH_GPS_UNKNOWN:
    default:
        text = g_strdup_printf (
                "Whether this device (%s) has GPS, and which GPIO pins it "
                "uses, could not be determined from internet sources, which "
                "can be wrong.",
                model);
        break;
    }

    gtk_label_set_text (ctx->gps_note_label, text);
    gtk_widget_show (GTK_WIDGET (ctx->gps_note_label));

    g_free (text);
    g_free (model);
}

static void
set_writing (GpsCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    gsize    i;

    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->gps_mode_combo),       enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->gps_update_secs_spin), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->rx_gpio_spin),         enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->tx_gpio_spin),         enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->gps_en_gpio_spin),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),         enable);
    for (i = 0; i < G_N_ELEMENTS (POSITION_FLAGS); i++)
        gtk_widget_set_sensitive (GTK_WIDGET (ctx->flag_checks[i]), enable);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

/* Collect the editable controls + the mirrored broadcast fields into a
 * full PositionConfig write payload. */
static void
build_position_config (GpsCtx *ctx, PnMeshPositionConfigWrite *cfg)
{
    guint32 mode  = pn_device_form_combo_get_id (ctx->gps_mode_combo,
                                                 1 /* ENABLED */);
    guint32 flags = 0;
    gsize   i;

    for (i = 0; i < G_N_ELEMENTS (POSITION_FLAGS); i++)
        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (ctx->flag_checks[i])))
            flags |= POSITION_FLAGS[i].bit;
    /* Keep any bits newer firmware set that we have no checkbox for. */
    flags |= ctx->last_unknown_flags;

    /* GPS-hardware fields owned by this page. */
    cfg->gps_mode    = mode;
    /* Keep the legacy gps_enabled boolean in sync with gps_mode. */
    cfg->gps_enabled = (mode == 1);
    cfg->gps_update_interval =
            (guint32) gtk_spin_button_get_value_as_int (ctx->gps_update_secs_spin);
    cfg->rx_gpio     = (guint32) gtk_spin_button_get_value_as_int (ctx->rx_gpio_spin);
    cfg->tx_gpio     = (guint32) gtk_spin_button_get_value_as_int (ctx->tx_gpio_spin);
    cfg->gps_en_gpio = (guint32) gtk_spin_button_get_value_as_int (ctx->gps_en_gpio_spin);
    cfg->position_flags = flags;

    /* Broadcast fields owned by the Position page -- shipped back verbatim. */
    cfg->position_broadcast_smart_enabled  = ctx->last_broadcast_smart_enabled;
    cfg->fixed_position                    = ctx->last_fixed_position;
    cfg->position_broadcast_secs           = ctx->last_broadcast_secs;
    cfg->broadcast_smart_min_distance      = ctx->last_smart_min_distance;
    cfg->broadcast_smart_min_interval_secs = ctx->last_smart_min_interval_secs;
}

/* ------------------------------------------------------------------ */
/*  Apply + live GPS test (modal)                                       */
/* ------------------------------------------------------------------ */

/* Gap between GPS probes while we wait for a fix / satellites. */
#define PN_MESH_GPS_PROBE_GAP_MS 1500

/* The whole save-then-test flow, driven by a modal dialog.  One of
 * these lives for the duration of the modal; it is freed by
 * gps_test_finalize() once no worker thread is still in flight. */
typedef struct
{
    GtkWidget        *page;          /* the GPS page (borrowed) */
    PnMeshConnection *connection;    /* borrowed */
    PnMeshPositionConfigWrite cfg;

    GCancellable     *cancellable;
    GtkWidget        *dialog;
    GtkWidget        *spinner;
    GtkLabel         *phase;
    GtkLabel         *sats_value;
    GtkLabel         *fix_value;
    GtkLabel         *time_value;
    GtkButton        *action_button; /* Cancel -> Close */

    /* Mirror of the page's busy sink: held TRUE for the whole test so
     * the dialog's 300 ms monitor pump never races our worker threads
     * on the serial fd. */
    PnMeshPageBusyFunc busy_cb;
    gpointer           busy_ud;

    guint    poll_id;          /* inter-probe timeout, 0 if none */
    gboolean async_pending;    /* a save/probe worker is in flight */
    gboolean closing;          /* user dismissed; finalize when safe */
    gboolean finished;         /* a terminal result is shown */
    gboolean finalized;        /* gps_test_finalize ran (run-once guard) */
} GpsTest;

static void start_probe (GpsTest *test);

static void
gps_test_finalize (GpsTest *test)
{
    if (test->finalized)
        return;
    test->finalized = TRUE;

    if (test->poll_id != 0)
    {
        g_source_remove (test->poll_id);
        test->poll_id = 0;
    }

    /* Release the busy overlay only now -- a worker thread may have
     * owned the serial fd right up to here; popping earlier would let
     * the monitor pump read concurrently. */
    if (test->busy_cb != NULL)
        test->busy_cb (FALSE, test->busy_ud);

    /* Repaint the page from whatever the last handshake left in state. */
    if (test->connection != NULL)
        pn_mesh_page_gps_set_state (
                test->page,
                pn_mesh_connection_get_state (test->connection),
                test->connection);

    if (test->dialog != NULL)
    {
        GtkWidget *d = test->dialog;
        test->dialog = NULL;
        gtk_widget_destroy (d);
    }

    g_clear_object (&test->cancellable);
    g_slice_free (GpsTest, test);
}

static gchar *
gps_fix_text (guint32 fix_type)
{
    switch (fix_type)
    {
    case 0:
    case 1:  return g_strdup ("no fix yet");
    case 2:  return g_strdup ("2D fix");
    case 3:  return g_strdup ("3D fix");
    case 4:  return g_strdup ("3D + dead reckoning");
    case 5:  return g_strdup ("time only");
    default: return g_strdup_printf ("type %u", fix_type);
    }
}

static gchar *
gps_time_text (guint32 secs)
{
    GDateTime *dt;
    gchar     *out;

    if (secs == 0)
        return g_strdup ("—");
    dt = g_date_time_new_from_unix_local ((gint64) secs);
    if (dt == NULL)
        return g_strdup_printf ("%u", secs);
    out = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref (dt);
    return out;
}

static void
gps_test_update_readout (GpsTest *test, const PnMeshGpsProbe *p)
{
    gchar *s;

    s = g_strdup_printf ("%u", p->sats_in_view);
    gtk_label_set_text (test->sats_value, s);
    g_free (s);

    s = gps_fix_text (p->fix_type);
    gtk_label_set_text (test->fix_value, s);
    g_free (s);

    s = gps_time_text (p->time);
    gtk_label_set_text (test->time_value, s);
    g_free (s);
}

/* Switch the dialog to its terminal state: stop the spinner, show a
 * final message and turn the Cancel button into Close. */
static void
gps_test_conclude (GpsTest *test, const gchar *phase_markup)
{
    gtk_spinner_stop (GTK_SPINNER (test->spinner));
    gtk_widget_hide  (test->spinner);
    gtk_label_set_markup (test->phase, phase_markup);
    gtk_button_set_label (test->action_button, "_Close");
    gtk_button_set_use_underline (test->action_button, TRUE);
    test->finished = TRUE;
}

/* Update the readout and decide whether the GPS has proven itself.  A
 * GPS that is wired and talking reports satellites-in-view well before
 * it gets a fix, so either is enough to declare success.  Returns TRUE
 * if the test concluded. */
static gboolean
gps_test_consider (GpsTest *test, const PnMeshGpsProbe *p)
{
    gps_test_update_readout (test, p);

    if (p->device_responded &&
        (p->sats_in_view > 0 || p->fix_type >= 2))
    {
        gps_test_conclude (test,
                "<b>GPS is working ✓</b>\n"
                "The device is talking to its GPS module.");
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_probe_gap_elapsed (gpointer user_data)
{
    GpsTest *test = user_data;

    test->poll_id = 0;
    if (test->closing || test->finished)
        return G_SOURCE_REMOVE;

    start_probe (test);
    return G_SOURCE_REMOVE;
}

static void
on_probe_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GpsTest        *test = user_data;
    PnMeshGpsProbe  p;
    GError         *error = NULL;

    (void) source;

    test->async_pending = FALSE;
    if (test->closing)
    {
        gps_test_finalize (test);
        return;
    }

    if (!pn_mesh_connection_probe_gps_finish (res, &p, &error))
    {
        gchar *m;
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_clear_error (&error);
            gps_test_finalize (test);
            return;
        }
        m = g_markup_printf_escaped (
                "<b>Lost contact with the device:</b> %s",
                error != NULL ? error->message : "unknown error");
        gps_test_conclude (test, m);
        g_free (m);
        g_clear_error (&error);
        return;
    }

    if (gps_test_consider (test, &p))
        return;

    /* Not yet -- keep waiting. */
    if (!p.device_responded)
        gtk_label_set_markup (test->phase,
                "Waiting for the device to come back…");
    else
        gtk_label_set_markup (test->phase,
                "Listening for the GPS… (this can take a while; "
                "a fix needs a clear view of the sky)");

    test->poll_id = g_timeout_add (PN_MESH_GPS_PROBE_GAP_MS,
                                   on_probe_gap_elapsed, test);
}

static void
start_probe (GpsTest *test)
{
    test->async_pending = TRUE;
    pn_mesh_connection_probe_gps_async (
            test->connection, test->cancellable, on_probe_done, test);
}

static void
on_save_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GpsTest        *test = user_data;
    GError         *error = NULL;
    const PnMeshState *state;
    PnMeshGpsProbe  p;

    (void) source;

    test->async_pending = FALSE;
    if (test->closing)
    {
        gps_test_finalize (test);
        return;
    }

    if (!pn_mesh_connection_set_position_config_finish (res, &error))
    {
        gchar *m;
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_clear_error (&error);
            gps_test_finalize (test);
            return;
        }
        m = g_markup_printf_escaped (
                "<b>Could not save GPS settings:</b> %s",
                error != NULL ? error->message : "unknown error");
        gps_test_conclude (test, m);
        g_free (m);
        g_clear_error (&error);
        return;
    }

    /* The write's verify-handshake already re-read the node DB, so the
     * first live snapshot is sitting in state -- show it immediately for
     * snappy feedback before settling into the probe loop. */
    gtk_label_set_markup (test->phase, "Saved. Listening for the GPS…");

    state = pn_mesh_connection_get_state (test->connection);
    p.device_responded = TRUE;
    p.have_position    = state->gps_have_live_position;
    p.time             = state->gps_live_time;
    p.fix_type         = state->gps_live_fix_type;
    p.sats_in_view     = state->gps_live_sats_in_view;

    if (gps_test_consider (test, &p))
        return;

    start_probe (test);
}

static void
on_test_response (GtkDialog *dialog, gint response_id, gpointer user_data)
{
    GpsTest *test = user_data;

    (void) response_id;

    g_cancellable_cancel (test->cancellable);
    if (test->poll_id != 0)
    {
        g_source_remove (test->poll_id);
        test->poll_id = 0;
    }
    test->closing = TRUE;

    if (test->async_pending)
    {
        /* A worker still owns the serial; hide now for responsiveness
         * and let its callback finalize once it returns. */
        gtk_widget_hide (GTK_WIDGET (dialog));
        return;
    }

    gps_test_finalize (test);
}

/* Build a labelled value row in @grid and return the (selectable) value
 * label so it can be updated as the test runs. */
static GtkLabel *
gps_test_add_row (GtkGrid *grid, gint row, const gchar *key)
{
    GtkWidget *key_label = gtk_label_new (key);
    GtkWidget *value     = gtk_label_new ("—");

    gtk_label_set_xalign (GTK_LABEL (key_label), 0.0);
    gtk_widget_set_margin_end (key_label, 12);
    gtk_grid_attach (grid, key_label, 0, row, 1, 1);

    gtk_label_set_xalign (GTK_LABEL (value), 0.0);
    gtk_label_set_selectable (GTK_LABEL (value), TRUE);
    gtk_grid_attach (grid, value, 1, row, 1, 1);

    return GTK_LABEL (value);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget    *page = user_data;
    GpsCtx       *ctx  = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_GPS_CTX_QDATA);
    GpsTest      *test;
    GtkWidget    *top;
    GtkWindow    *parent;
    GtkWidget    *content;
    GtkWidget    *header;
    GtkWidget    *grid;
    GtkWidget    *note;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    emit_status (ctx, "Saving GPS settings and testing the GPS…");

    test = g_slice_new0 (GpsTest);
    test->page       = page;
    test->connection = ctx->connection;
    test->busy_cb    = ctx->busy_cb;
    test->busy_ud    = ctx->busy_ud;
    test->cancellable = g_cancellable_new ();
    build_position_config (ctx, &test->cfg);

    /* ---- modal dialog ---- */
    top    = gtk_widget_get_toplevel (page);
    parent = GTK_IS_WINDOW (top) ? GTK_WINDOW (top) : NULL;

    test->dialog = gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (test->dialog), "GPS test");
    if (parent != NULL)
        gtk_window_set_transient_for (GTK_WINDOW (test->dialog), parent);
    gtk_window_set_modal (GTK_WINDOW (test->dialog), TRUE);
    gtk_window_set_default_size (GTK_WINDOW (test->dialog), 440, -1);
    test->action_button = GTK_BUTTON (gtk_dialog_add_button (
            GTK_DIALOG (test->dialog), "_Cancel", GTK_RESPONSE_CANCEL));
    gtk_button_set_use_underline (test->action_button, TRUE);

    content = gtk_dialog_get_content_area (GTK_DIALOG (test->dialog));
    gtk_orientable_set_orientation (GTK_ORIENTABLE (content),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing (GTK_BOX (content), 12);
    gtk_widget_set_margin_start  (content, 18);
    gtk_widget_set_margin_end    (content, 18);
    gtk_widget_set_margin_top    (content, 18);
    gtk_widget_set_margin_bottom (content, 12);

    header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    test->spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (test->spinner));
    gtk_box_pack_start (GTK_BOX (header), test->spinner, FALSE, FALSE, 0);
    {
        GtkWidget *phase = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (phase), "Saving GPS settings…");
        gtk_label_set_xalign (GTK_LABEL (phase), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (phase), TRUE);
        gtk_box_pack_start (GTK_BOX (header), phase, TRUE, TRUE, 0);
        test->phase = GTK_LABEL (phase);
    }
    gtk_box_pack_start (GTK_BOX (content), header, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    test->sats_value = gps_test_add_row (GTK_GRID (grid), 0,
                                         "Satellites in view:");
    test->fix_value  = gps_test_add_row (GTK_GRID (grid), 1, "Fix:");
    test->time_value = gps_test_add_row (GTK_GRID (grid), 2, "GPS time:");
    gtk_box_pack_start (GTK_BOX (content), grid, FALSE, FALSE, 0);

    note = gtk_label_new (
            "Satellites can take a minute or more, and a full fix needs a "
            "clear view of the sky. Seeing any satellites means the GPS is "
            "wired up and talking to the device.");
    gtk_label_set_xalign (GTK_LABEL (note), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (note), TRUE);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (note);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (content), note, FALSE, FALSE, 0);

    g_signal_connect (test->dialog, "response",
                      G_CALLBACK (on_test_response), test);

    /* Hold the dialog's busy overlay for the whole test so the monitor
     * pump backs off the serial while our worker threads use it. */
    if (test->busy_cb != NULL)
        test->busy_cb (TRUE, test->busy_ud);

    gtk_widget_show_all (test->dialog);

    test->async_pending = TRUE;
    pn_mesh_connection_set_position_config_async (
            test->connection, &test->cfg, test->cancellable,
            on_save_done, test);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

/* Attach a small dim-label "unit suffix" after a control in @cell. */
static void
attach_unit (GtkWidget *cell, const gchar *unit)
{
    GtkWidget       *suffix = gtk_label_new (unit);
    GtkStyleContext *sc     = gtk_widget_get_style_context (suffix);
    gtk_style_context_add_class (sc, "dim-label");
    gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
}

static GtkSpinButton *
add_gpio_row (GtkGrid *grid, gint row, const gchar *key, const gchar *tip)
{
    GtkWidget *cell = pn_device_form_attach_control_row (grid, row, key);
    GtkWidget *spin = pn_device_spin_new_with_range (0, 255, 1);
    gtk_widget_set_tooltip_text (spin, tip);
    gtk_box_pack_start (GTK_BOX (cell), spin, FALSE, FALSE, 0);
    return GTK_SPIN_BUTTON (spin);
}

GtkWidget *
pn_mesh_page_gps_new (void)
{
    GtkWidget *page;
    GtkWidget *grid;
    GtkWidget *cell;
    GtkWidget *gps_mode;
    GtkWidget *gps_update_secs;
    GtkWidget *gps_note;
    GtkWidget *flags_header;
    GtkWidget *flags_box;
    GtkWidget *action_bar;
    GtkWidget *apply;
    GpsCtx    *ctx;
    gint       row = 0;
    gsize      i;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    ctx = g_slice_new0 (GpsCtx);

    /* Device-specific GPS note -- continues the section's help text.
     * Filled in by update_gps_note() once a device connects; starts
     * hidden so it doesn't show a stale line before the handshake. */
    gps_note = gtk_label_new (NULL);
    gtk_label_set_xalign    (GTK_LABEL (gps_note), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (gps_note), TRUE);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (gps_note);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_widget_set_no_show_all (gps_note, TRUE);
    gtk_box_pack_start (GTK_BOX (page), gps_note, FALSE, FALSE, 0);
    ctx->gps_note_label = GTK_LABEL (gps_note);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    cell = pn_device_form_attach_control_row (GTK_GRID (grid), row++, "GPS mode");
    gps_mode = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (gps_mode,
            "ENABLED keeps the GPS running and uses real fixes; "
            "DISABLED turns it off; NOT_PRESENT tells the firmware "
            "the board has no GPS hardware (suppresses on-screen "
            "fix diagnostics).");
    pn_device_form_combo_fill (GTK_COMBO_BOX_TEXT (gps_mode),
                               GPS_MODES, G_N_ELEMENTS (GPS_MODES));
    gtk_box_pack_start (GTK_BOX (cell), gps_mode, FALSE, FALSE, 0);
    ctx->gps_mode_combo = GTK_COMBO_BOX_TEXT (gps_mode);

    cell = pn_device_form_attach_control_row (GTK_GRID (grid), row++,
                                              "GPS update interval");
    /* 0 = device default; cap at 86400 (one day). */
    gps_update_secs = pn_device_spin_new_with_range (0, 86400, 30);
    gtk_widget_set_tooltip_text (gps_update_secs,
            "Seconds between GPS sampling attempts.  0 lets the "
            "firmware pick its default.");
    gtk_box_pack_start (GTK_BOX (cell), gps_update_secs, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->gps_update_secs_spin = GTK_SPIN_BUTTON (gps_update_secs);

    ctx->rx_gpio_spin = add_gpio_row (GTK_GRID (grid), row++, "GPS RX GPIO",
            "GPIO pin the device's UART receives GPS data on.  "
            "0 keeps the firmware's board default.");
    ctx->tx_gpio_spin = add_gpio_row (GTK_GRID (grid), row++, "GPS TX GPIO",
            "GPIO pin the device's UART sends to the GPS on.  "
            "0 keeps the firmware's board default.");
    ctx->gps_en_gpio_spin = add_gpio_row (GTK_GRID (grid), row++, "GPS enable GPIO",
            "GPIO pin that powers / enables the GPS module.  "
            "0 keeps the firmware's board default (or none).");

    /* Position-report flags -- which fields each Position packet carries. */
    flags_header = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (flags_header),
                          "<b>Report in position packets</b>");
    gtk_label_set_xalign (GTK_LABEL (flags_header), 0.0);
    gtk_widget_set_margin_top (flags_header, 10);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (flags_header);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), flags_header, FALSE, FALSE, 0);

    flags_box = gtk_flow_box_new ();
    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flags_box),
                                     GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flags_box), 3);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flags_box), 12);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flags_box), TRUE);
    gtk_box_pack_start (GTK_BOX (page), flags_box, FALSE, FALSE, 0);

    for (i = 0; i < G_N_ELEMENTS (POSITION_FLAGS); i++)
    {
        GtkWidget *check = gtk_check_button_new_with_label (POSITION_FLAGS[i].label);
        gtk_widget_set_tooltip_text (check, POSITION_FLAGS[i].tooltip);
        gtk_flow_box_insert (GTK_FLOW_BOX (flags_box), check, -1);
        ctx->flag_checks[i] = GTK_CHECK_BUTTON (check);
    }

    action_bar = pn_device_form_add_action_bar (page);
    apply = pn_action_button_new ("_Apply GPS settings", PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (action_bar), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_GPS_CTX_QDATA,
                            ctx, gps_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_gps_set_state (GtkWidget         *page,
                            const PnMeshState *state,
                            PnMeshConnection  *connection)
{
    GpsCtx *ctx;
    gsize   i;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_GPS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    /* The GPS note depends only on the hardware model, which arrives
     * with the handshake even when no PositionConfig has streamed yet. */
    update_gps_note (ctx, state);

    if (state == NULL || !state->have_position)
    {
        ctx->last_broadcast_smart_enabled  = FALSE;
        ctx->last_fixed_position           = FALSE;
        ctx->last_broadcast_secs           = 0;
        ctx->last_smart_min_distance       = 0;
        ctx->last_smart_min_interval_secs  = 0;
        ctx->last_unknown_flags            = 0;
        set_writing (ctx, FALSE);
        return;
    }

    /* Mirror the Position page's fields for verbatim round-trip. */
    ctx->last_broadcast_smart_enabled  = state->pos_position_broadcast_smart_enabled;
    ctx->last_fixed_position           = state->pos_fixed_position;
    ctx->last_broadcast_secs           = state->pos_position_broadcast_secs;
    ctx->last_smart_min_distance       = state->pos_broadcast_smart_min_distance;
    ctx->last_smart_min_interval_secs  = state->pos_broadcast_smart_min_interval_secs;
    ctx->last_unknown_flags =
            state->pos_position_flags & ~PN_MESH_POSITION_FLAGS_KNOWN_MASK;

    pn_device_form_combo_select (ctx->gps_mode_combo, GPS_MODES,
                                 G_N_ELEMENTS (GPS_MODES), state->pos_gps_mode);
    gtk_spin_button_set_value (ctx->gps_update_secs_spin,
                               (gdouble) state->pos_gps_update_interval);
    gtk_spin_button_set_value (ctx->rx_gpio_spin,
                               (gdouble) state->pos_rx_gpio);
    gtk_spin_button_set_value (ctx->tx_gpio_spin,
                               (gdouble) state->pos_tx_gpio);
    gtk_spin_button_set_value (ctx->gps_en_gpio_spin,
                               (gdouble) state->pos_gps_en_gpio);

    for (i = 0; i < G_N_ELEMENTS (POSITION_FLAGS); i++)
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (ctx->flag_checks[i]),
                (state->pos_position_flags & POSITION_FLAGS[i].bit) != 0);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_gps_set_status_callback (GtkWidget           *page,
                                      PnMeshGpsStatusFunc  callback,
                                      gpointer             user_data)
{
    GpsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_GPS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_gps_set_busy_callback (GtkWidget          *page,
                                    PnMeshPageBusyFunc  callback,
                                    gpointer            user_data)
{
    GpsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_GPS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
