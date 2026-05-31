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
/*  MQTT page — Phase 10.                                              */
/*                                                                     */
/*  Same shape as the External Notification page: Apply ships the     */
/*  whole sub-block, the verify-cycle re-handshake refreshes the      */
/*  state.  Detail fields follow the Enabled master switch.           */
/*                                                                     */
/*  The handshake captures MqttConfig.map_report_settings (field 11)  */
/*  as opaque bytes; Apply ships them back verbatim so the un-        */
/*  exposed sub-fields aren't lost to proto3 default-zero.            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-mqtt.h"
#include "pn-action-button.h"

#define PN_MESH_MQTT_CTX_QDATA "pn-mesh-page-mqtt-ctx"

typedef struct
{
    GtkSwitch     *enabled_switch;
    GtkEntry      *address_entry;
    GtkEntry      *username_entry;
    GtkEntry      *password_entry;
    GtkSwitch     *encryption_switch;
    GtkSwitch     *tls_switch;
    GtkEntry      *root_entry;
    GtkSwitch     *proxy_to_client_switch;
    GtkSwitch     *map_reporting_switch;
    GtkButton     *apply_button;

    GtkLabel      *unavailable_label;
    GtkWidget     *form_grid;
    GtkWidget     *apply_box;

    /* MapReportSettings shipped back verbatim on Apply.  Owned: a
     * fresh ref taken on every set_state call so the writer's
     * thread-side dup is well-defined. */
    GBytes        *map_report_settings;

    PnMeshConnection *connection;

    gboolean       writing;
    gboolean       have_data;

    PnMeshMqttStatusFunc status_cb;
    gpointer             status_ud;

    PnMeshPageBusyFunc   busy_cb;
    gpointer             busy_ud;
} MqttCtx;

static void
mqtt_ctx_free (gpointer data)
{
    MqttCtx *ctx = data;
    if (ctx->map_report_settings != NULL)
        g_bytes_unref (ctx->map_report_settings);
    g_slice_free (MqttCtx, ctx);
}

static void
emit_status (MqttCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* ------------------------------------------------------------------ */
/*  Sensitivity                                                         */
/* ------------------------------------------------------------------ */

static void
set_writing (MqttCtx *ctx, gboolean writing)
{
    gboolean form_enable =
            !writing && ctx->connection != NULL && ctx->have_data;
    gboolean detail_enable = form_enable
            && gtk_switch_get_active (ctx->enabled_switch);
    gboolean transition = (ctx->writing != writing);

    ctx->writing = writing;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->enabled_switch),         form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),           form_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->address_entry),          detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->username_entry),         detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->password_entry),         detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->encryption_switch),      detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->tls_switch),             detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->root_entry),             detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->proxy_to_client_switch), detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->map_reporting_switch),   detail_enable);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

static void
on_enabled_switch_toggled (GObject *src, GParamSpec *pspec, gpointer user_data)
{
    MqttCtx *ctx = user_data;
    (void) src; (void) pspec;
    set_writing (ctx, ctx->writing);
}

/* ------------------------------------------------------------------ */
/*  Paint                                                               */
/* ------------------------------------------------------------------ */

