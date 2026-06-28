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
/*  Store & Forward page — Phase 11 (TODO #48.2).                      */
/*                                                                     */
/*  Standalone form for StoreForwardConfig.  Hosted inside an expander */
/*  under the dialog's Mesh tools top tab.  Apply ships the whole sub- */
/*  block at once (the device replaces it as a unit, so omitting fields */
/*  would reset them to proto3 zero), then a verify-cycle handshake     */
/*  refreshes the controls from the device's authoritative reply.      */
/*                                                                     */
/*  The controls cascade: Enabled gates everything, Is server gates    */
/*  the server-side knobs, and only when this node is a server do the   */
/*  heartbeat / records / history-return fields mean anything.         */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-store-forward.h"
#include "pn-action-button.h"

#include "pn-device-spin.h"

#define PN_MESH_STORE_FORWARD_CTX_QDATA "pn-mesh-page-store-forward-ctx"

typedef struct
{
    /* Widget tree handles. */
    GtkSwitch     *enabled_switch;
    GtkSwitch     *is_server_switch;
    GtkSwitch     *heartbeat_switch;
    GtkSpinButton *records_spin;
    GtkSpinButton *history_max_spin;
    GtkSpinButton *history_window_spin;
    GtkButton     *apply_button;

    /* "Not yet streamed" placeholder shown until the device sends a
     * StoreForwardConfig block.  The form grid hides while the
     * placeholder is visible so the user sees one or the other. */
    GtkLabel      *unavailable_label;
    GtkWidget     *form_grid;
    GtkWidget     *apply_box;

    /* Borrowed; dialog owns it. */
    PnMeshConnection *connection;

    gboolean       writing;
    gboolean       have_data;

    PnMeshStoreForwardStatusFunc status_cb;
    gpointer                     status_ud;

    PnMeshPageBusyFunc           busy_cb;
    gpointer                     busy_ud;
} StoreForwardCtx;

static void
store_forward_ctx_free (gpointer data)
{
    g_slice_free (StoreForwardCtx, data);
}

static void
emit_status (StoreForwardCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* ------------------------------------------------------------------ */
/*  Sensitivity                                                         */
/* ------------------------------------------------------------------ */

static void
set_writing (StoreForwardCtx *ctx, gboolean writing)
{
    /* "Form is editable at all": needs a live connection, a streamed
     * config block, and no in-flight write.  The Enabled toggle and
     * Apply both follow this gate. */
    gboolean form_enable = !writing && ctx->connection != NULL && ctx->have_data;
    /* "Is server is meaningful": only when the module is enabled. */
    gboolean server_enable = form_enable
            && gtk_switch_get_active (ctx->enabled_switch);
    /* "Server-side knobs are meaningful": the node also has to be a
     * server.  A plain client buffers nothing, so paint those fields
     * disabled to match. */
    gboolean detail_enable = server_enable
            && gtk_switch_get_active (ctx->is_server_switch);
    gboolean transition = (ctx->writing != writing);

    ctx->writing = writing;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->enabled_switch),  form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),    form_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->is_server_switch), server_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->heartbeat_switch),     detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->records_spin),         detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->history_max_spin),     detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->history_window_spin),  detail_enable);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* Re-run the sensitivity calc when Enabled or Is server flips, so the
 * gated fields visibly follow.  Pure UI re-evaluation, no I/O. */
static void
on_gate_switch_toggled (GObject *src, GParamSpec *pspec, gpointer user_data)
{
    StoreForwardCtx *ctx = user_data;

    (void) src;
    (void) pspec;

    set_writing (ctx, ctx->writing);
}

/* ------------------------------------------------------------------ */
/*  Paint                                                               */
/* ------------------------------------------------------------------ */

