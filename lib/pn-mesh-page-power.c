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
/*  Power page — Phase 12.                                             */
/*                                                                     */
/*  Power-saving master switch, four sleep/wake interval spinners,    */
/*  on-battery shutdown threshold, ADC multiplier override.  Single   */
/*  Apply button at the bottom.  device_battery_ina_address is        */
/*  round-tripped verbatim so an Apply does not clobber a board-      */
/*  specific I2C address for the battery fuel gauge.                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-power.h"
#include "pn-action-button.h"

#include "pn-device-spin.h"

#define PN_MESH_POWER_CTX_QDATA "pn-mesh-page-power-ctx"

typedef struct
{
    GtkSwitch     *power_saving_switch;
    GtkSpinButton *shutdown_spin;
    GtkSpinButton *adc_spin;
    GtkSpinButton *wait_bluetooth_spin;
    GtkSpinButton *sds_spin;
    GtkSpinButton *ls_spin;
    GtkSpinButton *min_wake_spin;
    GtkButton     *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Mirrored at set_state-time so Apply ships it back unchanged. */
    guint32          last_ina_address;

    gboolean         writing;

    PnMeshPowerStatusFunc status_cb;
    gpointer              status_ud;

    PnMeshPageBusyFunc    busy_cb;
    gpointer              busy_ud;
} PowerCtx;

static void
power_ctx_free (gpointer data)
{
    g_slice_free (PowerCtx, data);
}

static void
emit_status (PowerCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* Recompute every control's sensitivity from the current connection,
 * write-in-flight and power-saving state.  The four sleep/wake timing
 * spinners are part of the firmware's power-saving state machine and
 * are ignored when power saving is off, so they follow the switch; the
 * on-battery shutdown timer and the ADC calibration multiplier are used
 * regardless, so they only follow the base connected/idle state. */
static void
sync_sensitivity (PowerCtx *ctx)
{
    gboolean base   = !ctx->writing && ctx->connection != NULL;
    gboolean sleeps = base
            && gtk_switch_get_active (ctx->power_saving_switch);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->power_saving_switch),  base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->shutdown_spin),        base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->adc_spin),             base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->wait_bluetooth_spin),  sleeps);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->sds_spin),             sleeps);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->ls_spin),              sleeps);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->min_wake_spin),        sleeps);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),         base);
}

static void
set_writing (PowerCtx *ctx, gboolean writing)
{
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    sync_sensitivity (ctx);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* Toggling the master switch flips the sleep-timing spinners between
 * enabled and greyed-out without waiting for an Apply. */
static void
on_power_saving_toggled (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    GtkWidget *page = user_data;
    PowerCtx  *ctx  = g_object_get_data (G_OBJECT (page),
                                         PN_MESH_POWER_CTX_QDATA);
    (void) obj;
    (void) pspec;
    if (ctx != NULL)
        sync_sensitivity (ctx);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_power_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *page = user_data;
    PowerCtx  *ctx  = g_object_get_data (G_OBJECT (page),
                                         PN_MESH_POWER_CTX_QDATA);
    GError    *error = NULL;
    gboolean   ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_power_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_power_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply power settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_power_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Power settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget               *page = user_data;
    PowerCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                                       PN_MESH_POWER_CTX_QDATA);
    PnMeshPowerConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.is_power_saving =
            gtk_switch_get_active (ctx->power_saving_switch);
    cfg.on_battery_shutdown_after_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->shutdown_spin);
    cfg.adc_multiplier_override =
            (gfloat) gtk_spin_button_get_value (ctx->adc_spin);
    cfg.wait_bluetooth_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->wait_bluetooth_spin);
    cfg.sds_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->sds_spin);
    cfg.ls_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->ls_spin);
    cfg.min_wake_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->min_wake_spin);
    cfg.device_battery_ina_address = ctx->last_ina_address;

    emit_status (ctx, "Applying power settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_power_config_async (
            ctx->connection, &cfg, NULL,
            on_set_power_config_done, page);
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

static GtkWidget *
add_row (GtkGrid *grid, gint row, const gchar *label_text)
{
    GtkWidget *key = gtk_label_new (label_text);
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    gtk_widget_set_margin_end (key, 16);
    gtk_grid_attach (grid, key, 0, row, 1, 1);

    {
        GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand (cell, TRUE);
        gtk_grid_attach (grid, cell, 1, row, 1, 1);
        g_signal_connect (cell, "realize",
                          G_CALLBACK (on_cell_realize),
                          get_value_size_group (grid));
        return cell;
    }
}

static void
attach_unit (GtkWidget *cell, const gchar *unit)
{
    GtkWidget       *suffix = gtk_label_new (unit);
    GtkStyleContext *sc     = gtk_widget_get_style_context (suffix);
    gtk_style_context_add_class (sc, "dim-label");
    gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
}