static void
repaint (GtkWidget *page, const PnMeshState *state)
{
    MqttCtx *ctx;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    /* Drop any previous opaque sub-message; ref the new one if
     * present.  NULL after a "no device" set_state is fine -- the
     * writer treats NULL as zero-length. */
    g_clear_pointer (&ctx->map_report_settings, g_bytes_unref);

    if (state == NULL || !state->have_mqtt)
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

    gtk_switch_set_active (ctx->enabled_switch,    state->mqtt_enabled);
    gtk_entry_set_text    (ctx->address_entry,
            state->mqtt_address  != NULL ? state->mqtt_address  : "");
    gtk_entry_set_text    (ctx->username_entry,
            state->mqtt_username != NULL ? state->mqtt_username : "");
    gtk_entry_set_text    (ctx->password_entry,
            state->mqtt_password != NULL ? state->mqtt_password : "");
    gtk_switch_set_active (ctx->encryption_switch, state->mqtt_encryption_enabled);
    gtk_switch_set_active (ctx->tls_switch,        state->mqtt_tls_enabled);
    gtk_entry_set_text    (ctx->root_entry,
            state->mqtt_root     != NULL ? state->mqtt_root     : "");
    gtk_switch_set_active (ctx->proxy_to_client_switch,
                           state->mqtt_proxy_to_client_enabled);
    gtk_switch_set_active (ctx->map_reporting_switch,
                           state->mqtt_map_reporting_enabled);

    if (state->mqtt_map_report_settings != NULL)
        ctx->map_report_settings = g_bytes_ref (state->mqtt_map_report_settings);

    set_writing (ctx, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_apply_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *page = user_data;
    MqttCtx   *ctx;
    GError    *error = NULL;
    gboolean   ok;

    (void) source;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    if (ctx == NULL)
    {
        pn_mesh_connection_set_mqtt_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_mqtt_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply MQTT settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    repaint (page, pn_mesh_connection_get_state (ctx->connection));
    emit_status (ctx, "MQTT settings applied.");
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget             *page = user_data;
    MqttCtx               *ctx;
    PnMeshMqttConfigWrite  cfg;

    (void) button;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.enabled                 = gtk_switch_get_active (ctx->enabled_switch);
    cfg.address                 = gtk_entry_get_text (ctx->address_entry);
    cfg.username                = gtk_entry_get_text (ctx->username_entry);
    cfg.password                = gtk_entry_get_text (ctx->password_entry);
    cfg.encryption_enabled      = gtk_switch_get_active (ctx->encryption_switch);
    cfg.tls_enabled             = gtk_switch_get_active (ctx->tls_switch);
    cfg.root                    = gtk_entry_get_text (ctx->root_entry);
    cfg.proxy_to_client_enabled = gtk_switch_get_active (ctx->proxy_to_client_switch);
    cfg.map_reporting_enabled   = gtk_switch_get_active (ctx->map_reporting_switch);
    cfg.map_report_settings     = ctx->map_report_settings;

    emit_status (ctx, "Applying MQTT settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_mqtt_config_async (
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

static GtkWidget *
make_switch (void)
{
    GtkWidget *sw = gtk_switch_new ();
    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    return sw;
}

/* Toggle a password entry between hidden and visible when its reveal
 * icon is clicked -- the same gesture the credentials dialog uses for
 * secret fields. */
static void
on_reveal_icon_press (GtkEntry             *entry,
                      GtkEntryIconPosition  pos,
                      GdkEvent             *event,
                      gpointer              user_data)
{
    (void) event; (void) user_data;
    if (pos == GTK_ENTRY_ICON_SECONDARY)
        gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

/* Add a clickable eye icon to a (hidden) password entry so the user can
 * peek at what they typed. */
static void
add_password_reveal_icon (GtkEntry *entry)
{
    gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY,
                                       "view-reveal-symbolic");
    gtk_entry_set_icon_activatable (entry, GTK_ENTRY_ICON_SECONDARY, TRUE);
    gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY,
                                     "Show or hide the password");
    g_signal_connect (entry, "icon-press",
                      G_CALLBACK (on_reveal_icon_press), NULL);
}

GtkWidget *
pn_mesh_page_mqtt_new (void)
{
    MqttCtx   *ctx;
    GtkWidget *page;
    GtkWidget *subtitle;
    GtkWidget *unavailable;
    GtkWidget *grid;
    GtkWidget *cell;
    GtkWidget *w;
    GtkWidget *apply_box;
    GtkWidget *apply;
    gint       row = 0;

    ctx = g_slice_new0 (MqttCtx);

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Bridges the device's mesh traffic to an MQTT broker so "
            "internet-connected nodes can forward messages off-radio.  "
            "Leave Address blank to use the upstream default broker.");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    unavailable = gtk_label_new (
            "The device has not reported its MQTT configuration. Try "
            "reconnecting; if the message persists this firmware does "
            "not expose the module.");
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
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 6);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);
    ctx->form_grid = grid;

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Master switch for the MQTT module. Turn off to stop the "
            "device bridging mesh traffic to the broker.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_enabled_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Address",
            "MQTT broker host[:port]. Leave blank to use the "
            "Meshtastic default broker.");
    w = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (w), "(upstream default)");
    gtk_widget_set_hexpand (w, TRUE);
    gtk_box_pack_start (GTK_BOX (cell), w, TRUE, TRUE, 0);
    ctx->address_entry = GTK_ENTRY (w);

    cell = add_row (GTK_GRID (grid), row++, "Username",
            "Broker username. Leave blank for anonymous brokers.");
    w = gtk_entry_new ();
    gtk_widget_set_hexpand (w, TRUE);
    gtk_box_pack_start (GTK_BOX (cell), w, TRUE, TRUE, 0);
    ctx->username_entry = GTK_ENTRY (w);

    cell = add_row (GTK_GRID (grid), row++, "Password",
            "Broker password. Stored on the device in plain text -- "
            "the firmware ships it back to us on every handshake.");
    w = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (w), FALSE);
    gtk_widget_set_hexpand (w, TRUE);
    add_password_reveal_icon (GTK_ENTRY (w));
    gtk_box_pack_start (GTK_BOX (cell), w, TRUE, TRUE, 0);
    ctx->password_entry = GTK_ENTRY (w);

    cell = add_row (GTK_GRID (grid), row++, "Send encrypted",
            "When on, mesh packets are forwarded to MQTT in their "
            "encrypted form. When off, the device decrypts before "
            "publishing -- needed for interop with non-Meshtastic "
            "MQTT subscribers but reveals message contents to the "
            "broker.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->encryption_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "TLS",
            "Connect to the broker over TLS (port 8883 typical). "
            "Disable for plain MQTT (port 1883 typical).");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->tls_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Root topic",
            "Root prefix for every MQTT topic the device publishes. "
            "Default is \"msh\". Useful if you share a broker with "
            "other Meshtastic networks.");
    w = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (w), "msh");
    gtk_widget_set_hexpand (w, TRUE);
    gtk_box_pack_start (GTK_BOX (cell), w, TRUE, TRUE, 0);
    ctx->root_entry = GTK_ENTRY (w);

    cell = add_row (GTK_GRID (grid), row++, "Proxy client → MQTT",
            "Allow a connected phone/client to proxy its MQTT "
            "traffic through this device.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->proxy_to_client_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Map reporting",
            "Periodically publish node-info to the public Meshtastic "
            "map.  The fine-grained settings (interval, precision) "
            "stay at whatever the device already has.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->map_reporting_switch = GTK_SWITCH (w);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 12);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply MQTT settings",
                                  PN_ACTION_BUTTON_SUGGESTED);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);
    ctx->apply_box = apply_box;

    g_object_set_data_full (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA,
                            ctx, mqtt_ctx_free);

    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_mqtt_set_state (GtkWidget         *page,
                             const PnMeshState *state,
                             PnMeshConnection  *connection)
{
    MqttCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    repaint (page, state);
}

void
pn_mesh_page_mqtt_set_status_callback (GtkWidget            *page,
                                       PnMeshMqttStatusFunc  callback,
                                       gpointer              user_data)
{
    MqttCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_mqtt_set_busy_callback (GtkWidget          *page,
                                     PnMeshPageBusyFunc  callback,
                                     gpointer            user_data)
{
    MqttCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_MQTT_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