static void
repaint (GtkWidget *page, const PnMeshState *state)
{
    StoreForwardCtx *ctx;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    if (state == NULL || !state->have_store_forward)
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

    gtk_switch_set_active     (ctx->enabled_switch,      state->sf_enabled);
    gtk_switch_set_active     (ctx->is_server_switch,    state->sf_is_server);
    gtk_switch_set_active     (ctx->heartbeat_switch,    state->sf_heartbeat);
    gtk_spin_button_set_value (ctx->records_spin,        state->sf_records);
    gtk_spin_button_set_value (ctx->history_max_spin,    state->sf_history_return_max);
    gtk_spin_button_set_value (ctx->history_window_spin, state->sf_history_return_window);

    set_writing (ctx, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_apply_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget       *page = user_data;
    StoreForwardCtx *ctx;
    GError          *error = NULL;
    gboolean         ok;

    (void) source;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    if (ctx == NULL)
    {
        pn_mesh_connection_set_store_forward_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_store_forward_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply Store & Forward settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* Verify-cycle re-handshake refreshed state in place. */
    repaint (page, pn_mesh_connection_get_state (ctx->connection));
    emit_status (ctx, "Store & Forward settings applied.");
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                     *page = user_data;
    StoreForwardCtx               *ctx;
    PnMeshStoreForwardConfigWrite  cfg;

    (void) button;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.enabled   = gtk_switch_get_active (ctx->enabled_switch);
    cfg.is_server = gtk_switch_get_active (ctx->is_server_switch);
    cfg.heartbeat = gtk_switch_get_active (ctx->heartbeat_switch);
    cfg.records   = (guint32) gtk_spin_button_get_value_as_int (ctx->records_spin);
    cfg.history_return_max =
            (guint32) gtk_spin_button_get_value_as_int (ctx->history_max_spin);
    cfg.history_return_window =
            (guint32) gtk_spin_button_get_value_as_int (ctx->history_window_spin);

    emit_status (ctx, "Applying Store & Forward settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_store_forward_async (
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

    /* Spin buttons share a common width via the size group so the
     * value column lines up.  A switch is naturally narrow and already
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

GtkWidget *
pn_mesh_page_store_forward_new (void)
{
    StoreForwardCtx *ctx;
    GtkWidget       *page;
    GtkWidget       *unavailable;
    GtkWidget       *grid;
    GtkWidget       *cell;
    GtkWidget       *w;
    GtkWidget       *apply_box;
    GtkWidget       *apply;
    gint             row = 0;

    ctx = g_slice_new0 (StoreForwardCtx);

    /* Hosted inside a GtkExpander, so the outer box wears small
     * margins -- the expander body already pads. */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    unavailable = gtk_label_new (
            "The device has not reported its Store & Forward "
            "configuration. Try reconnecting; if the message persists "
            "this firmware does not expose the module.");
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
            "Master switch for the Store & Forward module. Turn off to "
            "leave this node a plain participant that neither stores nor "
            "requests history.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->enabled_switch = GTK_SWITCH (w);
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_gate_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Is server",
            "Promote this node to the storing server: it buffers mesh "
            "traffic and replays it on request. Needs a mains-powered, "
            "always-on node. Leave off for a plain client that can only "
            "ask a server for history.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->is_server_switch = GTK_SWITCH (w);
    /* Server-side knobs below stay sensitive only while this node is a
     * server; flipping it off greys them out. */
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_gate_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Heartbeat",
            "Have the server periodically advertise its presence so "
            "clients can discover it. Server only.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->heartbeat_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Records",
            "How many messages the server keeps buffered. 0 lets the "
            "firmware pick a default sized to available memory. Server "
            "only.");
    w = pn_device_spin_new_with_range (0, 100000, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->records_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "History return max",
            "Maximum number of buffered messages the server returns in a "
            "single history request. Server only.");
    w = pn_device_spin_new_with_range (0, 100000, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->history_max_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "History return window",
            "How far back in time (minutes) a history request may reach. "
            "Messages older than this are not replayed. Server only.");
    w = pn_device_spin_new_with_range (0, 100000, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->history_window_spin = GTK_SPIN_BUTTON (w);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 12);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply Store & Forward settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);
    ctx->apply_box = apply_box;

    g_object_set_data_full (G_OBJECT (page),
                            PN_MESH_STORE_FORWARD_CTX_QDATA,
                            ctx, store_forward_ctx_free);

    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_store_forward_set_state (GtkWidget         *page,
                                      const PnMeshState *state,
                                      PnMeshConnection  *connection)
{
    StoreForwardCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    repaint (page, state);
}

void
pn_mesh_page_store_forward_set_status_callback (
        GtkWidget                     *page,
        PnMeshStoreForwardStatusFunc   callback,
        gpointer                       user_data)
{
    StoreForwardCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_store_forward_set_busy_callback (
        GtkWidget          *page,
        PnMeshPageBusyFunc  callback,
        gpointer            user_data)
{
    StoreForwardCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_STORE_FORWARD_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