GtkWidget *
pn_mesh_page_power_new (void)
{
    GtkWidget *page;
    GtkWidget *subtitle;
    GtkWidget *grid;
    GtkWidget *cell;
    GtkWidget *power_saving;
    GtkWidget *shutdown;
    GtkWidget *adc;
    GtkWidget *wait_bluetooth;
    GtkWidget *sds;
    GtkWidget *ls;
    GtkWidget *min_wake;
    GtkWidget *apply_box;
    GtkWidget *apply;
    PowerCtx  *ctx;
    gint       row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Battery-saving behaviour, sleep timings, on-battery "
            "shutdown threshold and ADC calibration.  The sleep/wake "
            "timings apply only while power saving is on.  Apply writes "
            "the whole PowerConfig block at once.  Leave intervals "
            "at 0 to use the firmware's hardware-specific defaults.");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    ctx = g_slice_new0 (PowerCtx);

    cell = add_row (GTK_GRID (grid), row++, "Power saving");
    power_saving = gtk_switch_new ();
    gtk_widget_set_halign (power_saving, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (power_saving,
            "Enable the firmware's power-saving state machine.  "
            "Combined with the sleep timings below the device "
            "alternates between light-sleep and deep-sleep to "
            "extend battery life.");
    gtk_box_pack_start (GTK_BOX (cell), power_saving, FALSE, FALSE, 0);
    ctx->power_saving_switch = GTK_SWITCH (power_saving);

    cell = add_row (GTK_GRID (grid), row++, "On-battery shutdown");
    /* 0 = never; the firmware accepts any uint32. */
    shutdown = pn_device_spin_new_with_range (0, 86400 * 30, 60);
    gtk_widget_set_tooltip_text (shutdown,
            "Seconds the device stays on after USB power is removed "
            "before it shuts down.  0 means \"never auto-shutdown\".");
    gtk_box_pack_start (GTK_BOX (cell), shutdown, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->shutdown_spin = GTK_SPIN_BUTTON (shutdown);

    cell = add_row (GTK_GRID (grid), row++, "ADC multiplier");
    /* Float spin button: 0 = firmware default; the range covers the
     * realistic calibration offsets a user would punch in (1.5–4.0). */
    adc = pn_device_spin_new_with_range (0.0, 10.0, 0.01);
    gtk_spin_button_set_digits (GTK_SPIN_BUTTON (adc), 3);
    gtk_widget_set_tooltip_text (adc,
            "Multiplier applied to the battery-voltage ADC reading.  "
            "0 lets the firmware use its built-in calibration for "
            "this board.");
    gtk_box_pack_start (GTK_BOX (cell), adc, FALSE, FALSE, 0);
    attach_unit (cell, " ×");
    ctx->adc_spin = GTK_SPIN_BUTTON (adc);

    cell = add_row (GTK_GRID (grid), row++, "Wait for Bluetooth");
    wait_bluetooth = pn_device_spin_new_with_range (0, 3600, 5);
    gtk_widget_set_tooltip_text (wait_bluetooth,
            "Seconds the device stays awake at boot waiting for a "
            "Bluetooth client before entering sleep.  0 uses the "
            "firmware default.");
    gtk_box_pack_start (GTK_BOX (cell), wait_bluetooth, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->wait_bluetooth_spin = GTK_SPIN_BUTTON (wait_bluetooth);

    cell = add_row (GTK_GRID (grid), row++, "Super-deep sleep");
    sds = pn_device_spin_new_with_range (0, 86400 * 365, 60);
    gtk_widget_set_tooltip_text (sds,
            "Seconds before the device enters Super-Deep-Sleep "
            "(\"SDS\") after going idle.  0 uses the firmware "
            "default.");
    gtk_box_pack_start (GTK_BOX (cell), sds, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->sds_spin = GTK_SPIN_BUTTON (sds);

    cell = add_row (GTK_GRID (grid), row++, "Light sleep");
    ls = pn_device_spin_new_with_range (0, 86400, 30);
    gtk_widget_set_tooltip_text (ls,
            "Seconds before the device enters Light-Sleep after "
            "going idle.  0 uses the firmware default.");
    gtk_box_pack_start (GTK_BOX (cell), ls, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->ls_spin = GTK_SPIN_BUTTON (ls);

    cell = add_row (GTK_GRID (grid), row++, "Minimum awake");
    min_wake = pn_device_spin_new_with_range (0, 3600, 5);
    gtk_widget_set_tooltip_text (min_wake,
            "Minimum seconds the device stays awake between sleep "
            "cycles.  0 uses the firmware default.");
    gtk_box_pack_start (GTK_BOX (cell), min_wake, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->min_wake_spin = GTK_SPIN_BUTTON (min_wake);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply power settings",
                                  PN_ACTION_BUTTON_SUGGESTED);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_POWER_CTX_QDATA,
                            ctx, power_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);
    /* Re-evaluate the sleep-spinner sensitivity whenever the master
     * switch flips (set_state's gtk_switch_set_active also lands here). */
    g_signal_connect (power_saving, "notify::active",
                      G_CALLBACK (on_power_saving_toggled), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_power_set_state (GtkWidget         *page,
                              const PnMeshState *state,
                              PnMeshConnection  *connection)
{
    PowerCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POWER_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_power)
    {
        ctx->last_ina_address = 0;
        set_writing (ctx, FALSE);
        return;
    }

    ctx->last_ina_address = state->pow_device_battery_ina_address;

    gtk_switch_set_active (ctx->power_saving_switch,
                           state->pow_is_power_saving);
    gtk_spin_button_set_value (ctx->shutdown_spin,
                               (gdouble) state->pow_on_battery_shutdown_after_secs);
    gtk_spin_button_set_value (ctx->adc_spin,
                               (gdouble) state->pow_adc_multiplier_override);
    gtk_spin_button_set_value (ctx->wait_bluetooth_spin,
                               (gdouble) state->pow_wait_bluetooth_secs);
    gtk_spin_button_set_value (ctx->sds_spin,
                               (gdouble) state->pow_sds_secs);
    gtk_spin_button_set_value (ctx->ls_spin,
                               (gdouble) state->pow_ls_secs);
    gtk_spin_button_set_value (ctx->min_wake_spin,
                               (gdouble) state->pow_min_wake_secs);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_power_set_status_callback (GtkWidget             *page,
                                        PnMeshPowerStatusFunc  callback,
                                        gpointer               user_data)
{
    PowerCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POWER_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_power_set_busy_callback (GtkWidget          *page,
                                      PnMeshPageBusyFunc  callback,
                                      gpointer            user_data)
{
    PowerCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POWER_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
