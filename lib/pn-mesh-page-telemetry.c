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
/*  Telemetry page — Phase 11.                                         */
/*                                                                     */
/*  Five sub-systems, each grouped under a small bolded section       */
/*  header that spans both grid columns: device / environment / air   */
/*  quality / power / health.  Each section's detail fields follow    */
/*  its own enable switch -- flipping one greys out the corresponding */
/*  intervals and screen toggles in real time.                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-telemetry.h"

#include "pn-device-spin.h"

#define PN_MESH_TELEMETRY_CTX_QDATA "pn-mesh-page-telemetry-ctx"

typedef struct
{
    /* Device sub-system. */
    GtkSwitch     *dev_enabled_switch;
    GtkSpinButton *dev_interval_spin;

    /* Environment sub-system. */
    GtkSwitch     *env_enabled_switch;
    GtkSpinButton *env_interval_spin;
    GtkSwitch     *env_screen_switch;
    GtkSwitch     *env_fahrenheit_switch;

    /* Air quality sub-system. */
    GtkSwitch     *air_enabled_switch;
    GtkSpinButton *air_interval_spin;
    GtkSwitch     *air_screen_switch;

    /* Power sub-system. */
    GtkSwitch     *pow_enabled_switch;
    GtkSpinButton *pow_interval_spin;
    GtkSwitch     *pow_screen_switch;

    /* Health sub-system. */
    GtkSwitch     *hlth_enabled_switch;
    GtkSpinButton *hlth_interval_spin;
    GtkSwitch     *hlth_screen_switch;

    GtkButton     *apply_button;

    GtkLabel      *unavailable_label;
    GtkWidget     *form_grid;
    GtkWidget     *apply_box;

    PnMeshConnection *connection;

    gboolean       writing;
    gboolean       have_data;

    PnMeshTelemetryStatusFunc status_cb;
    gpointer                  status_ud;

    PnMeshPageBusyFunc        busy_cb;
    gpointer                  busy_ud;
} TelemetryCtx;

static void
telemetry_ctx_free (gpointer data)
{
    g_slice_free (TelemetryCtx, data);
}

static void
emit_status (TelemetryCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* ------------------------------------------------------------------ */
/*  Sensitivity                                                         */
/* ------------------------------------------------------------------ */

/* Per-sub-system gating: each enable switch greys out its own
 * detail fields.  The Apply button + enable switches themselves
 * follow the "form editable at all" gate (connection + have_data
 * + not writing). */
static void
set_writing (TelemetryCtx *ctx, gboolean writing)
{
    gboolean form_enable =
            !writing && ctx->connection != NULL && ctx->have_data;
    gboolean dev_detail  = form_enable
            && gtk_switch_get_active (ctx->dev_enabled_switch);
    gboolean env_detail  = form_enable
            && gtk_switch_get_active (ctx->env_enabled_switch);
    gboolean air_detail  = form_enable
            && gtk_switch_get_active (ctx->air_enabled_switch);
    gboolean pow_detail  = form_enable
            && gtk_switch_get_active (ctx->pow_enabled_switch);
    gboolean hlth_detail = form_enable
            && gtk_switch_get_active (ctx->hlth_enabled_switch);
    gboolean transition = (ctx->writing != writing);

    ctx->writing = writing;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),         form_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->dev_enabled_switch),   form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->dev_interval_spin),    dev_detail);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->env_enabled_switch),   form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->env_interval_spin),    env_detail);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->env_screen_switch),    env_detail);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->env_fahrenheit_switch),env_detail);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->air_enabled_switch),   form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->air_interval_spin),    air_detail);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->air_screen_switch),    air_detail);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->pow_enabled_switch),   form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->pow_interval_spin),    pow_detail);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->pow_screen_switch),    pow_detail);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->hlth_enabled_switch),  form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->hlth_interval_spin),   hlth_detail);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->hlth_screen_switch),   hlth_detail);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* One callback shared by all five enable switches.  We don't need
 * to know which one changed -- set_writing recomputes all five
 * gates from scratch on every call. */
static void
on_subsystem_switch_toggled (GObject *src, GParamSpec *pspec, gpointer user_data)
{
    TelemetryCtx *ctx = user_data;
    (void) src; (void) pspec;
    set_writing (ctx, ctx->writing);
}

/* ------------------------------------------------------------------ */
/*  Paint                                                               */
/* ------------------------------------------------------------------ */

