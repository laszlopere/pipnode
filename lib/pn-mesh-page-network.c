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
/*  Network page — Phase 14.                                           */
/*                                                                     */
/*  Switches for WiFi enabled / Ethernet enabled / IPv6 enabled,       */
/*  entries for the SSID and PSK (PSK masked).  Round-trip slots in    */
/*  ctx capture ntp_server, address_mode, the opaque ipv4_config       */
/*  sub-message bytes, rsyslog_server and enabled_protocols so that    */
/*  Apply does not silently reset them.                                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-network.h"
#include "pn-action-button.h"

#define PN_MESH_NETWORK_CTX_QDATA "pn-mesh-page-network-ctx"

typedef struct
{
    GtkSwitch *wifi_switch;
    GtkEntry  *wifi_ssid_entry;
    GtkEntry  *wifi_psk_entry;
    GtkSwitch *eth_switch;
    GtkSwitch *ipv6_switch;
    GtkButton *apply_button;

    PnMeshConnection *connection;

    /* Round-trip slots. */
    gchar    *last_ntp_server;
    gchar    *last_rsyslog_server;
    guint32   last_address_mode;
    guint32   last_enabled_protocols;
    GBytes   *last_ipv4_config;        /* may be NULL */

    gboolean  writing;

    PnMeshNetworkStatusFunc status_cb;
    gpointer                status_ud;

    PnMeshPageBusyFunc      busy_cb;
    gpointer                busy_ud;
} NetworkCtx;

static void
network_ctx_free (gpointer data)
{
    NetworkCtx *ctx = data;
    g_free (ctx->last_ntp_server);
    g_free (ctx->last_rsyslog_server);
    if (ctx->last_ipv4_config != NULL)
        g_bytes_unref (ctx->last_ipv4_config);
    g_slice_free (NetworkCtx, ctx);
}

/* ------------------------------------------------------------------ */
/*  Status + writing                                                    */
/* ------------------------------------------------------------------ */

