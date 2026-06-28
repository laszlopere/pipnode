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
/*  Position page -- the broadcast-behaviour half of PositionConfig.   */
/*                                                                     */
/*  Switches for fixed position + smart broadcast, spinners for the    */
/*  broadcast interval and the smart-broadcast distance / interval     */
/*  thresholds, a single Apply button at the bottom.                   */
/*                                                                     */
/*  PositionConfig is one protobuf message but its settings are split   */
/*  across two sibling pages: the GPS Settings page (above) owns the    */
/*  GPS-hardware fields (gps_mode, the GPIO pins, the update interval,  */
/*  the position-report flags), this page owns the broadcast-behaviour */
/*  fields.  Each page mirrors the OTHER page's fields at set_state-    */
/*  time and ships them back verbatim on Apply -- proto3-zeroing them   */
/*  would clobber the half the user did not touch.  After either Apply  */
/*  the connection re-handshakes and set_state runs on both pages.      */
/*                                                                     */
/*  gps_mode supersedes the deprecated gps_enabled boolean upstream;    */
/*  Apply syncs them (mode=ENABLED -> enabled=TRUE) so older firmware   */
/*  still sees a consistent pair.                                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-position.h"
#include "pn-action-button.h"

#include "pn-device-spin.h"

#define PN_MESH_POSITION_CTX_QDATA "pn-mesh-page-position-ctx"

typedef struct
{
    GtkSwitch       *fixed_position_switch;
    GtkSwitch       *smart_broadcast_switch;
    GtkSpinButton   *broadcast_secs_spin;
    GtkSpinButton   *smart_min_distance_spin;
    GtkSpinButton   *smart_min_interval_spin;
    GtkButton       *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* The GPS Settings page's fields, mirrored at set_state-time so this
     * page's Apply ships them back unchanged. */
    guint32          last_gps_mode;
    guint32          last_gps_update_interval;
    guint32          last_position_flags;
    guint32          last_rx_gpio;
    guint32          last_tx_gpio;
    guint32          last_gps_en_gpio;

    gboolean         writing;

    PnMeshPositionStatusFunc status_cb;
    gpointer                 status_ud;

    PnMeshPageBusyFunc       busy_cb;
    gpointer                 busy_ud;
} PositionCtx;

static void
position_ctx_free (gpointer data)
{
    g_slice_free (PositionCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Status + writing flag                                               */
/* ------------------------------------------------------------------ */

static void
emit_status (PositionCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (PositionCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->fixed_position_switch),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->smart_broadcast_switch), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->broadcast_secs_spin),    enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->smart_min_distance_spin),enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->smart_min_interval_spin),enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),           enable);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_position_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget   *page = user_data;
    PositionCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_POSITION_CTX_QDATA);
    GError      *error = NULL;
    gboolean     ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_position_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_position_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply position settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_position_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Position settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                  *page = user_data;
    PositionCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                                          PN_MESH_POSITION_CTX_QDATA);
    PnMeshPositionConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    /* Broadcast-behaviour fields owned by this page. */
    cfg.position_broadcast_smart_enabled =
            gtk_switch_get_active (ctx->smart_broadcast_switch);
    cfg.fixed_position = gtk_switch_get_active (ctx->fixed_position_switch);
    cfg.position_broadcast_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->broadcast_secs_spin);
    cfg.broadcast_smart_min_distance =
            gtk_spin_button_get_value_as_int (ctx->smart_min_distance_spin);
    cfg.broadcast_smart_min_interval_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->smart_min_interval_spin);

    /* GPS-hardware fields owned by the GPS Settings page -- shipped back
     * verbatim.  Keep the legacy gps_enabled boolean in sync with the
     * mirrored gps_mode (ENABLED -> TRUE). */
    cfg.gps_mode            = ctx->last_gps_mode;
    cfg.gps_enabled         = (ctx->last_gps_mode == 1);
    cfg.gps_update_interval = ctx->last_gps_update_interval;
    cfg.position_flags      = ctx->last_position_flags;
    cfg.rx_gpio             = ctx->last_rx_gpio;
    cfg.tx_gpio             = ctx->last_tx_gpio;
    cfg.gps_en_gpio         = ctx->last_gps_en_gpio;

    emit_status (ctx, "Applying position settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_position_config_async (
            ctx->connection, &cfg, NULL,
            on_set_position_config_done, page);
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

    /* Spin buttons share a common width via the size group so the value
     * column lines up.  A switch is naturally narrow and already
     * left-aligned (halign START), so leave it out -- otherwise the
     * size group stretches it to the widest control. */
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

/* Attach a small dim-label "unit suffix" after a control in @cell. */
static void
attach_unit (GtkWidget *cell, const gchar *unit)
{
    GtkWidget       *suffix = gtk_label_new (unit);
    GtkStyleContext *sc     = gtk_widget_get_style_context (suffix);
    gtk_style_context_add_class (sc, "dim-label");
    gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
}

