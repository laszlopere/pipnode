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
/*  Bluetooth page — Phase 13.                                         */
/*                                                                     */
/*  Enabled master switch, pairing-mode combo and the fixed-PIN        */
/*  spinner.  The PIN spinner only matters when the mode is FIXED_PIN, */
/*  so it follows the combo.  Single Apply button at the bottom ships  */
/*  the whole BluetoothConfig at once.                                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-bluetooth.h"
#include "pn-action-button.h"

#include "pn-device-combo.h"
#include "pn-device-spin.h"

#define PN_MESH_BLUETOOTH_CTX_QDATA "pn-mesh-page-bluetooth-ctx"

/* Meshtastic BluetoothConfig.PairingMode. */
#define BT_MODE_FIXED_PIN 1

typedef struct
{
    GtkSwitch       *enabled_switch;
    GtkComboBoxText *mode_combo;
    GtkSpinButton   *fixed_pin_spin;
    GtkButton       *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    gboolean         writing;

    PnMeshBluetoothStatusFunc status_cb;
    gpointer                  status_ud;

    PnMeshPageBusyFunc        busy_cb;
    gpointer                  busy_ud;
} BluetoothCtx;

static void
bluetooth_ctx_free (gpointer data)
{
    g_slice_free (BluetoothCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Enum table                                                          */
/* ------------------------------------------------------------------ */

typedef struct { guint32 id; const char *name; } EnumEntry;

/* Meshtastic PairingMode.  RANDOM_PIN shows a fresh PIN on the device
 * screen each time; FIXED_PIN always uses the configured PIN; NO_PIN
 * pairs without confirmation (least secure). */
static const EnumEntry PAIRING_MODES[] = {
    { 0, "RANDOM_PIN" },
    { 1, "FIXED_PIN"  },
    { 2, "NO_PIN"     },
};

static void
fill_combo (GtkComboBoxText *combo,
            const EnumEntry *table, gsize n)
{
    gsize i;
    for (i = 0; i < n; i++)
    {
        gchar id[12];
        g_snprintf (id, sizeof id, "%u", table[i].id);
        gtk_combo_box_text_append (combo, id, table[i].name);
    }
}

static void
select_combo_by_id (GtkComboBoxText *combo,
                    const EnumEntry *table, gsize n,
                    guint32 value)
{
    gchar id[12];
    gsize i;

    for (i = 0; i < n; i++)
        if (table[i].id == value)
        {
            g_snprintf (id, sizeof id, "%u", value);
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
            return;
        }

    g_snprintf (id, sizeof id, "%u", value);
    {
        gchar *label = g_strdup_printf ("(unknown #%u)", value);
        gtk_combo_box_text_append (combo, id, label);
        g_free (label);
    }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
}

static guint32
get_combo_id (GtkComboBoxText *combo, guint32 fallback)
{
    const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
    if (id == NULL)
        return fallback;
    return (guint32) g_ascii_strtoull (id, NULL, 10);
}

/* ------------------------------------------------------------------ */
/*  Status + sensitivity                                                */
/* ------------------------------------------------------------------ */

static void
emit_status (BluetoothCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* The PIN spinner is only meaningful when Bluetooth is enabled and the
 * pairing mode is FIXED_PIN, so it follows both; the switch and combo
 * follow the base connected/idle state. */
static void
sync_sensitivity (BluetoothCtx *ctx)
{
    gboolean base = !ctx->writing && ctx->connection != NULL;
    gboolean pin  = base
            && gtk_switch_get_active (ctx->enabled_switch)
            && get_combo_id (ctx->mode_combo, 0) == BT_MODE_FIXED_PIN;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->enabled_switch), base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->mode_combo),     base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->fixed_pin_spin), pin);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),   base);
}

static void
set_writing (BluetoothCtx *ctx, gboolean writing)
{
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    sync_sensitivity (ctx);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* Toggling the switch or changing the mode re-evaluates the PIN
 * spinner's sensitivity without waiting for an Apply. */
static void
on_settings_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    GtkWidget    *page = user_data;
    BluetoothCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_BLUETOOTH_CTX_QDATA);
    (void) obj;
    (void) pspec;
    if (ctx != NULL)
        sync_sensitivity (ctx);
}