static void
emit_status (NetworkCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (NetworkCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->wifi_switch),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->wifi_ssid_entry), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->wifi_psk_entry),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->eth_switch),      enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->ipv6_switch),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),    enable);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_network_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget  *page = user_data;
    NetworkCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                          PN_MESH_NETWORK_CTX_QDATA);
    GError     *error = NULL;
    gboolean    ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_network_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_network_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply network settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_network_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Network settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                *page = user_data;
    NetworkCtx               *ctx  = g_object_get_data (G_OBJECT (page),
                                                       PN_MESH_NETWORK_CTX_QDATA);
    PnMeshNetworkConfigWrite  cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.wifi_enabled       = gtk_switch_get_active (ctx->wifi_switch);
    cfg.wifi_ssid          = gtk_entry_get_text (ctx->wifi_ssid_entry);
    cfg.wifi_psk           = gtk_entry_get_text (ctx->wifi_psk_entry);
    cfg.eth_enabled        = gtk_switch_get_active (ctx->eth_switch);
    cfg.ipv6_enabled       = gtk_switch_get_active (ctx->ipv6_switch);
    cfg.ntp_server         = ctx->last_ntp_server;
    cfg.address_mode       = ctx->last_address_mode;
    cfg.ipv4_config        = ctx->last_ipv4_config;
    cfg.rsyslog_server     = ctx->last_rsyslog_server;
    cfg.enabled_protocols  = ctx->last_enabled_protocols;

    emit_status (ctx, "Applying network settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_network_config_async (
            ctx->connection, &cfg, NULL,
            on_set_network_config_done, page);
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
pn_mesh_page_network_new (void)
{
    GtkWidget  *page;
    GtkWidget  *subtitle;
    GtkWidget  *grid;
    GtkWidget  *cell;
    GtkWidget  *wifi_switch;
    GtkWidget  *wifi_ssid;
    GtkWidget  *wifi_psk;
    GtkWidget  *eth_switch;
    GtkWidget  *ipv6_switch;
    GtkWidget  *apply_box;
    GtkWidget  *apply;
    NetworkCtx *ctx;
    gint        row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "WiFi, Ethernet and IPv6 settings.  WiFi requires firmware "
            "built with WiFi support (most ESP32 boards).  Apply writes "
            "the whole NetworkConfig block; NTP / syslog / static IP "
            "settings are kept verbatim from the device.");
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

    ctx = g_slice_new0 (NetworkCtx);

    cell = add_row (GTK_GRID (grid), row++, "WiFi");
    wifi_switch = gtk_switch_new ();
    gtk_widget_set_halign (wifi_switch, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (wifi_switch,
            "Master switch for the WiFi radio.  Needs an SSID + PSK "
            "below; firmware that wasn't compiled with WiFi support "
            "(nrf52 boards, for example) silently ignores this.");
    gtk_box_pack_start (GTK_BOX (cell), wifi_switch, FALSE, FALSE, 0);
    ctx->wifi_switch = GTK_SWITCH (wifi_switch);

    cell = add_row (GTK_GRID (grid), row++, "WiFi SSID");
    wifi_ssid = gtk_entry_new ();
    gtk_entry_set_width_chars (GTK_ENTRY (wifi_ssid), 24);
    gtk_widget_set_hexpand (wifi_ssid, TRUE);
    gtk_widget_set_tooltip_text (wifi_ssid,
            "Network name to associate with.  Leave blank to clear.");
    gtk_box_pack_start (GTK_BOX (cell), wifi_ssid, TRUE, TRUE, 0);
    ctx->wifi_ssid_entry = GTK_ENTRY (wifi_ssid);

    cell = add_row (GTK_GRID (grid), row++, "WiFi password");
    wifi_psk = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (wifi_psk), FALSE);
    gtk_entry_set_width_chars (GTK_ENTRY (wifi_psk), 24);
    gtk_widget_set_hexpand (wifi_psk, TRUE);
    gtk_widget_set_tooltip_text (wifi_psk,
            "WPA2 PSK for the network above.  Stored on the device in "
            "plain text -- not shared across the mesh.");
    add_password_reveal_icon (GTK_ENTRY (wifi_psk));
    gtk_box_pack_start (GTK_BOX (cell), wifi_psk, TRUE, TRUE, 0);
    ctx->wifi_psk_entry = GTK_ENTRY (wifi_psk);

    cell = add_row (GTK_GRID (grid), row++, "Ethernet");
    eth_switch = gtk_switch_new ();
    gtk_widget_set_halign (eth_switch, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (eth_switch,
            "Enable the on-board Ethernet PHY (only meaningful on "
            "boards that ship with one, e.g. T-ETH-Lite).");
    gtk_box_pack_start (GTK_BOX (cell), eth_switch, FALSE, FALSE, 0);
    ctx->eth_switch = GTK_SWITCH (eth_switch);

    cell = add_row (GTK_GRID (grid), row++, "IPv6");
    ipv6_switch = gtk_switch_new ();
    gtk_widget_set_halign (ipv6_switch, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (ipv6_switch,
            "Enable IPv6 alongside IPv4 on the connected interface.");
    gtk_box_pack_start (GTK_BOX (cell), ipv6_switch, FALSE, FALSE, 0);
    ctx->ipv6_switch = GTK_SWITCH (ipv6_switch);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply network settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The device "
            "re-handshakes after the write to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_NETWORK_CTX_QDATA,
                            ctx, network_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_network_set_state (GtkWidget         *page,
                                const PnMeshState *state,
                                PnMeshConnection  *connection)
{
    NetworkCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_NETWORK_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_network)
    {
        gtk_entry_set_text (ctx->wifi_ssid_entry, "");
        gtk_entry_set_text (ctx->wifi_psk_entry,  "");
        gtk_switch_set_active (ctx->wifi_switch, FALSE);
        gtk_switch_set_active (ctx->eth_switch,  FALSE);
        gtk_switch_set_active (ctx->ipv6_switch, FALSE);
        g_clear_pointer (&ctx->last_ntp_server,     g_free);
        g_clear_pointer (&ctx->last_rsyslog_server, g_free);
        g_clear_pointer (&ctx->last_ipv4_config,    g_bytes_unref);
        ctx->last_address_mode      = 0;
        ctx->last_enabled_protocols = 0;
        set_writing (ctx, FALSE);
        return;
    }

    g_free (ctx->last_ntp_server);
    ctx->last_ntp_server = g_strdup (state->net_ntp_server);
    g_free (ctx->last_rsyslog_server);
    ctx->last_rsyslog_server = g_strdup (state->net_rsyslog_server);
    ctx->last_address_mode      = state->net_address_mode;
    ctx->last_enabled_protocols = state->net_enabled_protocols;
    g_clear_pointer (&ctx->last_ipv4_config, g_bytes_unref);
    if (state->net_ipv4_config != NULL)
        ctx->last_ipv4_config = g_bytes_ref (state->net_ipv4_config);

    gtk_switch_set_active (ctx->wifi_switch, state->net_wifi_enabled);
    gtk_entry_set_text (ctx->wifi_ssid_entry,
                        state->net_wifi_ssid != NULL ? state->net_wifi_ssid : "");
    gtk_entry_set_text (ctx->wifi_psk_entry,
                        state->net_wifi_psk  != NULL ? state->net_wifi_psk  : "");
    gtk_switch_set_active (ctx->eth_switch,  state->net_eth_enabled);
    gtk_switch_set_active (ctx->ipv6_switch, state->net_ipv6_enabled);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_network_set_status_callback (GtkWidget               *page,
                                          PnMeshNetworkStatusFunc  callback,
                                          gpointer                 user_data)
{
    NetworkCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_NETWORK_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_network_set_busy_callback (GtkWidget          *page,
                                        PnMeshPageBusyFunc  callback,
                                        gpointer            user_data)
{
    NetworkCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_NETWORK_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