static void
repaint (GtkWidget *page, const PnMeshState *state)
{
    TelemetryCtx *ctx;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    if (state == NULL || !state->have_telemetry)
    {
        ctx->have_data = FALSE;
        gtk_widget_show (GTK_WIDGET (ctx->unavailable_label));
        gtk_widget_hide (ctx->form_grid);
        gtk_widget_hide (ctx->apply_box);
        set_writing (ctx, FALSE);
        return;
    }

    ctx->have_data = TRUE;
    gtk_widget_hide (GTK_WIDGET (ctx->unavailable_label));
    gtk_widget_show (ctx->form_grid);
    gtk_widget_show (ctx->apply_box);

    gtk_switch_set_active (ctx->dev_enabled_switch,
                           state->tel_device_telemetry_enabled);
    gtk_spin_button_set_value (ctx->dev_interval_spin,
                               state->tel_device_update_interval);

    gtk_switch_set_active (ctx->env_enabled_switch,
                           state->tel_environment_measurement_enabled);
    gtk_spin_button_set_value (ctx->env_interval_spin,
                               state->tel_environment_update_interval);
    gtk_switch_set_active (ctx->env_screen_switch,
                           state->tel_environment_screen_enabled);
    gtk_switch_set_active (ctx->env_fahrenheit_switch,
                           state->tel_environment_display_fahrenheit);

    gtk_switch_set_active (ctx->air_enabled_switch,
                           state->tel_air_quality_enabled);
    gtk_spin_button_set_value (ctx->air_interval_spin,
                               state->tel_air_quality_interval);
    gtk_switch_set_active (ctx->air_screen_switch,
                           state->tel_air_quality_screen_enabled);

    gtk_switch_set_active (ctx->pow_enabled_switch,
                           state->tel_power_measurement_enabled);
    gtk_spin_button_set_value (ctx->pow_interval_spin,
                               state->tel_power_update_interval);
    gtk_switch_set_active (ctx->pow_screen_switch,
                           state->tel_power_screen_enabled);

    gtk_switch_set_active (ctx->hlth_enabled_switch,
                           state->tel_health_measurement_enabled);
    gtk_spin_button_set_value (ctx->hlth_interval_spin,
                               state->tel_health_update_interval);
    gtk_switch_set_active (ctx->hlth_screen_switch,
                           state->tel_health_screen_enabled);

    set_writing (ctx, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_apply_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget    *page = user_data;
    TelemetryCtx *ctx;
    GError       *error = NULL;
    gboolean      ok;

    (void) source;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    if (ctx == NULL)
    {
        pn_mesh_connection_set_telemetry_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_telemetry_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply Telemetry settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    repaint (page, pn_mesh_connection_get_state (ctx->connection));
    emit_status (ctx, "Telemetry settings applied.");
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                  *page = user_data;
    TelemetryCtx               *ctx;
    PnMeshTelemetryConfigWrite  cfg;

    (void) button;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.device_telemetry_enabled       = gtk_switch_get_active (ctx->dev_enabled_switch);
    cfg.device_update_interval         = (guint32) gtk_spin_button_get_value_as_int (ctx->dev_interval_spin);
    cfg.environment_measurement_enabled = gtk_switch_get_active (ctx->env_enabled_switch);
    cfg.environment_update_interval    = (guint32) gtk_spin_button_get_value_as_int (ctx->env_interval_spin);
    cfg.environment_screen_enabled     = gtk_switch_get_active (ctx->env_screen_switch);
    cfg.environment_display_fahrenheit = gtk_switch_get_active (ctx->env_fahrenheit_switch);
    cfg.air_quality_enabled            = gtk_switch_get_active (ctx->air_enabled_switch);
    cfg.air_quality_interval           = (guint32) gtk_spin_button_get_value_as_int (ctx->air_interval_spin);
    cfg.air_quality_screen_enabled     = gtk_switch_get_active (ctx->air_screen_switch);
    cfg.power_measurement_enabled      = gtk_switch_get_active (ctx->pow_enabled_switch);
    cfg.power_update_interval          = (guint32) gtk_spin_button_get_value_as_int (ctx->pow_interval_spin);
    cfg.power_screen_enabled           = gtk_switch_get_active (ctx->pow_screen_switch);
    cfg.health_measurement_enabled     = gtk_switch_get_active (ctx->hlth_enabled_switch);
    cfg.health_update_interval         = (guint32) gtk_spin_button_get_value_as_int (ctx->hlth_interval_spin);
    cfg.health_screen_enabled          = gtk_switch_get_active (ctx->hlth_screen_switch);

    emit_status (ctx, "Applying Telemetry settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_telemetry_config_async (
            ctx->connection, &cfg, NULL, on_apply_done, page);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

#define VALUE_MIN_WIDTH 260

static void
on_cell_realize (GtkWidget *cell, gpointer user_data)
{
    GtkSizeGroup *sg = user_data;
    GList        *children = gtk_container_get_children (GTK_CONTAINER (cell));

    /* Combos, spin buttons and entries share a common width via the
     * size group so the value column lines up.  A switch is naturally
     * narrow and already left-aligned (halign START), so leave it out
     * -- otherwise the size group stretches it to the widest control. */
    if (children != NULL && !GTK_IS_SWITCH (children->data)) {
        GtkWidget *first = children->data;
        gtk_widget_set_size_request (first, VALUE_MIN_WIDTH, -1);
        gtk_size_group_add_widget (sg, first);
    }
    g_list_free (children);
    g_signal_handlers_disconnect_by_func (cell, on_cell_realize, sg);
}

static GtkSizeGroup *
get_value_size_group (GtkGrid *grid)
{
    GtkSizeGroup *sg = g_object_get_data (G_OBJECT (grid), "value-sg");
    if (sg == NULL) {
        sg = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
        g_object_set_data_full (G_OBJECT (grid), "value-sg",
                                sg, g_object_unref);
    }
    return sg;
}

/* Bold key | control row.  Identical in shape to the helpers in
 * the other module pages -- inlined here to avoid pulling a
 * libpipnode-gui-internal header just for one function. */
static GtkWidget *
add_row (GtkGrid *grid, gint row, const gchar *label_text,
         const gchar *tooltip)
{
    GtkWidget     *key   = gtk_label_new (label_text);
    PangoAttrList *attrs = pango_attr_list_new ();
    GtkWidget     *cell;

    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    gtk_widget_set_margin_end (key, 16);
    if (tooltip != NULL)
        gtk_widget_set_tooltip_text (key, tooltip);
    gtk_grid_attach (grid, key, 0, row, 1, 1);

    cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (cell, TRUE);
    gtk_grid_attach (grid, cell, 1, row, 1, 1);
    g_signal_connect (cell, "realize",
                      G_CALLBACK (on_cell_realize),
                      get_value_size_group (grid));
    return cell;
}

/* Section header spanning both grid columns.  Visually breaks the
 * form into the five logical sub-systems without making each one
 * its own expander (over-nesting). */
static void
add_section (GtkGrid *grid, gint row, const gchar *title)
{
    GtkWidget *label = gtk_label_new (NULL);
    gchar     *markup;
    GtkStyleContext *sc;

    markup = g_markup_printf_escaped ("<b>%s</b>", title);
    gtk_label_set_markup (GTK_LABEL (label), markup);
    g_free (markup);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    /* Extra top padding except on the very first section. */
    gtk_widget_set_margin_top (label, row == 0 ? 0 : 10);
    sc = gtk_widget_get_style_context (label);
    gtk_style_context_add_class (sc, "dim-label");

    gtk_grid_attach (grid, label, 0, row, 2, 1);
}

static GtkWidget *
make_switch (void)
{
    GtkWidget *sw = gtk_switch_new ();
    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    return sw;
}

/* Spin button for an interval-in-seconds.  Range 0..86400 (one day)
 * covers everything plausible; the firmware caps at its own limit
 * if the user picks something absurd. */
static GtkSpinButton *
make_interval_spin (GtkWidget *cell)
{
    GtkWidget       *w   = pn_device_spin_new_with_range (0, 86400, 1);
    GtkWidget       *suf = gtk_label_new (" s");
    GtkStyleContext *sc  = gtk_widget_get_style_context (suf);
    gtk_style_context_add_class (sc, "dim-label");
    gtk_box_pack_start (GTK_BOX (cell), w,   FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (cell), suf, FALSE, FALSE, 0);
    return GTK_SPIN_BUTTON (w);
}

GtkWidget *
pn_mesh_page_telemetry_new (void)
{
    TelemetryCtx *ctx;
    GtkWidget    *page;
    GtkWidget    *subtitle;
    GtkWidget    *unavailable;
    GtkWidget    *grid;
    GtkWidget    *cell;
    GtkWidget    *w;
    GtkWidget    *apply_box;
    GtkWidget    *apply;
    gint          row = 0;

    ctx = g_slice_new0 (TelemetryCtx);

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "How often the device broadcasts its own metrics.  Each "
            "sub-system has its own enable + interval; intervals are "
            "in seconds (0 disables periodic broadcasts for that "
            "sub-system).");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    unavailable = gtk_label_new (
            "The device has not reported its Telemetry configuration. "
            "Try reconnecting; if the message persists this firmware "
            "does not expose the module.");
    gtk_label_set_xalign    (GTK_LABEL (unavailable), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (unavailable), TRUE);
    gtk_widget_set_margin_top (unavailable, 6);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (unavailable);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), unavailable, FALSE, FALSE, 0);
    ctx->unavailable_label = GTK_LABEL (unavailable);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 6);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);
    ctx->form_grid = grid;

    /* ----- Device metrics --------------------------------------- */
    add_section (GTK_GRID (grid), row++, "Device metrics");

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Allow the device to broadcast its own metrics "
            "(battery, channel utilisation, airtime).");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->dev_enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_subsystem_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Interval",
            "Seconds between device-metrics broadcasts.");
    ctx->dev_interval_spin = make_interval_spin (cell);

    /* ----- Environment sensors ---------------------------------- */
    add_section (GTK_GRID (grid), row++, "Environment sensors");

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Collect data from attached environment sensors (e.g. "
            "BME280, AHT10).  Requires the right sensor wired in.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->env_enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_subsystem_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Interval",
            "Seconds between environment broadcasts.");
    ctx->env_interval_spin = make_interval_spin (cell);

    cell = add_row (GTK_GRID (grid), row++, "Show on screen",
            "Add a screen page showing the latest environment readings.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->env_screen_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Use °F",
            "Display temperature in Fahrenheit. Off = Celsius.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->env_fahrenheit_switch = GTK_SWITCH (w);

    /* ----- Air quality ------------------------------------------ */
    add_section (GTK_GRID (grid), row++, "Air quality");

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Collect data from an attached air-quality sensor (PMSA, "
            "SDS011, etc.).");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->air_enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_subsystem_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Interval",
            "Seconds between air-quality broadcasts.");
    ctx->air_interval_spin = make_interval_spin (cell);

    cell = add_row (GTK_GRID (grid), row++, "Show on screen",
            "Add a screen page for air-quality readings.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->air_screen_switch = GTK_SWITCH (w);

    /* ----- Power metrics ---------------------------------------- */
    add_section (GTK_GRID (grid), row++, "Power metrics");

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Collect power-supply metrics (typically from a connected "
            "INA219/INA260 sensor).");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->pow_enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_subsystem_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Interval",
            "Seconds between power broadcasts.");
    ctx->pow_interval_spin = make_interval_spin (cell);

    cell = add_row (GTK_GRID (grid), row++, "Show on screen",
            "Add a screen page for power readings.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->pow_screen_switch = GTK_SWITCH (w);

    /* ----- Health metrics --------------------------------------- */
    add_section (GTK_GRID (grid), row++, "Health metrics");

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Collect health/wearable metrics from a supported sensor.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->hlth_enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_subsystem_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Interval",
            "Seconds between health broadcasts.");
    ctx->hlth_interval_spin = make_interval_spin (cell);

    cell = add_row (GTK_GRID (grid), row++, "Show on screen",
            "Add a screen page for health readings.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->hlth_screen_switch = GTK_SWITCH (w);

    /* ----- Apply ------------------------------------------------- */
    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 12);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = gtk_button_new_with_mnemonic ("_Apply");
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);
    ctx->apply_box = apply_box;

    g_object_set_data_full (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA,
                            ctx, telemetry_ctx_free);

    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_telemetry_set_state (GtkWidget         *page,
                                  const PnMeshState *state,
                                  PnMeshConnection  *connection)
{
    TelemetryCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    repaint (page, state);
}

void
pn_mesh_page_telemetry_set_status_callback (
        GtkWidget                 *page,
        PnMeshTelemetryStatusFunc  callback,
        gpointer                   user_data)
{
    TelemetryCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_telemetry_set_busy_callback (GtkWidget          *page,
                                          PnMeshPageBusyFunc  callback,
                                          gpointer            user_data)
{
    TelemetryCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_TELEMETRY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