static void
on_mode_changed (GtkComboBox *combo, gpointer user_data)
{
    GtkWidget    *page = user_data;
    BluetoothCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_BLUETOOTH_CTX_QDATA);
    (void) combo;
    if (ctx != NULL)
        sync_sensitivity (ctx);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_bluetooth_config_done (GObject *source, GAsyncResult *res,
                              gpointer user_data)
{
    GtkWidget    *page = user_data;
    BluetoothCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_BLUETOOTH_CTX_QDATA);
    GError       *error = NULL;
    gboolean      ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_bluetooth_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_bluetooth_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply Bluetooth settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_bluetooth_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Bluetooth settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                   *page = user_data;
    BluetoothCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                                PN_MESH_BLUETOOTH_CTX_QDATA);
    PnMeshBluetoothConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.enabled   = gtk_switch_get_active (ctx->enabled_switch);
    cfg.mode      = get_combo_id (ctx->mode_combo, 0);
    cfg.fixed_pin =
            (guint32) gtk_spin_button_get_value_as_int (ctx->fixed_pin_spin);

    emit_status (ctx, "Applying Bluetooth settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_bluetooth_config_async (
            ctx->connection, &cfg, NULL,
            on_set_bluetooth_config_done, page);
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

GtkWidget *
pn_mesh_page_bluetooth_new (void)
{
    GtkWidget    *page;
    GtkWidget    *grid;
    GtkWidget    *cell;
    GtkWidget    *enabled;
    GtkWidget    *mode;
    GtkWidget    *fixed_pin;
    GtkWidget    *apply_box;
    GtkWidget    *apply;
    BluetoothCtx *ctx;
    gint          row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    ctx = g_slice_new0 (BluetoothCtx);

    cell = add_row (GTK_GRID (grid), row++, "Bluetooth");
    enabled = gtk_switch_new ();
    gtk_widget_set_halign (enabled, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (enabled,
            "Enable the device's Bluetooth radio so phone apps can "
            "connect to it.  Disabling it saves power and removes a "
            "wireless attack surface.");
    gtk_box_pack_start (GTK_BOX (cell), enabled, FALSE, FALSE, 0);
    ctx->enabled_switch = GTK_SWITCH (enabled);

    cell = add_row (GTK_GRID (grid), row++, "Pairing mode");
    mode = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (mode,
            "RANDOM_PIN shows a fresh PIN on the device screen on each "
            "pairing; FIXED_PIN always uses the PIN below; NO_PIN pairs "
            "without confirmation (least secure, and needs no screen).");
    fill_combo (GTK_COMBO_BOX_TEXT (mode),
                PAIRING_MODES, G_N_ELEMENTS (PAIRING_MODES));
    gtk_box_pack_start (GTK_BOX (cell), mode, FALSE, FALSE, 0);
    ctx->mode_combo = GTK_COMBO_BOX_TEXT (mode);

    cell = add_row (GTK_GRID (grid), row++, "Fixed PIN");
    /* Meshtastic uses a 6-digit pairing PIN (000000–999999). */
    fixed_pin = pn_device_spin_new_with_range (0, 999999, 1);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (fixed_pin), TRUE);
    gtk_widget_set_tooltip_text (fixed_pin,
            "The 6-digit PIN used when the pairing mode is FIXED_PIN.  "
            "Ignored in the other modes.");
    gtk_box_pack_start (GTK_BOX (cell), fixed_pin, FALSE, FALSE, 0);
    ctx->fixed_pin_spin = GTK_SPIN_BUTTON (fixed_pin);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply Bluetooth settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_BLUETOOTH_CTX_QDATA,
                            ctx, bluetooth_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);
    g_signal_connect (enabled, "notify::active",
                      G_CALLBACK (on_settings_changed), page);
    g_signal_connect (mode, "changed",
                      G_CALLBACK (on_mode_changed), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_bluetooth_set_state (GtkWidget         *page,
                                  const PnMeshState *state,
                                  PnMeshConnection  *connection)
{
    BluetoothCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_BLUETOOTH_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_bluetooth_config)
    {
        set_writing (ctx, FALSE);
        return;
    }

    gtk_switch_set_active (ctx->enabled_switch, state->bt_enabled);
    select_combo_by_id (ctx->mode_combo, PAIRING_MODES,
                        G_N_ELEMENTS (PAIRING_MODES), state->bt_mode);
    gtk_spin_button_set_value (ctx->fixed_pin_spin,
                               (gdouble) state->bt_fixed_pin);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_bluetooth_set_status_callback (GtkWidget                 *page,
                                            PnMeshBluetoothStatusFunc  callback,
                                            gpointer                   user_data)
{
    BluetoothCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_BLUETOOTH_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_bluetooth_set_busy_callback (GtkWidget          *page,
                                          PnMeshPageBusyFunc  callback,
                                          gpointer            user_data)
{
    BluetoothCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_BLUETOOTH_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