GtkWidget *
pn_mesh_page_position_new (void)
{
    GtkWidget   *page;
    GtkWidget   *grid;
    GtkWidget   *cell;
    GtkWidget   *fixed_position;
    GtkWidget   *smart_broadcast;
    GtkWidget   *broadcast_secs;
    GtkWidget   *smart_min_distance;
    GtkWidget   *smart_min_interval;
    GtkWidget   *apply_box;
    GtkWidget   *apply;
    PositionCtx *ctx;
    gint         row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    ctx = g_slice_new0 (PositionCtx);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    cell = add_row (GTK_GRID (grid), row++, "Fixed position");
    fixed_position = gtk_switch_new ();
    gtk_widget_set_halign (fixed_position, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (fixed_position,
            "Treat the device's stored coordinates as authoritative "
            "and stop polling the GPS for updates.  Useful for a "
            "stationary base node.");
    gtk_box_pack_start (GTK_BOX (cell), fixed_position, FALSE, FALSE, 0);
    ctx->fixed_position_switch = GTK_SWITCH (fixed_position);

    cell = add_row (GTK_GRID (grid), row++, "Broadcast interval");
    /* 0 = device default; the firmware ships about 900 s (15 min) as
     * its default if unset. */
    broadcast_secs = pn_device_spin_new_with_range (0, 86400, 60);
    gtk_widget_set_tooltip_text (broadcast_secs,
            "Seconds between Position packets sent on the mesh.  "
            "0 lets the firmware pick its default (~15 min).");
    gtk_box_pack_start (GTK_BOX (cell), broadcast_secs, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->broadcast_secs_spin = GTK_SPIN_BUTTON (broadcast_secs);

    cell = add_row (GTK_GRID (grid), row++, "Smart broadcast");
    smart_broadcast = gtk_switch_new ();
    gtk_widget_set_halign (smart_broadcast, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (smart_broadcast,
            "Send a Position packet only when the node has moved at "
            "least the minimum distance below, or the minimum "
            "interval has elapsed.  Saves airtime on a busy mesh.");
    gtk_box_pack_start (GTK_BOX (cell), smart_broadcast, FALSE, FALSE, 0);
    ctx->smart_broadcast_switch = GTK_SWITCH (smart_broadcast);

    cell = add_row (GTK_GRID (grid), row++, "Smart minimum distance");
    smart_min_distance = pn_device_spin_new_with_range (0, 100000, 10);
    gtk_widget_set_tooltip_text (smart_min_distance,
            "Distance (in metres) the node must have moved before a "
            "smart-broadcast Position packet is sent.  Only used "
            "when Smart broadcast is on.");
    gtk_box_pack_start (GTK_BOX (cell), smart_min_distance, FALSE, FALSE, 0);
    attach_unit (cell, " m");
    ctx->smart_min_distance_spin = GTK_SPIN_BUTTON (smart_min_distance);

    cell = add_row (GTK_GRID (grid), row++, "Smart minimum interval");
    smart_min_interval = pn_device_spin_new_with_range (0, 86400, 30);
    gtk_widget_set_tooltip_text (smart_min_interval,
            "Minimum seconds between two smart-broadcast Position "
            "packets, regardless of movement.  Only used when "
            "Smart broadcast is on.");
    gtk_box_pack_start (GTK_BOX (cell), smart_min_interval, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->smart_min_interval_spin = GTK_SPIN_BUTTON (smart_min_interval);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply position settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_POSITION_CTX_QDATA,
                            ctx, position_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_position_set_state (GtkWidget         *page,
                                 const PnMeshState *state,
                                 PnMeshConnection  *connection)
{
    PositionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POSITION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_position)
    {
        ctx->last_gps_mode            = 0;
        ctx->last_gps_update_interval = 0;
        ctx->last_position_flags      = 0;
        ctx->last_rx_gpio             = 0;
        ctx->last_tx_gpio             = 0;
        ctx->last_gps_en_gpio         = 0;
        set_writing (ctx, FALSE);
        return;
    }

    /* Mirror the GPS Settings page's fields for verbatim round-trip. */
    ctx->last_gps_mode            = state->pos_gps_mode;
    ctx->last_gps_update_interval = state->pos_gps_update_interval;
    ctx->last_position_flags      = state->pos_position_flags;
    ctx->last_rx_gpio             = state->pos_rx_gpio;
    ctx->last_tx_gpio             = state->pos_tx_gpio;
    ctx->last_gps_en_gpio         = state->pos_gps_en_gpio;

    gtk_switch_set_active (ctx->fixed_position_switch,
                           state->pos_fixed_position);
    gtk_switch_set_active (ctx->smart_broadcast_switch,
                           state->pos_position_broadcast_smart_enabled);
    gtk_spin_button_set_value (ctx->broadcast_secs_spin,
                               (gdouble) state->pos_position_broadcast_secs);
    gtk_spin_button_set_value (ctx->smart_min_distance_spin,
                               (gdouble) state->pos_broadcast_smart_min_distance);
    gtk_spin_button_set_value (ctx->smart_min_interval_spin,
                               (gdouble) state->pos_broadcast_smart_min_interval_secs);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_position_set_status_callback (GtkWidget                *page,
                                           PnMeshPositionStatusFunc  callback,
                                           gpointer                  user_data)
{
    PositionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POSITION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_position_set_busy_callback (GtkWidget          *page,
                                         PnMeshPageBusyFunc  callback,
                                         gpointer            user_data)
{
    PositionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_POSITION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
